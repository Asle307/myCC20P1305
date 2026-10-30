#ifndef AEAD_UIO_INTERNAL_H
#define AEAD_UIO_INTERNAL_H
/* Diagnostic-only internals. Applications must use aead_uio_api.h. */
#include "aead_uio_api.h"
#include <stdbool.h>
#include <stdio.h>
enum {
    CONTROL = 0x00, STATUS = 0x04, KEY0 = 0x08, NONCE0 = 0x28,
    INPUT_DATA = 0x34, STREAM_CTRL = 0x38, OUTPUT_META = 0x38, OUTPUT_DATA = 0x3c,
    START = 1u << 0, ENCRYPT = 1u << 1, SOFT_RESET = 1u << 3,
    READY = 1u << 0, BUSY = 1u << 1, DONE = 1u << 2, AUTH_OK = 1u << 3,
    INPUT_FREE = 1u << 4, OUTPUT_VALID = 1u << 5, ERROR = 1u << 6,
    IS_AAD = 1u << 8, IS_TAG = 1u << 8, LAST = 1u << 9,
    PUSH = 1u << 16, POP = 1u << 17, META_VALID = 1u << 16
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


uint64_t aead_uio_nanoseconds(void);
int aead_uio_fpga_operating(FILE *state);
size_t aead_uio_words(size_t count);
size_t aead_uio_section_words(size_t count);
int aead_uio_transfer_case(const struct io *, const struct test_case *, const struct options *,
                  uint8_t *, uint8_t *, bool);
int aead_uio_run_case(const struct io *, const struct test_case *, const struct options *);
#endif
