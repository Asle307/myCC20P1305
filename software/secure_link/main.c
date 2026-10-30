#define _POSIX_C_SOURCE 200809L
#include "protocol.h"
#include "transfer.h"
#include "transport.h"
#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <openssl/crypto.h>

static volatile sig_atomic_t interrupted;

#define SEND_QUEUE_SIZE 8

struct pending_send {
    uint8_t wire[SL_WIRE];
    size_t length, offset;
    uint64_t queued_at;
};

struct application {
    int fd, chat_pending, demo, closing;
    struct sl_session session;
    struct sl_transfer transfer;
    struct sl_decoder decoder;
    uint8_t chat_wire[SL_WIRE];
    size_t chat_length;
    uint64_t chat_sequence, chat_started, last_rx, last_ping;
    unsigned rejections;
    struct pending_send sends[SEND_QUEUE_SIZE];
    unsigned send_head, send_count;
};

static void signal_stop(int sig)
{
    (void)sig;
    interrupted = 1;
}

static int queue_wire(struct application *app, const uint8_t *wire, size_t length)
{
    struct pending_send *send;
    if (app->send_count == SEND_QUEUE_SIZE || length > SL_WIRE) {
        fprintf(stderr, "[ERROR] Link send queue full; ending session.\n");
        return -1;
    }
    send = &app->sends[(app->send_head + app->send_count) % SEND_QUEUE_SIZE];
    memcpy(send->wire, wire, length);
    send->length = length;
    send->offset = 0;
    send->queued_at = sl_now();
    ++app->send_count;
    return 0;
}

static int send_frame(void *context, uint8_t type, const void *data, size_t length)
{
    struct application *app = context;
    uint8_t wire[SL_WIRE];
    size_t wire_length;
    if (app->send_count == SEND_QUEUE_SIZE ||
        sl_encode(&app->session, type, data, length, wire, &wire_length)) {
        return -1;
    }
    return queue_wire(app, wire, wire_length);
}

static int flush_send(struct application *app)
{
    struct pending_send *send = &app->sends[app->send_head];
    ssize_t n = write(app->fd, send->wire + send->offset, send->length - send->offset);
    if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
        return 0;
    }
    if (n <= 0) {
        fprintf(stderr, "[ERROR] Link write failed.\n");
        return -1;
    }
    send->offset += (size_t)n;
    if (send->offset == send->length) {
        app->send_head = (app->send_head + 1) % SEND_QUEUE_SIZE;
        --app->send_count;
    }
    return 0;
}

/* Reject overlong encodings, surrogates, invalid continuation bytes and
 * terminal control characters. The remaining UTF-8 is displayed verbatim. */
static int safe_utf8(const uint8_t *s, size_t length)
{
    size_t i = 0;
    while (i < length) {
        uint32_t cp;
        unsigned trailing, j;
        uint8_t c = s[i++];
        if (c < 128) {
            if ((c < 32 && c != '\t') || c == 127) {
                return 0;
            }
            continue;
        }
        if (c >= 0xc2 && c <= 0xdf) {
            cp = c & 31;
            trailing = 1;
        }
        else if (c >= 0xe0 && c <= 0xef) {
            cp = c & 15;
            trailing = 2;
        }
        else if (c >= 0xf0 && c <= 0xf4) {
            cp = c & 7;
            trailing = 3;
        }
        else {
            return 0;
        }
        if (length - i < trailing) {
            return 0;
        }
        for (j = 0; j < trailing; ++j) {
            c = s[i++];
            if ((c & 0xc0) != 0x80) {
                return 0;
            }
            cp = (cp << 6) | (c & 63);
        }
        if ((trailing == 1 && cp < 0x80) || (trailing == 2 && cp < 0x800) ||
            (trailing == 3 && cp < 0x10000) || cp > 0x10ffff ||
            (cp >= 0xd800 && cp <= 0xdfff) || (cp >= 0x80 && cp <= 0x9f)) {
            return 0;
        }
    }
    return 1;
}

