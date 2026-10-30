/* Test the C host's MMIO sequencing against independently generated native
 * fixtures. This is a register/slot model, not a cryptographic or RTL model.
 */
#define main aead_uio_program_main
#include "../aead_uio.c"
#undef main
#include <assert.h>

struct fixture {
    struct test_case test;
    const uint32_t *input, *input_control, *output, *output_control, *configuration;
    size_t input_count, output_count;
};

#include "vectors.h"

enum fault { NORMAL, BAD_DATA, BAD_META, BAD_AUTH, EARLY_DONE, STUCK, ERROR_FLAG, EXTRA_OUTPUT };

struct model {
    const struct fixture *fixture;
    const struct fixture *encrypt_fixture, *decrypt_fixture;
    uint32_t config[11], staging, pending_data, pending_control;
    size_t accepted, produced;
    bool active, pending, valid, done, error, read_data, read_metadata;
    unsigned start_delay, starts, resets, fail_on_start, status_reads;
    enum fault fault;
    void *tag_page;
    size_t tag_page_size;
};

static void tick(struct model *model)
{
    const struct fixture *fixture = model->fixture;
    size_t payload_words = aead_uio_words(fixture->test.payload_bytes);
    if (!model->active || model->fault == STUCK) {
        return;
    }
    if (model->start_delay) {
        --model->start_delay;
        return;
    }
    if (model->pending && !model->valid) {
        assert(model->accepted < fixture->input_count);
        assert(model->pending_data == fixture->input[model->accepted]);
        assert(model->pending_control == (PUSH | fixture->input_control[model->accepted]));
        ++model->accepted;
        model->pending = false;
    }
    if (!model->valid && model->produced < fixture->output_count) {
        if (model->produced < payload_words) {
            model->valid = model->accepted > aead_uio_section_words(fixture->test.aad_bytes) + model->produced;
        } else {
            model->valid = model->accepted == fixture->input_count;
        }
    }
    if (model->accepted == fixture->input_count && model->produced == fixture->output_count) {
        if (model->fault == EXTRA_OUTPUT) {
            model->valid = true;
        } else {
            model->done = true;
            model->active = false;
        }
    }
}

static uint32_t model_read(void *context, unsigned offset)
{
    struct model *model = context;
    const struct fixture *fixture = model->fixture;
    if (offset == STATUS) {
        uint32_t value;
        ++model->status_reads;
        tick(model);
        value = model->active ? BUSY : READY;
        if (model->active && !model->pending && !model->start_delay && model->fault != STUCK) {
            value |= INPUT_FREE;
        }
        if (model->done) {
            value |= DONE;
            if (model->tag_page) {
                assert(mprotect(model->tag_page, model->tag_page_size, PROT_READ | PROT_WRITE) == 0);
                model->tag_page = NULL;
            }
            if (fixture->test.auth_ok != (model->fault == BAD_AUTH)) {
                value |= AUTH_OK;
            }
        }
        if (model->valid) {
            value |= OUTPUT_VALID;
            value &= ~READY;
        }
        if (model->error || (model->active && model->fault == ERROR_FLAG)) {
            value |= ERROR;
        }
        if (model->active && model->fault == EARLY_DONE) {
            value |= DONE;
        }
        return value;
    }
    assert(model->valid);
    if (offset == OUTPUT_META) {
        model->read_metadata = true;
        return (META_VALID | (model->produced < fixture->output_count ?
                fixture->output_control[model->produced] : 4)) ^ (model->fault == BAD_META ? 0x80 : 0);
    }
    assert(offset == OUTPUT_DATA);
    model->read_data = true;
    return (model->produced < fixture->output_count ? fixture->output[model->produced] : 0) ^
           (model->fault == BAD_DATA ? 1 : 0);
}

