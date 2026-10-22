#define _POSIX_C_SOURCE 200809L

/* Linux UIO host for the 16-register AEAD AXI-Lite peripheral.
 * Same UIO mmap boundary as VJU_SoC, with no DMA or Xilinx BSP dependency.
 * Only one process may own the peripheral while this program runs.
 */
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

enum {
    CONTROL = 0x00, STATUS = 0x04, KEY0 = 0x08, NONCE0 = 0x28,
    INPUT_DATA = 0x34, STREAM_CTRL = 0x38, OUTPUT_META = 0x38, OUTPUT_DATA = 0x3c,
    START = 1u << 0, ENCRYPT = 1u << 1, SOFT_RESET = 1u << 3,
    READY = 1u << 0, BUSY = 1u << 1, DONE = 1u << 2, AUTH_OK = 1u << 3,
    INPUT_FREE = 1u << 4, OUTPUT_VALID = 1u << 5, ERROR = 1u << 6,
    IS_AAD = 1u << 8, IS_TAG = 1u << 8, LAST = 1u << 9,
    PUSH = 1u << 16, POP = 1u << 17, META_VALID = 1u << 16
};

struct io {
    uint32_t (*read)(void *context, unsigned offset);
    void (*write)(void *context, unsigned offset, uint32_t value);
    void *context;
};

struct test_case {
    const char *name;
    const uint8_t *key, *nonce, *aad, *input, *expected, *tag;
    size_t aad_bytes, payload_bytes;
    bool encrypt, auth_ok;
};

struct options {
    const char *device;
    const char *test;
    unsigned timeout_ms;
    bool pause;
};

struct benchmark_options {
    bool enabled;
    size_t payload_bytes, aad_bytes;
    unsigned iterations, warmup;
};

static uint64_t nanoseconds(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        perror("clock_gettime");
        exit(2);
    }
    return (uint64_t)now.tv_sec * UINT64_C(1000000000) + (uint64_t)now.tv_nsec;
}

static uint64_t milliseconds(void)
{
    return nanoseconds() / UINT64_C(1000000);
}

static uint32_t read_register(void *context, unsigned offset)
{
    volatile uint32_t *registers = context;
    uint32_t value = registers[offset / 4];
    __sync_synchronize();
    return value;
}

static void write_register(void *context, unsigned offset, uint32_t value)
{
    volatile uint32_t *registers = context;
    registers[offset / 4] = value;
    /* ARM Device-MMIO accesses must not reorder INPUT_DATA behind PUSH. */
    __sync_synchronize();
}

static uint32_t pack_word(const uint8_t *bytes, size_t count)
{
    uint32_t value = 0;
    size_t index;
    for (index = 0; index < count; ++index) {
        value |= (uint32_t)bytes[index] << (8 * index);
    }
    return value;
}

static size_t words(size_t count)
{
    return (count + 3) / 4;
}

static size_t section_words(size_t count)
{
    return count == 0 ? 1 : words(count);
}

static void input_word(const struct test_case *test, size_t index,
                       uint32_t *data, uint32_t *control)
{
    const uint8_t *bytes;
    size_t length, offset, count;
    *control = 0;
    if (index < section_words(test->aad_bytes)) {
        bytes = test->aad;
        length = test->aad_bytes;
        *control = IS_AAD;
    } else {
        index -= section_words(test->aad_bytes);
        if (index < section_words(test->payload_bytes)) {
            bytes = test->input;
            length = test->payload_bytes;
        } else {
            index -= section_words(test->payload_bytes);
            bytes = test->tag;
            length = 16;
        }
    }
    offset = index * 4;
    count = length > offset ? length - offset : 0;
    if (count > 4) {
        count = 4;
    }
    *data = count == 0 ? 0 : pack_word(bytes + offset, count);
    *control |= PUSH | (uint32_t)count | (offset + count == length ? LAST : 0);
}