static int chat(struct application *app, const char *text, int demo)
{
    size_t length = strlen(text);
    if (app->chat_pending || app->transfer.state != SL_IDLE) {
        puts("[WAIT] Busy: wait for the chat receipt or file transfer to finish.");
        return 0;
    }
    if (!length || length > SL_CHUNK || !safe_utf8((const uint8_t *)text, length)) {
        puts("[ERROR] Chat requires 1..4096 bytes of UTF-8 without terminal controls.");
        return 0;
    }
    app->chat_sequence = app->session.tx_seq;
    if (sl_encode(&app->session, SL_CHAT, text, length, app->chat_wire, &app->chat_length) ||
        queue_wire(app, app->chat_wire, app->chat_length)) {
        return -1;
    }
    app->chat_pending = 1;
    app->chat_started = sl_now();
    app->demo = demo;
    return 0;
}

static void banner(int role)
{
    puts("+------------------------------------------+");
    printf("| %-40s |\n", "          SECURE LINK / SPP");
    printf("| %-40s |\n", "       ChaCha20-Poly1305  |  PSK");
    puts("+------------------------------------------+");
    printf("[INFO] Role: %s | AEAD: %s\n", role ? "LAPTOP" : "KV260",
           role ? "OpenSSL" : "FPGA / UIO");
}

static void help(void)
{
    puts("[HELP] /msg TEXT | plain text | /file PATH | /status\n"
         "[HELP] /tamper TEXT | /replay TEXT | /help | /quit\n"
         "[HELP] Demos deliver once, then inject a modified or replayed copy.");
}

static int command(struct application *app, char *line)
{
    size_t length = strlen(line);
    if (!strcmp(line, "/quit")) {
        return send_frame(app, SL_CLOSE, NULL, 0) ? -1 : 1;
    }
    if (!strcmp(line, "/help")) {
        help();
        return 0;
    }
    if (!strcmp(line, "/status")) {
        printf("[STATUS] Session authenticated; backend=%s; tx-next=%" PRIu64 "; rx-next=%" PRIu64
               "; file-state=%d; chat-pending=%d\n",
               app->session.hardware ? "FPGA UIO" : "OpenSSL",
               app->session.tx_seq, app->session.rx_seq, app->transfer.state, app->chat_pending);
        return 0;
    }
    if (length >= 6 && !strncmp(line, "/file ", 6)) {
        if (app->chat_pending) {
            puts("[WAIT] Wait for the chat receipt before offering a file.");
            return 0;
        }
        return sl_transfer_offer(&app->transfer, line + 6) < 0 ? -1 : 0;
    }
    if (length >= 8 && !strncmp(line, "/tamper ", 8)) {
        return chat(app, line + 8, 1);
    }
    if (length >= 8 && !strncmp(line, "/replay ", 8)) {
        return chat(app, line + 8, 2);
    }
    if (length >= 5 && !strncmp(line, "/msg ", 5)) {
        return chat(app, line + 5, 0);
    }
    if (line[0] == '/') {
        puts("[ERROR] Unknown command; /help lists commands.");
        return 0;
    }
    return line[0] ? chat(app, line, 0) : 0;
}

static int handle_frame(struct application *app, const struct sl_frame *f)
{
    uint8_t receipt[8];
    switch (f->type) {
    case SL_CHAT:
        if (!f->length || !safe_utf8(f->data, f->length)) {
            return -1;
        }
        printf("[CHAT] Peer: %.*s\n", (int)f->length, (const char *)f->data);
        sl_put64(receipt, f->sequence);
        return send_frame(app, SL_RECEIPT, receipt, 8);
    case SL_RECEIPT:
        if (f->length != 8 || !app->chat_pending || sl_get64(f->data) != app->chat_sequence) {
            return -1;
        }
        puts("[OK] Chat delivered (authenticated receipt).");
        app->chat_pending = 0;
        if (app->demo) {
            if (app->demo == 1) {
                app->chat_wire[SL_HEADER] ^= 1;
            }
            printf("[DEMO] %s: injecting a copy of accepted sequence %" PRIu64 ". Observe peer.\n",
                   app->demo == 1 ? "tamper" : "replay", app->chat_sequence);
            if (queue_wire(app, app->chat_wire, app->chat_length)) {
                return -1;
            }
            app->demo = 0;
        }
        OPENSSL_cleanse(app->chat_wire, sizeof(app->chat_wire));
        return 0;
    case SL_CLOSE:
        if (f->length) {
            return -1;
        }
        puts("[INFO] Peer closed the session. Restart both endpoints for a new session.");
        return 1;
    case SL_PING:
        return f->length ? -1 : send_frame(app, SL_PONG, NULL, 0);
    case SL_PONG:
        return f->length ? -1 : 0;
    default:
        return sl_transfer_handle(&app->transfer, f);
    }
}