static void model_write(void *context, unsigned offset, uint32_t value)
{
    struct model *model = context;
    if (offset >= KEY0 && offset < INPUT_DATA) {
        assert(!model->active);
        assert(value == model->fixture->configuration[(offset - KEY0) / 4]);
        model->config[(offset - KEY0) / 4] = value;
    } else if (offset == CONTROL && (value & SOFT_RESET)) {
        ++model->resets;
        model->active = model->pending = model->valid = model->done = model->error = false;
    } else if (offset == CONTROL) {
        if (model->encrypt_fixture != NULL) {
            model->fixture = (value & ENCRYPT) ? model->encrypt_fixture : model->decrypt_fixture;
        }
        assert(value == (START | (model->fixture->test.encrypt ? ENCRYPT : 0)));
        assert(!model->active && !model->valid);
        model->active = true;
        model->done = false;
        model->accepted = model->produced = 0;
        model->start_delay = 2;
        ++model->starts;
        if (model->starts == model->fail_on_start) {
            model->fault = BAD_DATA;
        }
    } else if (offset == INPUT_DATA) {
        model->staging = value;
    } else {
        assert(offset == STREAM_CTRL);
        if (value == POP) {
            assert(model->valid && model->read_data && model->read_metadata);
            model->valid = model->read_data = model->read_metadata = false;
            ++model->produced;
        } else {
            assert(value & PUSH);
            assert(model->active && !model->pending && !model->start_delay);
            model->pending_data = model->staging;
            model->pending_control = value;
            model->pending = true;
        }
    }
}