static void expected_word(const struct test_case *test, size_t index,
                          uint32_t *data, uint32_t *metadata)
{
    const uint8_t *bytes = test->expected;
    size_t length = test->payload_bytes, offset, count;
    *metadata = META_VALID;
    if (index >= words(length)) {
        index -= words(length);
        bytes = test->tag;
        length = 16;
        *metadata |= IS_TAG;
    }
    offset = index * 4;
    count = length - offset;
    if (count > 4) {
        count = 4;
    }
    *data = pack_word(bytes + offset, count);
    *metadata |= (uint32_t)count | (offset + count == length ? LAST : 0);
}

/* Capture is used only for the untimed synthetic encrypt preparation. All
 * timed transfers compare every output word, metadata field and auth status. */
static int transfer_case(const struct io *bus, const struct test_case *test,
                         const struct options *options, uint8_t *capture,
                         uint8_t *capture_tag, bool quiet)
{
    size_t sent = 0, received = 0, index;
    size_t input_count = section_words(test->aad_bytes) + section_words(test->payload_bytes) +
                         (test->encrypt ? 0 : 4);
    size_t output_count = words(test->payload_bytes) + (test->encrypt ? 4 : 0);
    uint64_t deadline = milliseconds() + options->timeout_ms;
    uint32_t status = 0;
    bool started = false;
    int result = 1;

    /* Do not abort an existing session just because another client left it busy. */
    for (;;) {
        status = bus->read(bus->context, STATUS);
        if (status & ERROR) {
            fprintf(stderr, "FAIL %s: sticky ERROR before START (STATUS=0x%08" PRIx32 ")\n",
                    test->name, status);
            goto out;
        }
        if (status & READY) {
            break;
        }
        if (milliseconds() >= deadline) {
            fprintf(stderr, "FAIL %s: timeout waiting for START_READY (STATUS=0x%08" PRIx32 ")\n",
                    test->name, status);
            goto out;
        }
    }
    for (index = 0; index < 8; ++index) {
        bus->write(bus->context, KEY0 + (unsigned)index * 4, pack_word(test->key + index * 4, 4));
    }
    for (index = 0; index < 3; ++index) {
        bus->write(bus->context, NONCE0 + (unsigned)index * 4, pack_word(test->nonce + index * 4, 4));
    }
    if (options->pause) {
        int character;
        printf("%s ready. Arm ILA, then press Enter to issue START...", test->name);
        fflush(stdout);
        do {
            character = getchar();
            if (character == EOF) {
                fprintf(stderr, "\nCancelled before START.\n");
                goto out;
            }
        } while (character != '\n');
    }
    deadline = milliseconds() + options->timeout_ms;
    bus->write(bus->context, CONTROL, START | (test->encrypt ? ENCRYPT : 0));
    started = true;
    for (;;) {
        uint32_t value, metadata, expected, expected_metadata;
        status = bus->read(bus->context, STATUS);
        if (status & ERROR) {
            fprintf(stderr, "FAIL %s: wrapper ERROR (STATUS=0x%08" PRIx32 ")\n", test->name, status);
            goto out;
        }
        /* Always drain available output before queuing more input. Decrypt
         * plaintext is compared privately; nothing is released to an app. */
        if (status & OUTPUT_VALID) {
            metadata = bus->read(bus->context, OUTPUT_META);
            value = bus->read(bus->context, OUTPUT_DATA);
            if (received >= output_count) {
                fprintf(stderr, "FAIL %s: extra output word\n", test->name);
                goto out;
            }
            expected_word(test, received, &expected, &expected_metadata);
            if ((capture == NULL && value != expected) || metadata != expected_metadata) {
                fprintf(stderr, "FAIL %s: output word %zu, data %08" PRIx32 "/%08" PRIx32
                        ", metadata %08" PRIx32 "/%08" PRIx32 " (actual/expected)\n",
                        test->name, received, value, expected, metadata, expected_metadata);
                goto out;
            }
            if (capture != NULL) {
                size_t byte, offset = received * 4;
                uint8_t *destination = capture;
                if (metadata & IS_TAG) {
                    destination = capture_tag;
                    offset = (received - words(test->payload_bytes)) * 4;
                }
                for (byte = 0; byte < (metadata & 7u); ++byte) {
                    destination[offset + byte] = (uint8_t)(value >> (8 * byte));
                }
            }
            bus->write(bus->context, STREAM_CTRL, POP);
            ++received;
        }
        if ((status & INPUT_FREE) && sent < input_count) {
            input_word(test, sent, &value, &metadata);
            bus->write(bus->context, INPUT_DATA, value);
            bus->write(bus->context, STREAM_CTRL, metadata);
            ++sent;
        }
        if (status & DONE) {
            if (sent != input_count || received != output_count ||
                (status & BUSY) || !!(status & AUTH_OK) != test->auth_ok) {
                fprintf(stderr, "FAIL %s: DONE with incomplete data or unexpected AUTH_OK "
                        "(STATUS=0x%08" PRIx32 ", sent=%zu/%zu, received=%zu/%zu)\n",
                        test->name, status, sent, input_count, received, output_count);
                goto out;
            }
            status = bus->read(bus->context, STATUS);
            if ((status & (READY | DONE)) != (READY | DONE) ||
                (status & (BUSY | OUTPUT_VALID | ERROR)) || !!(status & AUTH_OK) != test->auth_ok) {
                fprintf(stderr, "FAIL %s: final status not held (STATUS=0x%08" PRIx32 ")\n",
                        test->name, status);
                goto out;
            }
            if (!quiet) {
                printf("PASS %s: %zu payload bytes, AUTH_OK=%u\n",
                       test->name, test->payload_bytes, (unsigned)test->auth_ok);
            }
            result = 0;
            break;
        }
        if (milliseconds() >= deadline) {
            fprintf(stderr, "FAIL %s: timeout (STATUS=0x%08" PRIx32
                    ", sent=%zu/%zu, received=%zu/%zu)\n", test->name, status,
                    sent, input_count, received, output_count);
            goto out;
        }
    }
out:
    if (result != 0 && started) {
        bus->write(bus->context, CONTROL, SOFT_RESET | (test->encrypt ? ENCRYPT : 0));
        fprintf(stderr, "Session aborted with SOFT_RESET.\n");
    }
    return result;
}

