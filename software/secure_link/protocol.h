#ifndef SECURE_LINK_PROTOCOL_H
#define SECURE_LINK_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>
#include "../aead_uio_api.h"

#define SL_HEADER 24
#define SL_CHUNK 4096
#define SL_WIRE (SL_HEADER + SL_CHUNK + 16)
#define SL_FILE_LIMIT (10u * 1024u * 1024u)

enum sl_type {
    SL_CHAT = 1, SL_OFFER, SL_READY, SL_REFUSE, SL_CHUNK_DATA, SL_ACK,
    SL_END, SL_COMPLETE, SL_RECEIPT, SL_CLOSE, SL_PING, SL_PONG
};

struct sl_session {
    uint8_t tx_key[32], rx_key[32];
    uint64_t tx_seq, rx_seq;
    int role, established, verbose;
    const struct io *hardware; /* Required for role 0 (KV260) in production. */
    unsigned timeout_ms;
};

struct sl_frame {
    uint8_t type;
    uint64_t sequence;
    size_t length;
    uint8_t data[SL_CHUNK];
};

struct sl_decoder {
    uint8_t wire[SL_WIRE];
    size_t used, target;
    uint64_t started;
};

uint64_t sl_now(void);
void sl_put64(uint8_t *out, uint64_t value);
uint64_t sl_get64(const uint8_t *in);
int sl_nonblocking(int fd);
/* Deadlines cover the entire operation, including partial I/O and EINTR. */
int sl_io(int fd, void *buffer, size_t length, int writing, unsigned timeout_ms);
int sl_psk_create(const char *path);
int sl_psk_load(const char *path, uint8_t key[32]);
/* role 1 initiates, role 0 responds; clears all keys/state on failure. */
int sl_handshake(int fd, int role, const uint8_t psk[32], struct sl_session *session);
int sl_software_crypt(int encrypt, const uint8_t key[32], const uint8_t nonce[12],
                      const uint8_t *aad, size_t aad_length, const uint8_t *input,
                      size_t length, uint8_t tag[16], uint8_t *output);
int sl_encode(struct sl_session *, uint8_t type, const void *, size_t,
              uint8_t wire[SL_WIRE], size_t *wire_length);
/* -2: authentication rejection, -3: authenticated replay/out-of-order,
 * -1: fatal framing/backend failure.
 * Neither failure changes rx_seq nor the caller's plaintext frame. */
int sl_decode(struct sl_session *, const uint8_t *, size_t, struct sl_frame *);
int sl_header_length(const uint8_t header[SL_HEADER], size_t *length);
void sl_session_clear(struct sl_session *);

#endif