int main(void)
{
    size_t index;
    struct options options = {"model", "all", 5000, false};
    {
        struct aead_uio_device device = {0};
        int stdin_flags = fcntl(STDIN_FILENO, F_GETFD);
        aead_uio_close(&device);
        assert(fcntl(STDIN_FILENO, F_GETFD) == stdin_flags);
        aead_uio_close(&device);
        aead_uio_close(NULL);
        assert(aead_uio_open(NULL, "/dev/uio12") == -1);
        assert(aead_uio_open(&device, NULL) == -1);
        aead_uio_close(&device);
    }
    {
        const char *states[] = {"operating\n", "operating", "unknown\n", "", "operating\nextra"};
        size_t i;
        for (i = 0; i < sizeof(states) / sizeof(states[0]); ++i) {
            FILE *state = tmpfile();
            assert(state != NULL && fputs(states[i], state) >= 0);
            rewind(state);
            assert(aead_uio_fpga_operating(state) == (i < 2));
            fclose(state);
        }
    }
    for (index = 0; index < sizeof(fixtures) / sizeof(fixtures[0]); ++index) {
        struct model model = {0};
        struct io bus = {model_read, model_write, &model};
        model.fixture = &fixtures[index];
        assert(aead_uio_run_case(&bus, &model.fixture->test, &options) == 0);
        assert(model.starts == 1 && model.resets == 0);
        assert(model.accepted == model.fixture->input_count);
        assert(model.produced == model.fixture->output_count);
    }
    /* The public API must never expose speculative plaintext, even though the
     * model produces it before the final tag. Independent fixtures cover RFC,
     * empty sections, partial words and a multi-block payload. */
    for (index = 0; index < sizeof(fixtures) / sizeof(fixtures[0]); ++index) {
        struct model model = {0};
        struct io bus = {model_read, model_write, &model};
        const struct test_case *test = &fixtures[index].test;
        uint8_t *out = malloc(test->payload_bytes ? test->payload_bytes : 1);
        uint8_t tag[16], original[16];
        size_t byte;
        int expected = !test->encrypt && !test->auth_ok ? -2 : 0;
        model.fixture = &fixtures[index];
        assert(out);
        memset(out, 0xa5, test->payload_bytes);
        memcpy(tag, test->tag, 16);
        memcpy(original, tag, 16);
        assert(aead_uio_crypt(&bus, test->encrypt, test->key, test->nonce,
                              test->aad, test->aad_bytes, test->input, test->payload_bytes,
                              tag, out, 5000) == expected);
        if (!expected) {
            assert(!memcmp(out, test->expected, test->payload_bytes));
            assert(!memcmp(tag, test->tag, 16));
        }
        else {
            for (byte = 0; byte < test->payload_bytes; ++byte) {
                assert(out[byte] == 0xa5);
            }
            assert(!memcmp(tag, original, 16));
            assert(model.resets == 1);
        }
        free(out);
    }
    {
        /* An expected bad-tag rejection is reported by the caller, without
         * misleading FAIL or session-aborted messages from the UIO layer. */
        struct model model = {0};
        struct io bus = {model_read, model_write, &model};
        const struct test_case *bad = &fixtures[2].test;
        FILE *log = tmpfile();
        uint8_t output[114], tag[16];
        int saved_stderr = dup(STDERR_FILENO);
        size_t i;
        assert(log && saved_stderr >= 0 && fflush(stderr) == 0);
        assert(dup2(fileno(log), STDERR_FILENO) >= 0);
        model.fixture = &fixtures[2];
        memset(output, 0xa5, sizeof(output));
        memcpy(tag, bad->tag, sizeof(tag));
        assert(aead_uio_crypt(&bus, 0, bad->key, bad->nonce, bad->aad, bad->aad_bytes,
                              bad->input, bad->payload_bytes, tag, output, 5000) == -2);
        assert(fflush(stderr) == 0 && dup2(saved_stderr, STDERR_FILENO) >= 0);
        close(saved_stderr);
        assert(fseek(log, 0, SEEK_END) == 0 && ftell(log) == 0);
        fclose(log);
        assert(model.resets == 1);
        for (i = 0; i < sizeof(output); ++i) assert(output[i] == 0xa5);
        model.fixture = &fixtures[1];
        memcpy(tag, model.fixture->test.tag, sizeof(tag));
        assert(aead_uio_crypt(&bus, 0, model.fixture->test.key, model.fixture->test.nonce,
                              model.fixture->test.aad, model.fixture->test.aad_bytes,
                              model.fixture->test.input, model.fixture->test.payload_bytes,
                              tag, output, 5000) == 0);
        assert(!memcmp(output, model.fixture->test.expected, sizeof(output)));
    }
    {
        /* Encrypt's tag is output-only. Keep its page inaccessible until DONE;
         * even a speculative read of the caller's tag must fault this test. */
        struct model model = {0};
        struct io bus = {model_read, model_write, &model};
        const struct test_case *test = &fixtures[0].test;
        FILE *backing = tmpfile();
        long page_size = sysconf(_SC_PAGESIZE);
        uint8_t out[114], *tag;
        assert(backing && page_size >= 16);
        assert(ftruncate(fileno(backing), page_size) == 0);
        tag = mmap(NULL, (size_t)page_size, PROT_NONE, MAP_SHARED, fileno(backing), 0);
        assert(tag != MAP_FAILED);
        model.fixture = &fixtures[0];
        model.tag_page = tag;
        model.tag_page_size = (size_t)page_size;
        assert(aead_uio_crypt(&bus, 1, test->key, test->nonce, test->aad, test->aad_bytes,
                              test->input, test->payload_bytes, tag, out, 5000) == 0);
        assert(!memcmp(out, test->expected, sizeof(out)));
        assert(!memcmp(tag, test->tag, 16));
        assert(munmap(tag, (size_t)page_size) == 0);
        fclose(backing);
    }
    {
        struct model model = {0};
        struct io bus = {model_read, model_write, &model};
        const struct test_case *test = &fixtures[1].test;
        uint8_t out[114], tag[16];
        size_t byte;
        model.fixture = &fixtures[1];
        memcpy(tag, test->tag, 16);
        memset(out, 0xa5, sizeof(out));
        model.fault = STUCK;
        assert(aead_uio_crypt(&bus, 0, test->key, test->nonce, test->aad, test->aad_bytes,
                              test->input, test->payload_bytes, tag, out, 1) == -1);
        assert(model.resets == 1);
        for (byte = 0; byte < sizeof(out); ++byte) assert(out[byte] == 0xa5);
        model.fault = NORMAL;
        assert(aead_uio_crypt(&bus, 0, test->key, test->nonce, test->aad, test->aad_bytes,
                              test->input, test->payload_bytes, tag, out, 5000) == 0);
        assert(!memcmp(out, test->expected, sizeof(out)));
        model.error = true;
        assert(aead_uio_crypt(&bus, 0, test->key, test->nonce, test->aad, test->aad_bytes,
                              test->input, test->payload_bytes, tag, out, 1) == -1);
        assert(model.resets == 1); /* Preserve preexisting error, do not steal ownership. */
        assert(aead_uio_crypt(&bus, 0, test->key, test->nonce, NULL, 1,
                              test->input, test->payload_bytes, tag, out, 1) == -1);
    }
    for (index = BAD_META; index <= EXTRA_OUTPUT; ++index) {
        struct model model = {0};
        struct io bus = {model_read, model_write, &model};
        const struct test_case *test = &fixtures[1].test;
        uint8_t out[114], tag[16];
        size_t byte;
        model.fixture = &fixtures[1];
        model.fault = (enum fault)index;
        memcpy(tag, test->tag, 16);
        memset(out, 0xa5, sizeof(out));
        assert(aead_uio_crypt(&bus, 0, test->key, test->nonce, test->aad, test->aad_bytes,
                              test->input, test->payload_bytes, tag, out, index == STUCK ? 1 : 100) ==
               (index == BAD_AUTH ? -2 : -1));
        assert(model.resets == 1 && !memcmp(tag, test->tag, 16));
        for (byte = 0; byte < sizeof(out); ++byte) assert(out[byte] == 0xa5);
        model.fault = NORMAL;
        assert(aead_uio_crypt(&bus, 0, test->key, test->nonce, test->aad, test->aad_bytes,
                              test->input, test->payload_bytes, tag, out, 5000) == 0);
        assert(!memcmp(out, test->expected, sizeof(out)));
    }
    {
        struct model model = {0};
        struct io bus = {model_read, model_write, &model};
        const struct test_case *test = &fixtures[1].test;
        uint8_t out[114], tag[16];
        model.fixture = &fixtures[1];
        model.active = true;
        model.fault = STUCK;
        memcpy(tag, test->tag, sizeof(tag));
        assert(aead_uio_crypt(&bus, 0, test->key, test->nonce, test->aad, test->aad_bytes,
                              test->input, test->payload_bytes, tag, out, 10) == -1);
        assert(model.starts == 0 && model.resets == 0 && model.active);
        assert(model.status_reads <= 20); /* Do not hammer an already-busy device. */
    }
    /* Invalid data, metadata, auth status and missing progress must fail and
     * abort only the session started by this client. */
    for (index = BAD_DATA; index <= EXTRA_OUTPUT; ++index) {
        struct model model = {0};
        struct io bus = {model_read, model_write, &model};
        model.fixture = &fixtures[1];
        model.fault = (enum fault)index;
        options.timeout_ms = 2;
        assert(aead_uio_run_case(&bus, &model.fixture->test, &options) == 1);
        assert(model.starts == 1 && model.resets == 1);
    }
    {
        struct model model = {0};
        struct io bus = {model_read, model_write, &model};
        model.fixture = &fixtures[0];
        model.active = true;
        model.fault = STUCK;
        assert(aead_uio_run_case(&bus, &model.fixture->test, &options) == 1);
        assert(model.starts == 0 && model.resets == 0);
    }
    /* Exercise the real benchmark driver with independent encrypt/decrypt
     * fixtures: capture preparation, authentication, warm-up and replay. */
    options.timeout_ms = 5000;
    for (index = 3; index < sizeof(fixtures) / sizeof(fixtures[0]); index += 2) {
        struct model model = {0};
        struct io bus = {model_read, model_write, &model};
        struct benchmark_options benchmark = {true, fixtures[index].test.payload_bytes,
            fixtures[index].test.aad_bytes, 2, 1};
        model.fixture = model.encrypt_fixture = &fixtures[index];
        model.decrypt_fixture = &fixtures[index + 1];
        assert(run_benchmark(&bus, &options, &benchmark) == 0);
        assert(model.starts == 8 && model.resets == 0);
    }
    {
        struct model model = {0};
        struct io bus = {model_read, model_write, &model};
        struct benchmark_options benchmark = {true, fixtures[5].test.payload_bytes,
            fixtures[5].test.aad_bytes, 2, 0};
        model.fixture = model.encrypt_fixture = &fixtures[5];
        model.decrypt_fixture = &fixtures[6];
        options.test = "encrypt";
        assert(run_benchmark(&bus, &options, &benchmark) == 0);
        assert(model.starts == 4 && model.resets == 0);
        options.test = "decrypt";
        assert(run_benchmark(&bus, &options, &benchmark) == 0);
        assert(model.starts == 8 && model.resets == 0);
        model.fault = BAD_AUTH;
        assert(run_benchmark(&bus, &options, &benchmark) == 1);
        assert(model.resets == 1);
    }
    {
        struct model model = {0};
        struct io bus = {model_read, model_write, &model};
        struct benchmark_options benchmark = {true, fixtures[5].test.payload_bytes,
            fixtures[5].test.aad_bytes, 2, 1};
        model.fixture = model.encrypt_fixture = &fixtures[5];
        model.decrypt_fixture = &fixtures[6];
        model.fail_on_start = 4; /* Two preparation sessions, one warm-up. */
        options.test = "encrypt";
        assert(run_benchmark(&bus, &options, &benchmark) == 1);
        assert(model.starts == 4 && model.resets == 1);
    }
    puts("PASS UIO host MMIO sequencing, byte order, framing and error recovery model");
    return 0;
}