static int run_case(const struct io *bus, const struct test_case *test,
                    const struct options *options)
{
    return transfer_case(bus, test, options, NULL, NULL, false);
}

static int run_benchmark(const struct io *bus, const struct options *options,
                         const struct benchmark_options *benchmark)
{
    uint8_t key[32], nonce[12], tag[16] = {0};
    uint8_t *plain = NULL, *cipher = NULL, *aad = NULL;
    size_t index;
    unsigned mode, iteration;
    int result = 1;
    struct test_case encrypt, decrypt;

    /* Public synthetic inputs only. The same key/nonce is intentionally reused
     * to replay a checked fixture; this is not an application encryption API. */
    plain = malloc(benchmark->payload_bytes ? benchmark->payload_bytes : 1);
    cipher = calloc(benchmark->payload_bytes ? benchmark->payload_bytes : 1, 1);
    aad = malloc(benchmark->aad_bytes ? benchmark->aad_bytes : 1);
    if (plain == NULL || cipher == NULL || aad == NULL) {
        fprintf(stderr, "Cannot allocate benchmark buffers\n");
        result = 2;
        goto out;
    }
    for (index = 0; index < sizeof(key); ++index) {
        key[index] = (uint8_t)index;
    }
    for (index = 0; index < sizeof(nonce); ++index) {
        nonce[index] = (uint8_t)index;
    }
    for (index = 0; index < benchmark->payload_bytes; ++index) {
        plain[index] = (uint8_t)index;
    }
    for (index = 0; index < benchmark->aad_bytes; ++index) {
        aad[index] = (uint8_t)index;
    }
    encrypt = (struct test_case){"benchmark encrypt", key, nonce, aad, plain, cipher, tag,
        benchmark->aad_bytes, benchmark->payload_bytes, true, false};
    decrypt = (struct test_case){"benchmark decrypt", key, nonce, aad, cipher, plain, tag,
        benchmark->aad_bytes, benchmark->payload_bytes, false, true};

    printf("Benchmark: %zu payload bytes, %zu AAD bytes, %u iterations, %u warm-up/mode\n",
           benchmark->payload_bytes, benchmark->aad_bytes, benchmark->iterations, benchmark->warmup);
    puts("Preparing synthetic fixture and checking encrypt/decrypt round trip (untimed)...");
    fflush(stdout);
    if (transfer_case(bus, &encrypt, options, cipher, tag, true) ||
        transfer_case(bus, &decrypt, options, NULL, NULL, true)) {
        goto out;
    }
    puts("Measured: Linux/UIO end-to-end, including per-message key/nonce setup,\n"
         "AXI-Lite transfers, polling and full output verification. Payload bytes only.\n"
         "Fixture preparation, RFC preflight, warm-up and printing are excluded.");
    for (mode = 0; mode < 2; ++mode) {
        const struct test_case *test = mode == 0 ? &encrypt : &decrypt;
        const char *name = mode == 0 ? "encrypt" : "decrypt";
        uint64_t begin, elapsed, total_bytes;
        double seconds, mbps;
        if (strcmp(options->test, "all") != 0 && strcmp(options->test, name) != 0) {
            continue;
        }
        printf("Running %s...\n", name);
        fflush(stdout);
        for (iteration = 0; iteration < benchmark->warmup; ++iteration) {
            if (transfer_case(bus, test, options, NULL, NULL, true)) {
                goto out;
            }
        }
        begin = nanoseconds();
        for (iteration = 0; iteration < benchmark->iterations; ++iteration) {
            if (transfer_case(bus, test, options, NULL, NULL, true)) {
                fprintf(stderr, "Benchmark failed at %s iteration %u; no rate for this mode.\n",
                        name, iteration + 1);
                goto out;
            }
        }
        elapsed = nanoseconds() - begin;
        if (elapsed == 0) {
            fprintf(stderr, "Timer interval is zero; increase --iterations\n");
            goto out;
        }
        total_bytes = (uint64_t)benchmark->payload_bytes * benchmark->iterations;
        seconds = (double)elapsed / 1e9;
        mbps = (double)total_bytes / seconds / 1e6;
        printf("BENCH %-7s: bytes=%" PRIu64 ", messages=%u, elapsed=%.6f s\n"
               "  %.3f MB/s | %.3f MiB/s | %.3f Mbit/s | %.3f us/message | %.1f messages/s\n",
               name, total_bytes, benchmark->iterations, seconds, mbps,
               (double)total_bytes / seconds / 1048576.0, mbps * 8.0,
               seconds * 1e6 / benchmark->iterations, benchmark->iterations / seconds);
        fflush(stdout);
    }
    result = 0;
out:
    free(aad);
    free(cipher);
    free(plain);
    return result;
}