/* Consume at most one header/body boundary per read; never wait for a whole
 * frame in the UI event loop. Decoder storage is fixed before parsing lengths. */
static int receive_bytes(struct application *app)
{
    struct sl_decoder *d = &app->decoder;
    struct sl_frame frame;
    ssize_t n;
    int result;
    if (!d->target) {
        d->target = SL_HEADER;
    }
    n = read(app->fd, d->wire + d->used, d->target - d->used);
    if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
        return 0;
    }
    if (n <= 0) {
        fprintf(stderr, "[ERROR] Link disconnected.\n");
        return -1;
    }
    if (!d->used) {
        d->started = sl_now();
    }
    d->used += (size_t)n;
    if (d->used != d->target) {
        return 0;
    }
    if (d->target == SL_HEADER) {
        if (sl_header_length(d->wire, &d->target)) {
            fprintf(stderr, "[ERROR] Invalid/oversized frame header; ending session.\n");
            return -1;
        }
        return 0;
    }
    result = sl_decode(&app->session, d->wire, d->target, &frame);
    d->used = d->target = 0;
    if (result == -2 || result == -3) {
        if (result == -2) {
            fprintf(stderr, "[AUTH] REJECTED: authentication failed; no plaintext released.\n");
        }
        else if (sl_get64(d->wire + 8) < app->session.rx_seq) {
            fprintf(stderr, "[REPLAY] REJECTED: sequence already accepted; no plaintext released.\n");
        }
        else {
            fprintf(stderr, "[ORDER] REJECTED: unexpected sequence; no plaintext released.\n");
        }
        if (++app->rejections > 8 || app->transfer.state != SL_IDLE) {
            return -1;
        }
        return 0;
    }
    if (result) {
        fprintf(stderr, "[ERROR] Fatal frame or AEAD backend error.\n");
        return -1;
    }
    app->last_rx = sl_now();
    app->rejections = 0;
    result = handle_frame(app, &frame);
    OPENSSL_cleanse(&frame, sizeof(frame));
    return result;
}

static int event_loop(struct application *app)
{
    char line[SL_CHUNK + 1];
    size_t used = 0;
    int overflow = 0;
    app->last_rx = app->last_ping = sl_now();
    help();
    while (!interrupted) {
        struct pollfd fds[2] = {
            {app->fd, (short)((app->closing ? 0 : POLLIN) | (app->send_count ? POLLOUT : 0)), 0},
            {app->closing ? -1 : STDIN_FILENO, POLLIN, 0}
        };
        uint64_t now;
        int result = poll(fds, 2, 200);
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (!app->closing && (fds[0].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL))) {
            result = receive_bytes(app);
            if (result) {
                return result < 0 ? -1 : 0;
            }
        }
        else if (app->closing && (fds[0].revents & (POLLHUP | POLLERR | POLLNVAL))) {
            fprintf(stderr, "[ERROR] Link closed while sending.\n");
            return -1;
        }
        if (app->send_count && (fds[0].revents & POLLOUT) && flush_send(app)) {
            return -1;
        }
        if (fds[1].revents & (POLLIN | POLLHUP)) {
            char bytes[1024];
            ssize_t n = read(STDIN_FILENO, bytes, sizeof(bytes));
            ssize_t i;
            if (n == 0) {
                if (send_frame(app, SL_CLOSE, NULL, 0)) {
                    return -1;
                }
                app->closing = 1;
            }
            if (n < 0 && errno != EINTR && errno != EAGAIN) {
                return -1;
            }
            for (i = 0; i < n; ++i) {
                if (bytes[i] == '\n') {
                    if (!overflow) {
                        if (used && line[used - 1] == '\r') {
                            --used;
                        }
                        line[used] = 0;
                        result = command(app, line);
                        if (result < 0) {
                            return -1;
                        }
                        if (result > 0) {
                            app->closing = 1;
                            break;
                        }
                    }
                    else {
                        puts("[ERROR] Input line discarded: too long or contains NUL.");
                    }
                    used = 0;
                    overflow = 0;
                }
                else if (used == SL_CHUNK || bytes[i] == '\0') {
                    overflow = 1;
                }
                else if (!overflow) {
                    line[used++] = bytes[i];
                }
            }
        }
        now = sl_now();
        if (app->closing && !app->send_count) {
            return 0;
        }
        if ((app->decoder.used && now - app->decoder.started >= app->session.timeout_ms) ||
            (app->send_count && now - app->sends[app->send_head].queued_at >= app->session.timeout_ms) ||
            now - app->last_rx >= 30000 ||
            (app->chat_pending && now - app->chat_started >= 30000) ||
            (app->transfer.state != SL_IDLE && now - app->transfer.progressed >= 30000)) {
            fprintf(stderr, "[ERROR] Session timeout (send, partial frame, peer, receipt or file progress).\n");
            return -1;
        }
        if (!app->closing && now - app->last_ping >= 5000) {
            if (send_frame(app, SL_PING, NULL, 0)) {
                return -1;
            }
            app->last_ping = now;
        }
    }
    puts("[INFO] Interrupted; ending session.");
    return 0;
}

