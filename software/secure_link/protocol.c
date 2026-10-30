#define _POSIX_C_SOURCE 200809L
#include "protocol.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/kdf.h>
#include <openssl/rand.h>

uint64_t sl_now(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts)) {
        return 0;
    }
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

void sl_put64(uint8_t *out, uint64_t value)
{
    unsigned i;
    for (i = 0; i < 8; ++i) {
        out[7 - i] = (uint8_t)(value >> (i * 8));
    }
}

uint64_t sl_get64(const uint8_t *in)
{
    uint64_t value = 0;
    unsigned i;
    for (i = 0; i < 8; ++i) {
        value = (value << 8) | in[i];
    }
    return value;
}

int sl_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL);
    return flags < 0 ? -1 : fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

int sl_io(int fd, void *buffer, size_t length, int writing, unsigned timeout_ms)
{
    uint8_t *bytes = buffer;
    uint64_t deadline = sl_now() + timeout_ms;
    while (length) {
        uint64_t now = sl_now();
        struct pollfd pfd = {fd, writing ? POLLOUT : POLLIN, 0};
        ssize_t count;
        int result;
        if (now >= deadline) {
            errno = ETIMEDOUT;
            return -1;
        }
        result = poll(&pfd, 1, (int)(deadline - now));
        if (result < 0 && errno == EINTR) {
            continue;
        }
        if (result <= 0 || (pfd.revents & (POLLERR | POLLNVAL)) ||
            ((pfd.revents & POLLHUP) && !(pfd.revents & POLLIN))) {
            if (!result) {
                errno = ETIMEDOUT;
            }
            return -1;
        }
        count = writing ? write(fd, bytes, length) : read(fd, bytes, length);
        if (count < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
            continue;
        }
        if (count <= 0) {
            errno = ECONNRESET;
            return -1;
        }
        bytes += count;
        length -= (size_t)count;
    }
    return 0;
}

int sl_psk_create(const char *path)
{
    uint8_t key[32];
    struct stat st;
    int fd, result = -1;
    if (RAND_bytes(key, sizeof(key)) != 1) {
        OPENSSL_cleanse(key, sizeof(key));
        return -1;
    }
    fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd >= 0) {
        ssize_t n;
        size_t offset = 0;
        if (!fstat(fd, &st) && (st.st_mode & S_IRUSR)) {
            while (offset < sizeof(key)) {
                n = write(fd, key + offset, sizeof(key) - offset);
                if (n < 0 && errno == EINTR) {
                    continue;
                }
                if (n <= 0) {
                    break;
                }
                offset += (size_t)n;
            }
        }
        result = offset == sizeof(key) && !fsync(fd) ? 0 : -1;
        if (close(fd)) {
            result = -1;
        }
        if (result && unlink(path) && errno != ENOENT) {
            fprintf(stderr, "[ERROR] Failed to remove incomplete PSK file: %s\n", strerror(errno));
        }
    }
    OPENSSL_cleanse(key, sizeof(key));
    return result;
}

int sl_psk_load(const char *path, uint8_t key[32])
{
    uint8_t private_key[32];
    struct stat st;
    size_t offset = 0;
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC), result = -1;
    if (fd < 0) {
        return -1;
    }
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size != 32 ||
        st.st_uid != geteuid() || (st.st_mode & 077) || !(st.st_mode & S_IRUSR)) {
        goto out;
    }
    while (offset < sizeof(private_key)) {
        ssize_t n = read(fd, private_key + offset, sizeof(private_key) - offset);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            goto out;
        }
        offset += (size_t)n;
    }
    {
        uint8_t extra;
        if (read(fd, &extra, 1) != 0) {
            goto out;
        }
    }
    memcpy(key, private_key, 32);
    result = 0;
out:
    OPENSSL_cleanse(private_key, sizeof(private_key));
    close(fd);
    return result;
}

static int mac(const uint8_t key[32], const char *label,
               const uint8_t transcript[80], uint8_t out[32])
{
    uint8_t message[128];
    unsigned length = 0;
    size_t n = strlen(label);
    if (n > sizeof(message) - 80) {
        return -1;
    }
    memcpy(message, label, n);
    memcpy(message + n, transcript, 80);
    return HMAC(EVP_sha256(), key, 32, message, n + 80, out, &length) && length == 32 ? 0 : -1;
}

static void hs_header(uint8_t *message, uint8_t type, int role)
{
    memcpy(message, "SLH1", 4);
    message[4] = type;
    message[5] = (uint8_t)role;
    message[6] = message[7] = 0;
}

static int hs_read(int fd, uint8_t *message, size_t length, uint8_t type,
                   int role, unsigned timeout)
{
    uint8_t header[8];
    hs_header(header, type, role);
    return sl_io(fd, message, length, 0, timeout) || CRYPTO_memcmp(header, message, 8) ? -1 : 0;
}

