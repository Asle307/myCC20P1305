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
    unsigned start_delay, starts, resets, fail_on_start;
    enum fault fault;
};

static void tick(struct model *model)
{
    const struct fixture *fixture = model->fixture;
    size_t payload_words = words(fixture->test.payload_bytes);
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
            model->valid = model->accepted > section_words(fixture->test.aad_bytes) + model->produced;
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
        tick(model);
        value = model->active ? BUSY : READY;
        if (model->active && !model->pending && !model->start_delay && model->fault != STUCK) {
            value |= INPUT_FREE;
        }
        if (model->done) {
            value |= DONE;
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
    for (index = 0; index < sizeof(fixtures) / sizeof(fixtures[0]); ++index) {
        struct model model = {0};
        struct io bus = {model_read, model_write, &model};
        model.fixture = &fixtures[index];
        assert(run_case(&bus, &model.fixture->test, &options) == 0);
        assert(model.starts == 1 && model.resets == 0);
        assert(model.accepted == model.fixture->input_count);
        assert(model.produced == model.fixture->output_count);
    }
    /* Invalid data, metadata, auth status and missing progress must fail and
     * abort only the session started by this client. */
    for (index = BAD_DATA; index <= EXTRA_OUTPUT; ++index) {
        struct model model = {0};
        struct io bus = {model_read, model_write, &model};
        model.fixture = &fixtures[1];
        model.fault = (enum fault)index;
        options.timeout_ms = 2;
        assert(run_case(&bus, &model.fixture->test, &options) == 1);
        assert(model.starts == 1 && model.resets == 1);
    }
    {
        struct model model = {0};
        struct io bus = {model_read, model_write, &model};
        model.fixture = &fixtures[0];
        model.active = true;
        model.fault = STUCK;
        assert(run_case(&bus, &model.fixture->test, &options) == 1);
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
