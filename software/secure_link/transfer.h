#ifndef SECURE_LINK_TRANSFER_H
#define SECURE_LINK_TRANSFER_H
#include "protocol.h"

typedef int (*sl_send_fn)(void *, uint8_t, const void *, size_t);
enum sl_transfer_state { SL_IDLE, SL_TX_OFFER, SL_TX_ACK, SL_TX_COMPLETE, SL_RX };
struct sl_transfer {
    enum sl_transfer_state state;
    int directory, file;
    char name[129], temporary[64];
    uint64_t size, count, progressed;
    unsigned last_percent;
    sl_send_fn send;
    void *context;
};
int sl_safe_name(const uint8_t *name, size_t length);
int sl_transfer_init(struct sl_transfer *, const char *directory, sl_send_fn, void *);
void sl_transfer_clear(struct sl_transfer *);
/* A local refusal returns 1 (session remains usable), fatal error returns -1. */
int sl_transfer_offer(struct sl_transfer *, const char *path);
int sl_transfer_handle(struct sl_transfer *, const struct sl_frame *);
#endif
