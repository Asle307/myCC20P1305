#define _POSIX_C_SOURCE 200809L

/* Shared Linux UIO transport for the 16-register AEAD AXI-Lite peripheral.
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
#include <sys/file.h>
#include "aead_uio_internal.h"
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

uint64_t aead_uio_nanoseconds(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        fprintf(stderr, "[ERROR] clock_gettime: %s\n", strerror(errno));
        exit(2);
    }
    return (uint64_t)now.tv_sec * UINT64_C(1000000000) + (uint64_t)now.tv_nsec;
}

static uint64_t milliseconds(void)
{
    return aead_uio_nanoseconds() / UINT64_C(1000000);
}

static void wait_for_progress(void)
{
    const struct timespec delay = {0, 1000000};
    nanosleep(&delay, NULL);
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

size_t aead_uio_words(size_t count)
{
    return (count + 3) / 4;
}

size_t aead_uio_section_words(size_t count)
{
    return count == 0 ? 1 : aead_uio_words(count);
}

static void input_word(const struct test_case *test, size_t index,
                       uint32_t *data, uint32_t *control)
{
    const uint8_t *bytes;
    size_t length, offset, count;
    *control = 0;
    if (index < aead_uio_section_words(test->aad_bytes)) {
        bytes = test->aad;
        length = test->aad_bytes;
        *control = IS_AAD;
    } else {
        index -= aead_uio_section_words(test->aad_bytes);
        if (index < aead_uio_section_words(test->payload_bytes)) {
            bytes = test->input;
            length = test->payload_bytes;
        } else {
            index -= aead_uio_section_words(test->payload_bytes);
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
    if (index >= aead_uio_words(length)) {
        index -= aead_uio_words(length);
        bytes = test->tag;
        length = 16;
        *metadata |= IS_TAG;
    }
    offset = index * 4;
    count = length - offset;
    if (count > 4) {
        count = 4;
    }
    if (data != NULL) {
        *data = bytes ? pack_word(bytes + offset, count) : 0;
    }
    *metadata |= (uint32_t)count | (offset + count == length ? LAST : 0);
}

/* Diagnostics compare fixture output; the public API captures privately and
 * publishes output only after completion and successful authentication. */