static int decode_hex(const char *text, uint8_t *bytes, size_t length)
{
    size_t index;
    if (strlen(text) != length * 2) {
        return -1;
    }
    for (index = 0; index < length; ++index) {
        unsigned value;
        if (sscanf(text + index * 2, "%2x", &value) != 1) {
            return -1;
        }
        bytes[index] = (uint8_t)value;
    }
    return 0;
}

static int read_sysfs_number(const char *device, const char *field, uint64_t *value)
{
    char path[256], buffer[128], *end;
    const char *name = strrchr(device, '/');
    FILE *file;
    name = name == NULL ? device : name + 1;
    if (strncmp(name, "uio", 3) != 0 || name[3] == '\0' || strspn(name + 3, "0123456789") != strlen(name + 3)) {
        fprintf(stderr, "Expected /dev/uioN, got %s\n", device);
        return -1;
    }
    if (snprintf(path, sizeof(path), "/sys/class/uio/%s/maps/map0/%s", name, field) >= (int)sizeof(path)) {
        return -1;
    }
    file = fopen(path, "r");
    if (file == NULL) {
        perror(path);
        return -1;
    }
    if (fgets(buffer, sizeof(buffer), file) == NULL) {
        fclose(file);
        fprintf(stderr, "Cannot read %s\n", path);
        return -1;
    }
    fclose(file);
    errno = 0;
    *value = strtoull(buffer, &end, 0);
    if (errno != 0 || end == buffer || (*end != '\n' && *end != '\0')) {
        fprintf(stderr, "Invalid number in %s\n", path);
        return -1;
    }
    return 0;
}