static int number(const char *text, unsigned minimum, unsigned maximum, unsigned *out)
{
    char *end;
    unsigned long value;
    errno = 0;
    value = strtoul(text, &end, 10);
    if (!text[0] || strspn(text, "0123456789") != strlen(text) || errno ||
        *end || value < minimum || value > maximum) {
        return -1;
    }
    *out = (unsigned)value;
    return 0;
}

static void usage(void)
{
    puts("[HELP] secure_link --keygen PSK_FILE | --check-key PSK_FILE\n"
         "[HELP] secure_link --role kv260 --serial PATH --psk FILE --receive DIR [--uio /dev/uio12]\n"
         "[HELP] secure_link --role laptop --bluetooth AA:BB:CC:DD:EE:FF --psk FILE --receive DIR\n"
         "[HELP] Options: --baud 230400 --channel 1 --timeout-ms 15000 --verbose\n"
         "[HELP] Linux and OpenSSL required; laptop builds also require BlueZ. Create receive DIR.\n"
         "[HELP] PSK: exactly 32 bytes, owned by invoking user, owner-readable, no group/other access.\n"
         "[HELP] No automatic reconnect, retransmission or partial-file resume.");
}

int main(int argc, char **argv)
{
    static const struct option options[] = {
        {"role", required_argument, NULL, 'r'}, {"serial", required_argument, NULL, 's'},
        {"bluetooth", required_argument, NULL, 'b'}, {"psk", required_argument, NULL, 'p'},
        {"receive", required_argument, NULL, 'o'}, {"uio", required_argument, NULL, 'u'},
        {"baud", required_argument, NULL, 'a'}, {"channel", required_argument, NULL, 'c'},
        {"timeout-ms", required_argument, NULL, 't'}, {"verbose", no_argument, NULL, 'v'},
        {"keygen", required_argument, NULL, 'k'}, {"check-key", required_argument, NULL, 'K'},
        {"help", no_argument, NULL, 'h'},
#ifdef SL_TESTING
        {"test-fd", required_argument, NULL, 'F'},
#endif
        {NULL, 0, NULL, 0}
    };
    const char *role = NULL, *serial = NULL, *address = NULL, *psk_path = NULL;
    const char *directory = NULL, *uio = "/dev/uio12", *keygen = NULL, *check = NULL;
    unsigned baud = 230400, channel = 1, timeout = 15000;
    int opt, result = 2, test_fd = -1, option_count = 0;
    uint8_t psk[32] = {0};
    struct application app = {0};
    struct aead_uio_device device = {0};
    struct sigaction action = {0};
    app.fd = app.transfer.file = app.transfer.directory = -1;
    device.fd = -1;
    setvbuf(stdout, NULL, _IOLBF, 0);
    while ((opt = getopt_long(argc, argv, "", options, NULL)) != -1) {
        ++option_count;
        switch (opt) {
        case 'r': role = optarg; break;
        case 's': serial = optarg; break;
        case 'b': address = optarg; break;
        case 'p': psk_path = optarg; break;
        case 'o': directory = optarg; break;
        case 'u': uio = optarg; break;
        case 'a': if (number(optarg, 9600, 921600, &baud)) goto usage_error; break;
        case 'c': if (number(optarg, 1, 30, &channel)) goto usage_error; break;
        case 't': if (number(optarg, 100, 60000, &timeout)) goto usage_error; break;
        case 'v': app.session.verbose = 1; break;
        case 'k': keygen = optarg; break;
        case 'K': check = optarg; break;
#ifdef SL_TESTING
        case 'F': {
            unsigned n;
            if (number(optarg, 3, 1048575, &n)) goto usage_error;
            test_fd = (int)n;
            break;
        }
#endif
        case 'h': usage(); return 0;
        default: goto usage_error;
        }
    }
    if (optind != argc) {
        goto usage_error;
    }
    if (keygen || check) {
        if (option_count != 1 || (keygen && check)) {
            goto usage_error;
        }
        result = keygen ? sl_psk_create(keygen) : sl_psk_load(check, psk);
        OPENSSL_cleanse(psk, sizeof(psk));
        if (result) {
            fprintf(stderr, "[ERROR] PSK operation failed; require a new output path or a private, owned 32-byte regular input file.\n");
            return 1;
        }
        puts(keygen ? "[OK] Created random 32-byte PSK (private file; key not displayed)." : "[OK] PSK size, owner and access permissions OK.");
        return 0;
    }
    if (!role || !psk_path || !directory || (strcmp(role, "kv260") && strcmp(role, "laptop"))) {
        goto usage_error;
    }
    app.session.role = !strcmp(role, "laptop");
    app.session.timeout_ms = timeout;
    banner(app.session.role);
    if (sl_psk_load(psk_path, psk)) {
        fprintf(stderr, "[ERROR] Invalid PSK file: check owner, permissions and exact 32-byte length.\n");
        goto cleanup;
    }
    if (sl_transfer_init(&app.transfer, directory, send_frame, &app)) {
        fprintf(stderr, "[ERROR] Receive directory: %s\n", strerror(errno));
        goto cleanup;
    }
    action.sa_handler = signal_stop;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGTERM, &action, NULL);
    signal(SIGPIPE, SIG_IGN);
    if (test_fd >= 0) {
        app.fd = test_fd;
        fprintf(stderr, "[WARN] TEST BUILD: socket transport and software AEAD for both roles.\n");
    }
    else if (!app.session.role) {
        if (!serial || address) {
            goto usage_error;
        }
        if (aead_uio_open(&device, uio)) {
            goto cleanup;
        }
        app.session.hardware = &device.bus;
        app.fd = sl_serial_open(serial, baud);
    }
    else {
        if (!address || serial) {
            goto usage_error;
        }
        app.fd = sl_rfcomm_open(address, channel, timeout);
    }
    if (app.fd < 0 || sl_nonblocking(app.fd)) {
        fprintf(stderr, "[ERROR] Open Bluetooth/serial transport: %s\n", strerror(errno));
        goto cleanup;
    }
    puts("[INFO] Transport open; authenticating a fresh session...");
    if (sl_handshake(app.fd, app.session.role, psk, &app.session)) {
        fprintf(stderr, "[ERROR] Handshake failed (PSK, altered transcript, link or timeout). Restart both endpoints.\n");
        result = 1;
        goto cleanup;
    }
    OPENSSL_cleanse(psk, sizeof(psk));
    puts("[OK] Session established: mutual authentication and directional keys confirmed.");
    result = event_loop(&app) ? 1 : 0;
    if (result) {
        fprintf(stderr, "[ERROR] Session ended on link/protocol/IO error; restart both endpoints. No resume or automatic retry.\n");
    }
cleanup:
    if (app.transfer.state != SL_IDLE) {
        fprintf(stderr, "[WARN] File transfer aborted; discarding incomplete data. No resume.\n");
    }
    sl_transfer_clear(&app.transfer);
    if (app.fd >= 0) {
        close(app.fd);
    }
    aead_uio_close(&device);
    OPENSSL_cleanse(psk, sizeof(psk));
    sl_session_clear(&app.session);
    OPENSSL_cleanse(app.chat_wire, sizeof(app.chat_wire));
    return result;
usage_error:
    usage();
    result = 2;
    goto cleanup;
}