static int derive(const uint8_t psk[32], const uint8_t transcript[80],
                  int direction, uint8_t key[32])
{
    uint8_t salt[32];
    const char *info = direction ? "secure-link-v1 laptop to kv260" : "secure-link-v1 kv260 to laptop";
    EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_HKDF, NULL);
    size_t length = 32;
    int ok = EVP_Digest(transcript, 80, salt, NULL, EVP_sha256(), NULL) == 1 && ctx &&
        EVP_PKEY_derive_init(ctx) > 0 &&
        EVP_PKEY_CTX_set_hkdf_md(ctx, EVP_sha256()) > 0 &&
        EVP_PKEY_CTX_set1_hkdf_salt(ctx, salt, 32) > 0 &&
        EVP_PKEY_CTX_set1_hkdf_key(ctx, psk, 32) > 0 &&
        EVP_PKEY_CTX_add1_hkdf_info(ctx, (const unsigned char *)info, (int)strlen(info)) > 0 &&
        EVP_PKEY_derive(ctx, key, &length) > 0 && length == 32;
    EVP_PKEY_CTX_free(ctx);
    return ok ? 0 : -1;
}

static int confirmation(int fd, int send, uint8_t type, int role,
                        const uint8_t key[32], const char *label,
                        const uint8_t transcript[80], unsigned timeout)
{
    uint8_t message[40], expected[32];
    if (mac(key, label, transcript, expected)) {
        return -1;
    }
    if (send) {
        hs_header(message, type, role);
        memcpy(message + 8, expected, 32);
        return sl_io(fd, message, sizeof(message), 1, timeout);
    }
    return hs_read(fd, message, sizeof(message), type, role, timeout) ||
        CRYPTO_memcmp(expected, message + 8, 32) ? -1 : 0;
}

int sl_handshake(int fd, int role, const uint8_t psk[32], struct sl_session *s)
{
    uint8_t transcript[80], message[72], expected[32];
    int result = -1;
    unsigned timeout = s->timeout_ms;
    s->established = 0;
    s->role = role;
    s->tx_seq = s->rx_seq = 0;
    if ((role != 0 && role != 1) || !timeout) {
        goto out;
    }
    if (role == 1) {
        hs_header(transcript, 1, 1);
        if (RAND_bytes(transcript + 8, 32) != 1 || sl_io(fd, transcript, 40, 1, timeout) ||
            hs_read(fd, message, 72, 2, 0, timeout)) {
            goto out;
        }
        memcpy(transcript + 40, message, 40);
        if (mac(psk, "secure-link-v1 server", transcript, expected) ||
            CRYPTO_memcmp(expected, message + 40, 32) ||
            confirmation(fd, 1, 3, 1, psk, "secure-link-v1 client", transcript, timeout)) {
            goto out;
        }
    }
    else {
        if (hs_read(fd, transcript, 40, 1, 1, timeout)) {
            goto out;
        }
        hs_header(transcript + 40, 2, 0);
        if (RAND_bytes(transcript + 48, 32) != 1 ||
            mac(psk, "secure-link-v1 server", transcript, expected)) {
            goto out;
        }
        memcpy(message, transcript + 40, 40);
        memcpy(message + 40, expected, 32);
        if (sl_io(fd, message, 72, 1, timeout) ||
            confirmation(fd, 0, 3, 1, psk, "secure-link-v1 client", transcript, timeout)) {
            goto out;
        }
    }
    if (derive(psk, transcript, role, s->tx_key) || derive(psk, transcript, !role, s->rx_key) ||
        confirmation(fd, role == 0, 4, 0, role ? s->rx_key : s->tx_key,
                     "secure-link-v1 finished", transcript, timeout)) {
        goto out;
    }
    s->established = 1;
    result = 0;
out:
    OPENSSL_cleanse(expected, sizeof(expected));
    if (result) {
        sl_session_clear(s);
    }
    return result;
}

int sl_software_crypt(int encrypt, const uint8_t key[32], const uint8_t nonce[12],
                      const uint8_t *aad, size_t aad_length, const uint8_t *input,
                      size_t length, uint8_t tag[16], uint8_t *output)
{
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    uint8_t private_output[SL_CHUNK + 16], private_tag[16];
    int n = 0, total = 0, result = -1;
    if (!ctx || length > SL_CHUNK || aad_length > SL_HEADER) {
        goto out;
    }
    if (EVP_CipherInit_ex(ctx, EVP_chacha20_poly1305(), NULL, NULL, NULL, encrypt) != 1 ||
        EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_IVLEN, 12, NULL) != 1 ||
        EVP_CipherInit_ex(ctx, NULL, NULL, key, nonce, encrypt) != 1 ||
        EVP_CipherUpdate(ctx, NULL, &n, aad, (int)aad_length) != 1 ||
        EVP_CipherUpdate(ctx, private_output, &n, input, (int)length) != 1) {
        goto out;
    }
    total = n;
    if (!encrypt && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_TAG, 16, tag) != 1) {
        goto out;
    }
    if (EVP_CipherFinal_ex(ctx, private_output + total, &n) != 1) {
        result = encrypt ? -1 : -2;
        goto out;
    }
    total += n;
    if ((size_t)total != length ||
        (encrypt && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_GET_TAG, 16, private_tag) != 1)) {
        goto out;
    }
    if (length) {
        memcpy(output, private_output, length);
    }
    if (encrypt) {
        memcpy(tag, private_tag, 16);
    }
    result = 0;