static void usage(const char *program)
{
    printf("Usage: %s [--device /dev/uio12] [--test all|encrypt|decrypt|bad-tag]\n"
           "       [--timeout-ms 5000] [--pause]\n"
           "       %s --bench [--device /dev/uio12] [--test all|encrypt|decrypt]\n"
           "       [--bytes 16384] [--aad-bytes 12] [--iterations 100] [--warmup 3]\n"
           "       [--timeout-ms 5000]\n"
           "Default: RFC 8439 tests. --pause waits before each START for ILA.\n"
           "--bench: verified Linux/UIO payload throughput; no RTL cycle counter.\n"
           "--bytes: 0..16777216; --aad-bytes: 0..1048576;\n"
           "--iterations: 1..1000000; --warmup: 0..1000000. Decimal integers.\n"
           "Timeout applies to each message, not the entire benchmark.\n", program, program);
}

static int parse_number(const char *text, const char *name, unsigned minimum,
                        unsigned maximum, unsigned *result)
{
    char *end;
    unsigned long value;
    errno = 0;
    value = strtoul(text, &end, 10);
    if (errno || text[0] == '\0' || strspn(text, "0123456789") != strlen(text) ||
        *end || value < minimum || value > maximum) {
        fprintf(stderr, "--%s must be %u..%u (decimal integer)\n", name, minimum, maximum);
        return -1;
    }
    *result = (unsigned)value;
    return 0;
}

