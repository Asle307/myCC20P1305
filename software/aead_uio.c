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

#include "aead_uio_internal.h"

struct benchmark_options {
    bool enabled;
    size_t payload_bytes, aad_bytes;
    unsigned iterations, warmup;
};

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
    if (aead_uio_transfer_case(bus, &encrypt, options, cipher, tag, true) ||
        aead_uio_transfer_case(bus, &decrypt, options, NULL, NULL, true)) {
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
            if (aead_uio_transfer_case(bus, test, options, NULL, NULL, true)) {
                goto out;
            }
        }
        begin = aead_uio_nanoseconds();
        for (iteration = 0; iteration < benchmark->iterations; ++iteration) {
            if (aead_uio_transfer_case(bus, test, options, NULL, NULL, true)) {
                fprintf(stderr, "Benchmark failed at %s iteration %u; no rate for this mode.\n",
                        name, iteration + 1);
                goto out;
            }
        }
        elapsed = aead_uio_nanoseconds() - begin;
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
    struct aead_uio_device device;
    uint8_t key[32], nonce[12], aad[12], ciphertext[sizeof(plaintext) - 1], tag[16], bad_tag[16];
    int option, result = 0, index;

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
    if (aead_uio_open(&device, options.device) != 0) {
        return 2;
    }
    bus = device.bus;
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
    printf("UIO %s, base 0x%08" PRIx64 "; RFC 8439 functional test\n", options.device, UINT64_C(0xa0000000));
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
        result = aead_uio_run_case(&bus, &test, &options);
        if (result != 0) {
            break;
        }
    }
    if (result == 0 && benchmark.enabled) {
        result = run_benchmark(&bus, &options, &benchmark);
    }
cleanup:
    aead_uio_close(&device);
    return result;
}