out:
    OPENSSL_cleanse(private_output, sizeof(private_output));
    OPENSSL_cleanse(private_tag, sizeof(private_tag));
    EVP_CIPHER_CTX_free(ctx);
    return result;
}

static int crypt(struct sl_session *s, int encrypt, uint64_t seq, const uint8_t *header,
                 const uint8_t *input, size_t length, uint8_t *tag, uint8_t *output)
{
    uint8_t nonce[12] = {0};
    const uint8_t *key = encrypt ? s->tx_key : s->rx_key;
    sl_put64(nonce + 4, seq);
    if (s->hardware) {
        return aead_uio_crypt(s->hardware, encrypt, key, nonce, header, SL_HEADER,
                              input, length, tag, output, s->timeout_ms);
    }
    return sl_software_crypt(encrypt, key, nonce, header, SL_HEADER, input, length, tag, output);
}

static void hex(const char *label, const uint8_t *bytes, size_t length)
{
    size_t i;
    fprintf(stderr, "[TRACE] %s=", label);
    for (i = 0; i < length; ++i) {
        fprintf(stderr, "%02x", bytes[i]);
    }
    fputc('\n', stderr);
}

static void trace(const char *operation, const uint8_t *wire, size_t length, int result)
{
    uint8_t nonce[12] = {0};
    memcpy(nonce + 4, wire + 8, 8);
    fprintf(stderr, "[TRACE] %s result=%d (0=accepted, -2=authentication, -3=sequence)\n", operation, result);
    hex("nonce", nonce, 12);
    hex("AAD", wire, SL_HEADER);
    hex("ciphertext", wire + SL_HEADER, length - SL_HEADER - 16);
    hex("tag", wire + length - 16, 16);
}

int sl_header_length(const uint8_t header[SL_HEADER], size_t *length)
{
    uint32_t n = ((uint32_t)header[16] << 24) | ((uint32_t)header[17] << 16) |
                 ((uint32_t)header[18] << 8) | header[19];
    if (memcmp(header, "SLF1", 4) || header[4] != 1 || header[5] < SL_CHAT ||
        header[5] > SL_PONG || header[6] > 1 || header[7] ||
        header[20] || header[21] || header[22] || header[23] || n > SL_CHUNK) {
        return -1;
    }
    *length = SL_HEADER + (size_t)n + 16;
    return 0;
}

int sl_encode(struct sl_session *s, uint8_t type, const void *data, size_t length,
              uint8_t wire[SL_WIRE], size_t *wire_length)
{
    if (!s->established || s->tx_seq == UINT64_MAX || length > SL_CHUNK ||
        (length && !data) || type < SL_CHAT || type > SL_PONG) {
        return -1;
    }
    memset(wire, 0, SL_HEADER);
    memcpy(wire, "SLF1", 4);
    wire[4] = 1;
    wire[5] = type;
    wire[6] = (uint8_t)s->role;
    sl_put64(wire + 8, s->tx_seq);
    wire[18] = (uint8_t)(length >> 8);
    wire[19] = (uint8_t)length;
    if (crypt(s, 1, s->tx_seq, wire, data, length, wire + SL_HEADER + length, wire + SL_HEADER)) {
        return -1;
    }
    *wire_length = SL_HEADER + length + 16;
    ++s->tx_seq; /* Consumed even if the subsequent transport write fails. */
    if (s->verbose) {
        trace("TX", wire, *wire_length, 0);
    }
    return 0;
}

int sl_decode(struct sl_session *s, const uint8_t *wire, size_t length, struct sl_frame *out)
{
    struct sl_frame private_frame = {0};
    uint8_t tag[16];
    size_t expected;
    int result;
    if (!s->established || length < SL_HEADER + 16 || sl_header_length(wire, &expected) ||
        expected != length || wire[6] == s->role || s->rx_seq == UINT64_MAX) {
        return -1;
    }
    private_frame.length = length - SL_HEADER - 16;
    private_frame.type = wire[5];
    private_frame.sequence = sl_get64(wire + 8);
    memcpy(tag, wire + length - 16, 16);
    result = crypt(s, 0, private_frame.sequence, wire, wire + SL_HEADER,
                   private_frame.length, tag, private_frame.data);
    if (!result && private_frame.sequence != s->rx_seq) {
        result = -3;
    }
    if (!result) {
        *out = private_frame;
        ++s->rx_seq;
    }
    if (s->verbose) {
        trace("RX", wire, length, result);
    }
    OPENSSL_cleanse(&private_frame, sizeof(private_frame));
    return result;
}

void sl_session_clear(struct sl_session *s)
{
    OPENSSL_cleanse(s, sizeof(*s));
}
