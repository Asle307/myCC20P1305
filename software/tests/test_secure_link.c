#define _XOPEN_SOURCE 600
#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "../secure_link/protocol.h"
#include "../secure_link/transfer.h"
#include "../secure_link/transport.h"
#include <assert.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include "protocol_vector.h"

int main(void)
{
    struct sl_session sender = {0}, receiver = {0};
    struct sl_frame frame, original;
    uint8_t wire[SL_WIRE], other[SL_WIRE], tag[16], output[3];
    size_t length, other_length, i;
    const uint8_t message[] = {0xe2, 0x98, 0x83};
    sender.role = 1;
    sender.established = receiver.established = 1;
    sender.timeout_ms = receiver.timeout_ms = 1000;
    for (i = 0; i < 32; ++i) {
        sender.tx_key[i] = receiver.rx_key[i] = (uint8_t)i;
    }
    assert(sl_encode(&sender, SL_CHAT, message, sizeof(message), wire, &length) == 0);
    assert(length == sizeof(protocol_vector) && !memcmp(wire, protocol_vector, length));
    assert(sender.tx_seq == 1);
    assert(sl_decode(&receiver, wire, length, &frame) == 0);
    assert(frame.sequence == 0 && frame.type == SL_CHAT && frame.length == 3);
    assert(!memcmp(frame.data, message, 3) && receiver.rx_seq == 1);
    memset(&frame, 0xa5, sizeof(frame));
    original = frame;
    assert(sl_decode(&receiver, wire, length, &frame) == -3); /* Replay. */
    assert(!memcmp(&frame, &original, sizeof(frame)) && receiver.rx_seq == 1);
    for (i = 0; i < 4; ++i) {
        const size_t offsets[] = {SL_HEADER, sizeof(protocol_vector) - 1, 5, 15};
        memcpy(other, wire, length);
        other[offsets[i]] ^= 1;
        /* Header type 1 xor 1 is invalid, use another well-formed type. */
        if (i == 2) other[5] = SL_OFFER;
        assert(sl_decode(&receiver, other, length, &frame) == -2);
        assert(!memcmp(&frame, &original, sizeof(frame)) && receiver.rx_seq == 1);
    }
    sender.tx_seq = 2;
    assert(sl_encode(&sender, SL_CHAT, message, 3, other, &other_length) == 0);
    assert(sl_decode(&receiver, other, other_length, &frame) == -3); /* Valid but ahead. */
    assert(receiver.rx_seq == 1 && !memcmp(&frame, &original, sizeof(frame)));
    sender.tx_seq = 1;
    assert(sl_encode(&sender, SL_CHAT, message, 3, other, &other_length) == 0);
    assert(memcmp(wire + SL_HEADER, other + SL_HEADER, 3)); /* Distinct nonce. */
    assert(sl_decode(&receiver, other, other_length, &frame) == 0);
    assert(receiver.rx_seq == 2);
    sender.tx_seq = UINT64_MAX;
    assert(sl_encode(&sender, SL_CHAT, message, 3, other, &other_length) == -1);
    assert(sender.tx_seq == UINT64_MAX);
    sender.established = 0;
    assert(sl_encode(&sender, SL_CHAT, message, 3, other, &other_length) == -1);
    receiver.established = 0;
    assert(sl_decode(&receiver, wire, length, &frame) == -1);
    memcpy(tag, wire + length - 16, 16);
    tag[0] ^= 1;
    memset(output, 0xa5, sizeof(output));
    {
        const uint8_t nonce[12] = {0};
        assert(sl_software_crypt(0, receiver.rx_key, nonce, wire, SL_HEADER,
                                 wire + SL_HEADER, 3, tag, output) == -2);
        for (i = 0; i < sizeof(output); ++i) assert(output[i] == 0xa5);
    }
    sender.tx_seq = 0;
    sender.established = 1;
    assert(sl_encode(&sender, SL_PING, NULL, 0, wire, &length) == 0);
    receiver.established = 1;
    receiver.rx_seq = 0;
    assert(sl_decode(&receiver, wire, length, &frame) == 0 && frame.length == 0);
    sl_session_clear(&sender);
    for (i = 0; i < sizeof(sender); ++i) assert(((uint8_t *)&sender)[i] == 0);
    assert(!sl_safe_name((const uint8_t *)"../x", 4));
    assert(!sl_safe_name((const uint8_t *)".hidden", 7));
    assert(sl_rfcomm_open("invalid", 1, 100) == -1);
    assert(sl_rfcomm_open("00:11:22:33:44:55", 31, 100) == -1);
    {
        int master = posix_openpt(O_RDWR | O_NOCTTY), serial;
        char *path;
        struct termios tty;
        assert(master >= 0 && !grantpt(master) && !unlockpt(master));
        path = ptsname(master);
        assert(path);
        serial = sl_serial_open(path, 230400);
        assert(serial >= 0 && !tcgetattr(serial, &tty));
        assert(cfgetispeed(&tty) == B230400 && cfgetospeed(&tty) == B230400);
        assert((tty.c_cflag & CSIZE) == CS8 && !(tty.c_cflag & (PARENB | CSTOPB | CRTSCTS)));
        assert(!(tty.c_iflag & (IXON | IXOFF | IXANY)) && !(tty.c_lflag & (ICANON | ECHO)));
        assert(sl_serial_open(path, 230400) == -1);
        close(serial);
        close(master);
    }
    puts("PASS secure link independent wire vector, private plaintext, sequence bounds and serial PTY");
    return 0;
}