int main(int argc, char **argv)
{
    enum { BENCH = 1000, BYTES, AAD_BYTES, ITERATIONS, WARMUP };
    static const struct option arguments[] = {
        {"device", required_argument, NULL, 'd'}, {"test", required_argument, NULL, 't'},
        {"timeout-ms", required_argument, NULL, 'm'}, {"pause", no_argument, NULL, 'p'},
        {"bench", no_argument, NULL, BENCH}, {"bytes", required_argument, NULL, BYTES},
        {"aad-bytes", required_argument, NULL, AAD_BYTES},
        {"iterations", required_argument, NULL, ITERATIONS}, {"warmup", required_argument, NULL, WARMUP},
        {"help", no_argument, NULL, 'h'}, {NULL, 0, NULL, 0}
    };
    static const uint8_t plaintext[] = "Ladies and Gentlemen of the class of '99: If I could offer you "
                                       "only one tip for the future, sunscreen would be it.";
    static const char ciphertext_hex[] =
        "d31a8d34648e60db7b86afbc53ef7ec2a4aded51296e08fea9e2b5a736ee62d6"
        "3dbea45e8ca9671282fafb69da92728b1a71de0a9e060b2905d6a5b67ecd3b36"
        "92ddbd7f2d778b8c9803aee328091b58fab324e4fad675945585808b4831d7bc"
        "3ff4def08e4b7a9de576d26586cec64b6116";
    struct options options = {"/dev/uio12", "all", 5000, false};
    struct benchmark_options benchmark = {false, 16384, 12, 100, 3};
    bool benchmark_arguments = false;
    struct io bus;
    struct stat info;
    uint8_t key[32], nonce[12], aad[12], ciphertext[sizeof(plaintext) - 1], tag[16], bad_tag[16];
    uint64_t base, size, map_offset;
    void *mapping;
    long page_size;
    int option, fd, result = 0, index;

    while ((option = getopt_long(argc, argv, "d:t:m:ph", arguments, NULL)) != -1) {
        unsigned value;
        switch (option) {
        case 'd': options.device = optarg; break;
        case 't': options.test = optarg; break;
        case 'p': options.pause = true; break;
        case 'm':
            if (parse_number(optarg, "timeout-ms", 1, 600000, &options.timeout_ms)) {
                return 2;
            }
            break;
        case BENCH: benchmark.enabled = true; break;
        case BYTES:
        case AAD_BYTES:
        case ITERATIONS:
        case WARMUP:
            benchmark_arguments = true;
            if (parse_number(optarg, option == BYTES ? "bytes" : option == AAD_BYTES ? "aad-bytes" :
                             option == ITERATIONS ? "iterations" : "warmup",
                             option == ITERATIONS ? 1 : 0,
                             option == BYTES ? 16777216 : option == AAD_BYTES ? 1048576 : 1000000, &value)) {
                return 2;
            }
            if (option == BYTES) benchmark.payload_bytes = value;
            if (option == AAD_BYTES) benchmark.aad_bytes = value;
            if (option == ITERATIONS) benchmark.iterations = value;
            if (option == WARMUP) benchmark.warmup = value;
            break;
        case 'h': usage(argv[0]); return 0;
        default: usage(argv[0]); return 2;
        }
    }
    if (optind != argc || (strcmp(options.test, "all") && strcmp(options.test, "encrypt") &&
                           strcmp(options.test, "decrypt") && strcmp(options.test, "bad-tag"))) {
        usage(argv[0]);
        return 2;
    }
    if ((benchmark_arguments && !benchmark.enabled) ||
        (benchmark.enabled && (options.pause || strcmp(options.test, "bad-tag") == 0))) {
        fprintf(stderr, "Benchmark options require --bench; --bench cannot use --pause or --test bad-tag\n");
        return 2;
    }
    page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0 || read_sysfs_number(options.device, "addr", &base) ||
        read_sysfs_number(options.device, "size", &size) ||
        read_sysfs_number(options.device, "offset", &map_offset)) {
        return 2;
    }
    if (base != UINT64_C(0xa0000000) || size < (uint64_t)page_size || map_offset != 0) {
        fprintf(stderr, "UIO map0 must start at 0xA0000000, cover one page and have offset 0\n");
        return 2;
    }
    fd = open(options.device, O_RDWR | O_SYNC);
    if (fd < 0) {
        perror(options.device);
        return 2;
    }
    if (fstat(fd, &info) != 0 || !S_ISCHR(info.st_mode)) {
        fprintf(stderr, "%s is not a UIO character device\n", options.device);
        close(fd);
        return 2;
    }
    mapping = mmap(NULL, (size_t)page_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mapping == MAP_FAILED) {
        perror("mmap");
        close(fd);
        return 2;
    }
    bus.read = read_register;
    bus.write = write_register;
    bus.context = mapping;
    for (index = 0; index < 32; ++index) {
        key[index] = (uint8_t)(0x80 + index);
    }
    if (decode_hex("070000004041424344454647", nonce, sizeof(nonce)) ||
        decode_hex("50515253c0c1c2c3c4c5c6c7", aad, sizeof(aad)) ||
        decode_hex(ciphertext_hex, ciphertext, sizeof(ciphertext)) ||
        decode_hex("1ae10b594f09e26a7e902ecbd0600691", tag, sizeof(tag))) {
        fprintf(stderr, "Invalid built-in RFC fixture\n");
        result = 2;
        goto cleanup;
    }
    memcpy(bad_tag, tag, sizeof(tag));
    bad_tag[0] ^= 1;
    printf("UIO %s, base 0x%08" PRIx64 "; RFC 8439 functional test\n", options.device, base);
    for (index = 0; index < 3; ++index) {
        const char *names[] = {"encrypt", "decrypt", "bad-tag"};
        const char *labels[] = {"RFC encrypt", "RFC decrypt", "RFC decrypt bad tag (rejected)"};
        struct test_case test = {labels[index], key, nonce, aad,
            index == 0 ? plaintext : ciphertext, index == 0 ? ciphertext : plaintext,
            index == 2 ? bad_tag : tag, sizeof(aad), sizeof(ciphertext), index == 0, index == 1};
        /* Benchmark always gates measurement on all three independent RFC tests. */
        if (!benchmark.enabled && strcmp(options.test, "all") != 0 && strcmp(options.test, names[index]) != 0) {
            continue;
        }
        result = run_case(&bus, &test, &options);
        if (result != 0) {
            break;
        }
    }
    if (result == 0 && benchmark.enabled) {
        result = run_benchmark(&bus, &options, &benchmark);
    }
cleanup:
    munmap(mapping, (size_t)page_size);
    close(fd);
    return result;
}