static int transfer_impl(const struct io *bus, const struct test_case *test,
                         const struct options *options, uint8_t *capture,
                         uint8_t *capture_tag, bool quiet, bool *auth_failed)
{
    size_t sent = 0, received = 0, index;
    size_t input_count = aead_uio_section_words(test->aad_bytes) + aead_uio_section_words(test->payload_bytes) +
                         (test->encrypt ? 0 : 4);
    size_t output_count = aead_uio_words(test->payload_bytes) + (test->encrypt ? 4 : 0);
    uint64_t deadline = milliseconds() + options->timeout_ms;
    uint32_t status = 0;
    bool started = false;
    int result = 1;

    /* Do not abort an existing session just because another client left it busy. */
    for (;;) {
        status = bus->read(bus->context, STATUS);
        if (status & ERROR) {
            fprintf(stderr, "[ERROR] FAIL %s: sticky ERROR before START (STATUS=0x%08" PRIx32 ")\n",
                    test->name, status);
            goto out;
        }
        if (status & READY) {
            break;
        }
        if (milliseconds() >= deadline) {
            fprintf(stderr, "[ERROR] FAIL %s: timeout waiting for START_READY (STATUS=0x%08" PRIx32 ")\n",
                    test->name, status);
            goto out;
        }
        wait_for_progress();
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
        uint32_t value, metadata, expected = 0, expected_metadata;
        bool progress = false;
        status = bus->read(bus->context, STATUS);
        if (status & ERROR) {
            fprintf(stderr, "[ERROR] FAIL %s: wrapper ERROR (STATUS=0x%08" PRIx32 ")\n", test->name, status);
            goto out;
        }
        /* Always drain available output before queuing more input. Decrypt
         * plaintext is compared privately; nothing is released to an app. */
        if (status & OUTPUT_VALID) {
            metadata = bus->read(bus->context, OUTPUT_META);
            value = bus->read(bus->context, OUTPUT_DATA);
            if (received >= output_count) {
                fprintf(stderr, "[ERROR] FAIL %s: extra output word\n", test->name);
                goto out;
            }
            /* Capture mode has no expected data, and encrypt's tag is an
             * output-only buffer that must not be read before completion. */
            expected_word(test, received, capture == NULL ? &expected : NULL, &expected_metadata);
            if ((capture == NULL && value != expected) || metadata != expected_metadata) {
                if (capture != NULL) {
                    /* Application capture can contain unauthenticated plaintext.
                     * Diagnostics must never print its data words on failure. */
                    fprintf(stderr, "[ERROR] FAIL %s: output metadata at word %zu: %08" PRIx32
                            "/%08" PRIx32 " (actual/expected)\n",
                            test->name, received, metadata, expected_metadata);
                }
                else {
                    fprintf(stderr, "[ERROR] FAIL %s: output word %zu, data %08" PRIx32 "/%08" PRIx32
                            ", metadata %08" PRIx32 "/%08" PRIx32 " (actual/expected)\n",
                            test->name, received, value, expected, metadata, expected_metadata);
                }
                goto out;
            }
            if (capture != NULL) {
                size_t byte, offset = received * 4;
                uint8_t *destination = capture;
                if (metadata & IS_TAG) {
                    destination = capture_tag;
                    offset = (received - aead_uio_words(test->payload_bytes)) * 4;
                }
                for (byte = 0; byte < (metadata & 7u); ++byte) {
                    destination[offset + byte] = (uint8_t)(value >> (8 * byte));
                }
            }
            bus->write(bus->context, STREAM_CTRL, POP);
            ++received;
            progress = true;
        }
        if ((status & INPUT_FREE) && sent < input_count) {
            input_word(test, sent, &value, &metadata);
            bus->write(bus->context, INPUT_DATA, value);
            bus->write(bus->context, STREAM_CTRL, metadata);
            ++sent;
            progress = true;
        }
        if (status & DONE) {
            if (auth_failed && !test->encrypt && !(status & AUTH_OK) &&
                sent == input_count && received == output_count && !(status & BUSY)) {
                *auth_failed = true;
            }
            if (sent != input_count || received != output_count ||
                (status & BUSY) || !!(status & AUTH_OK) != test->auth_ok) {
                /* A complete decrypt with AUTH_OK=0 is an expected API
                 * rejection, not a transport failure or incomplete message. */
                if (!auth_failed || !*auth_failed) {
                    fprintf(stderr, "[ERROR] FAIL %s: DONE with incomplete data or unexpected AUTH_OK "
                            "(STATUS=0x%08" PRIx32 ", sent=%zu/%zu, received=%zu/%zu)\n",
                            test->name, status, sent, input_count, received, output_count);
                }
                goto out;
            }
            status = bus->read(bus->context, STATUS);
            if ((status & (READY | DONE)) != (READY | DONE) ||
                (status & (BUSY | OUTPUT_VALID | ERROR)) || !!(status & AUTH_OK) != test->auth_ok) {
                fprintf(stderr, "[ERROR] FAIL %s: final status not held (STATUS=0x%08" PRIx32 ")\n",
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
            fprintf(stderr, "[ERROR] FAIL %s: timeout (STATUS=0x%08" PRIx32
                    ", sent=%zu/%zu, received=%zu/%zu)\n", test->name, status,
                    sent, input_count, received, output_count);
            goto out;
        }
        if (!progress) {
            wait_for_progress();
        }
    }
out:
    if (result != 0 && started) {
        bus->write(bus->context, CONTROL, SOFT_RESET | (test->encrypt ? ENCRYPT : 0));
        if (!auth_failed || !*auth_failed) {
            fprintf(stderr, "[ERROR] UIO operation aborted with SOFT_RESET.\n");
        }
    }
    return result;
}

int aead_uio_transfer_case(const struct io *bus, const struct test_case *test,
                  const struct options *options, uint8_t *capture,
                  uint8_t *capture_tag, bool quiet)
{
    return transfer_impl(bus, test, options, capture, capture_tag, quiet, NULL);
}

int aead_uio_run_case(const struct io *bus, const struct test_case *test,
                    const struct options *options)
{
    return aead_uio_transfer_case(bus, test, options, NULL, NULL, false);
}

static int read_sysfs_number(const char *device, const char *field, uint64_t *value)
{
    char path[256], buffer[128], *end;
    const char *name = strrchr(device, '/');
    FILE *file;
    name = name == NULL ? device : name + 1;
    if (strncmp(name, "uio", 3) != 0 || name[3] == '\0' || strspn(name + 3, "0123456789") != strlen(name + 3)) {
        fprintf(stderr, "[ERROR] Expected /dev/uioN, got %s\n", device);
        return -1;
    }
    if (snprintf(path, sizeof(path), "/sys/class/uio/%s/maps/map0/%s", name, field) >= (int)sizeof(path)) {
        return -1;
    }
    file = fopen(path, "r");
    if (file == NULL) {
        fprintf(stderr, "[ERROR] %s: %s\n", path, strerror(errno));
        return -1;
    }
    if (fgets(buffer, sizeof(buffer), file) == NULL) {
        fclose(file);
        fprintf(stderr, "[ERROR] Cannot read %s\n", path);
        return -1;
    }
    fclose(file);
    errno = 0;
    *value = strtoull(buffer, &end, 0);
    if (errno != 0 || end == buffer || (*end != '\n' && *end != '\0')) {
        fprintf(stderr, "[ERROR] Invalid number in %s\n", path);
        return -1;
    }
    return 0;
}


static void wipe(void *memory, size_t length)
{
    volatile uint8_t *bytes = memory;
    while (length--) {
        *bytes++ = 0;
    }
}

/* The manager reports its own programming state, not the PL clock or bus
 * health. Reject unconfirmed state before even opening the UIO device. */
int aead_uio_fpga_operating(FILE *state)
{
    char value[32];
    size_t length = fread(value, 1, sizeof(value), state);
    return !ferror(state) &&
           ((length == 10 && memcmp(value, "operating\n", 10) == 0) ||
            (length == 9 && memcmp(value, "operating", 9) == 0));
}

int aead_uio_crypt(const struct io *bus, int encrypt,
                   const uint8_t key[32], const uint8_t nonce[12],
                   const uint8_t *aad, size_t aad_length,
                   const uint8_t *input, size_t length,
                   uint8_t tag[16], uint8_t *out, unsigned timeout_ms)
{
    uint8_t *private_output, private_tag[16] = {0};
    struct options options = {NULL, NULL, timeout_ms, false};
    struct test_case test = {"AEAD API", key, nonce, aad, input, NULL, tag,
        aad_length, length, encrypt != 0, encrypt == 0};
    int result;
    bool auth_failed = false;
    if (!bus || !bus->read || !bus->write || !key || !nonce || !tag ||
        (length && (!input || !out)) || (aad_length && !aad) ||
        length > 16777216 || aad_length > 1048576 || !timeout_ms || timeout_ms > 600000) {
        return -1;
    }
    private_output = malloc(length ? length : 1);
    if (!private_output) {
        return -1;
    }
    result = transfer_impl(bus, &test, &options, private_output, private_tag, true, &auth_failed);
    if (!result) {
        if (length) {
            memcpy(out, private_output, length);
        }
        if (encrypt) {
            memcpy(tag, private_tag, 16);
        }
    }
    wipe(private_output, length);
    wipe(private_tag, sizeof(private_tag));
    free(private_output);
    return result ? (auth_failed ? -2 : -1) : 0;
}

int aead_uio_open(struct aead_uio_device *device, const char *path)
{
    uint64_t base, size, offset;
    struct stat info;
    FILE *state;
    int operating;
    long page = sysconf(_SC_PAGESIZE);
    if (!device) {
        return -1;
    }
    memset(device, 0, sizeof(*device));
    device->fd = -1;
    if (!path || !path[0]) {
        return -1;
    }
    state = fopen("/sys/class/fpga_manager/fpga0/state", "r");
    if (!state) {
        fprintf(stderr, "[ERROR] Cannot verify FPGA manager state; refusing UIO access.\n");
        return -1;
    }
    operating = aead_uio_fpga_operating(state);
    fclose(state);
    if (!operating) {
        fprintf(stderr, "[ERROR] FPGA manager is not operating; refusing UIO access.\n");
        return -1;
    }
    if (page <= 0 || read_sysfs_number(path, "addr", &base) ||
        read_sysfs_number(path, "size", &size) || read_sysfs_number(path, "offset", &offset)) {
        return -1;
    }
    if (base != UINT64_C(0xa0000000) || size < (uint64_t)page || offset) {
        fprintf(stderr, "[ERROR] UIO map0 must start at 0xA0000000, cover one page and have offset 0\n");
        return -1;
    }
    device->fd = open(path, O_RDWR | O_SYNC | O_CLOEXEC);
    if (device->fd < 0) {
        fprintf(stderr, "[ERROR] %s: %s\n", path, strerror(errno));
        return -1;
    }
    device->owns_fd = 1;
    if (fstat(device->fd, &info) || !S_ISCHR(info.st_mode) ||
        flock(device->fd, LOCK_EX | LOCK_NB)) {
        fprintf(stderr, "[ERROR] UIO must be an available character device\n");
        aead_uio_close(device);
        return -1;
    }
    device->mapping_size = (size_t)page;
    device->mapping = mmap(NULL, (size_t)page, PROT_READ | PROT_WRITE, MAP_SHARED, device->fd, 0);
    if (device->mapping == MAP_FAILED) {
        device->mapping = NULL;
        fprintf(stderr, "[ERROR] UIO mmap: %s\n", strerror(errno));
        aead_uio_close(device);
        return -1;
    }
    device->bus = (struct io){read_register, write_register, device->mapping};
    return 0;
}

void aead_uio_close(struct aead_uio_device *device)
{
    if (!device) {
        return;
    }
    if (device->mapping) {
        munmap(device->mapping, device->mapping_size);
    }
    if (device->owns_fd && device->fd >= 0) {
        close(device->fd);
    }
    memset(device, 0, sizeof(*device));
    device->fd = -1;
}
