#define _POSIX_C_SOURCE 200809L
#include "transfer.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <openssl/rand.h>

int sl_safe_name(const uint8_t *name, size_t length)
{
    size_t i;
    if (!length || length > 128 || name[0] == '.') {
        return 0;
    }
    for (i = 0; i < length; ++i) {
        uint8_t c = name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.')) {
            return 0;
        }
    }
    return 1;
}

static void reset_file(struct sl_transfer *t)
{
    if (t->file >= 0) {
        close(t->file);
    }
    if (t->temporary[0]) {
        if (unlinkat(t->directory, t->temporary, 0) && errno != ENOENT) {
            fprintf(stderr, "[ERROR] Incomplete temporary file removal failed: %s\n", strerror(errno));
        }
    }
    t->file = -1;
    t->temporary[0] = 0;
    t->state = SL_IDLE;
    t->count = t->size = 0;
    t->last_percent = 0;
}

int sl_transfer_init(struct sl_transfer *t, const char *directory, sl_send_fn send, void *context)
{
    struct stat st;
    memset(t, 0, sizeof(*t));
    t->file = t->directory = -1;
    t->send = send;
    t->context = context;
    t->directory = open(directory, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (t->directory < 0) {
        return -1;
    }
    if (fstat(t->directory, &st) || st.st_uid != geteuid() || (st.st_mode & 022)) {
        fprintf(stderr, "[ERROR] Receive directory must be owned by this user and not group/world writable\n");
        sl_transfer_clear(t);
        return -1;
    }
    return 0;
}

void sl_transfer_clear(struct sl_transfer *t)
{
    reset_file(t);
    if (t->directory >= 0) {
        close(t->directory);
    }
    t->directory = -1;
}

static void progress(struct sl_transfer *t, const char *verb)
{
    unsigned percent = t->size ? (unsigned)(t->count * 100 / t->size) : 100;
    t->progressed = sl_now();
    if (!t->count || t->count == t->size || percent >= t->last_percent + 5) {
        printf("[FILE] %s %s: %" PRIu64 "/%" PRIu64 " bytes (%u%%)\n",
               verb, t->name, t->count, t->size, percent);
        t->last_percent = percent;
    }
}

int sl_transfer_offer(struct sl_transfer *t, const char *path)
{
    uint8_t offer[8 + 128];
    struct stat st;
    const char *name = strrchr(path, '/');
    name = name ? name + 1 : path;
    if (t->state != SL_IDLE) {
        puts("[WAIT] File transfer busy; try again after completion.");
        return 1;
    }
    if (!sl_safe_name((const uint8_t *)name, strlen(name))) {
        puts("[ERROR] Unsafe filename: use 1..128 ASCII letters, digits, dot, dash or underscore; no leading dot.");
        return 1;
    }
    t->file = open(path, O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
    if (t->file < 0 || fstat(t->file, &st) || !S_ISREG(st.st_mode) ||
        st.st_size < 0 || st.st_size > SL_FILE_LIMIT) {
        puts("[ERROR] Cannot send file: require a readable regular file of at most 10 MiB, without a final symlink.");
        reset_file(t);
        return 1;
    }
    strcpy(t->name, name);
    t->size = (uint64_t)st.st_size;
    t->count = 0;
    t->state = SL_TX_OFFER;
    sl_put64(offer, t->size);
    memcpy(offer + 8, name, strlen(name));
    progress(t, "Offering");
    return t->send(t->context, SL_OFFER, offer, 8 + strlen(name));
}

static int next_chunk(struct sl_transfer *t)
{
    uint8_t buffer[SL_CHUNK];
    size_t n;
    if (t->count > t->size) {
        return -1;
    }
    n = (size_t)(t->size - t->count);
    if (n > sizeof(buffer)) {
        n = sizeof(buffer);
    }
    if (!n) {
        ssize_t extra;
        do {
            extra = read(t->file, buffer, 1);
        } while (extra < 0 && errno == EINTR);
        if (extra != 0) {
            fprintf(stderr, "[ERROR] Source file changed size during transfer\n");
            return -1;
        }
        sl_put64(buffer, t->count);
        t->state = SL_TX_COMPLETE;
        return t->send(t->context, SL_END, buffer, 8);
    }
    {
        size_t remaining = n;
        uint8_t *cursor = buffer;
        while (remaining) {
            ssize_t count = read(t->file, cursor, remaining);
            if (count < 0 && errno == EINTR) {
                continue;
            }
            if (count <= 0 || (size_t)count > remaining) {
                fprintf(stderr, "[ERROR] Source file truncated or unreadable\n");
                return -1;
            }
            cursor += count;
            remaining -= (size_t)count;
        }
    }
    t->count += n;
    t->state = SL_TX_ACK;
    t->progressed = sl_now();
    return t->send(t->context, SL_CHUNK_DATA, buffer, n);
}

static int refuse(struct sl_transfer *t)
{
    return t->send(t->context, SL_REFUSE, NULL, 0);
}

static int receive_offer(struct sl_transfer *t, const struct sl_frame *f)
{
    uint8_t random[12];
    struct stat st;
    size_t i;
    if (f->length < 9 || f->length > 136) {
        return -1;
    }
    if (t->state != SL_IDLE) {
        return refuse(t); /* Simultaneous offers are both refused, without deadlock. */
    }
    if (!sl_safe_name(f->data + 8, f->length - 8) || sl_get64(f->data) > SL_FILE_LIMIT) {
        puts("[REJECT] Refused unsafe filename or oversized file.");
        return refuse(t);
    }
    memcpy(t->name, f->data + 8, f->length - 8);
    t->name[f->length - 8] = 0;
    if (!fstatat(t->directory, t->name, &st, AT_SYMLINK_NOFOLLOW) || errno != ENOENT) {
        puts("[REJECT] Refused file: destination already exists or cannot be checked.");
        return refuse(t);
    }
    if (RAND_bytes(random, sizeof(random)) != 1) {
        return -1;
    }
    memcpy(t->temporary, ".secure-link-", 13);
    for (i = 0; i < sizeof(random); ++i) {
        snprintf(t->temporary + 13 + i * 2, 3, "%02x", random[i]);
    }
    t->file = openat(t->directory, t->temporary,
                     O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (t->file < 0) {
        /* Never unlink a temporary name that this process did not create. */
        t->temporary[0] = 0;
        return -1;
    }
    t->state = SL_RX;
    t->size = sl_get64(f->data);
    t->count = 0;
    progress(t, "Receiving");
    return t->send(t->context, SL_READY, NULL, 0);
}

int sl_transfer_handle(struct sl_transfer *t, const struct sl_frame *f)
{
    uint8_t count[8];
    size_t offset = 0;
    switch (f->type) {
    case SL_OFFER:
        return receive_offer(t, f);
    case SL_REFUSE:
        if (t->state != SL_TX_OFFER || f->length) {
            return -1;
        }
        puts("[REJECT] Peer refused file (busy, unsafe name, size, or existing destination).");
        reset_file(t);
        return 0;
    case SL_READY:
        if (t->state != SL_TX_OFFER || f->length) {
            return -1;
        }
        return next_chunk(t);
    case SL_ACK:
        if (t->state != SL_TX_ACK || f->length != 8 || sl_get64(f->data) != t->count) {
            return -1;
        }
        progress(t, "Sent");
        return next_chunk(t);
    case SL_CHUNK_DATA:
        if (t->state != SL_RX || !f->length || f->length > t->size - t->count) {
            return -1;
        }
        while (offset < f->length) {
            ssize_t n = write(t->file, f->data + offset, f->length - offset);
            if (n < 0 && errno == EINTR) {
                continue;
            }
            if (n <= 0) {
                return -1;
            }
            offset += (size_t)n;
        }
        t->count += f->length;
        progress(t, "Receiving");
        sl_put64(count, t->count);
        return t->send(t->context, SL_ACK, count, 8);
    case SL_END:
        if (t->state != SL_RX || f->length != 8 || sl_get64(f->data) != t->size ||
            t->count != t->size || fsync(t->file)) {
            return -1;
        }
        if (close(t->file)) {
            t->file = -1;
            return -1;
        }
        t->file = -1;
        /* linkat is atomic and fails if any destination (including symlink)
         * already exists. Temp and destination are in the same pinned dir. */
        if (linkat(t->directory, t->temporary, t->directory, t->name, 0)) {
            return -1;
        }
        if (unlinkat(t->directory, t->temporary, 0)) {
            return -1;
        }
        t->temporary[0] = 0;
        if (fsync(t->directory)) {
            return -1;
        }
        sl_put64(count, t->count);
        printf("[OK] File received: %s (%" PRIu64 " bytes, authenticated)\n", t->name, t->count);
        reset_file(t);
        return t->send(t->context, SL_COMPLETE, count, 8);
    case SL_COMPLETE:
        if (t->state != SL_TX_COMPLETE || f->length != 8 || sl_get64(f->data) != t->size) {
            return -1;
        }
        printf("[OK] File delivered: %s (%" PRIu64 " bytes, peer confirmed)\n", t->name, t->size);
        reset_file(t);
        return 0;
    default:
        return -1;
    }
}
