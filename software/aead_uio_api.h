#ifndef AEAD_UIO_API_H
#define AEAD_UIO_API_H

#include <stddef.h>
#include <stdint.h>

/* One caller at a time. Callback buses support register-model verification. */
struct io {
    uint32_t (*read)(void *context, unsigned offset);
    void (*write)(void *context, unsigned offset, uint32_t value);
    void *context;
};

struct aead_uio_device {
    struct io bus;
    void *mapping;
    size_t mapping_size;
    int fd, owns_fd;
};

/* Require fpga0 manager state "operating", validate map0 at 0xa0000000,
 * map one page, take an exclusive advisory lock. The manager state alone
 * cannot prove that the correct bitstream, clock or bus is ready.
 * All UIO clients must cooperate; this cannot exclude uncooperative clients. */
int aead_uio_open(struct aead_uio_device *device, const char *path);
/* Safe on a zero-initialized or already closed device. Close before reopening. */
void aead_uio_close(struct aead_uio_device *device);

/* Return 0 on success, -2 on authentication rejection, -1 on other failures.
 * out has length bytes; tag is 16 bytes, input for decrypt, output for encrypt.
 * out and tag remain unchanged on failure. In-place input/output is supported;
 * tag must not overlap out. NULL data/AAD/out are allowed for zero lengths.
 * Limits: payload 16 MiB, AAD 1 MiB, timeout 1..600000 ms per operation.
 * Tentative output remains private until DONE, final READY and (decrypt)
 * AUTH_OK. A failed started operation is soft-reset; preexisting busy/error
 * state is never reset. Polling pauses for 1 ms when no progress is possible.
 * Timeout cannot interrupt a stalled MMIO access. Key registers are not
 * zeroized by SOFT_RESET. */
int aead_uio_crypt(const struct io *bus, int encrypt,
                   const uint8_t key[32], const uint8_t nonce[12],
                   const uint8_t *aad, size_t aad_length,
                   const uint8_t *input, size_t length,
                   uint8_t tag[16], uint8_t *out, unsigned timeout_ms);

#endif
