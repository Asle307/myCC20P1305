#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "transport.h"
#include "protocol.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <termios.h>
#include <unistd.h>
#ifndef SL_SERIAL_ONLY
#include <bluetooth/bluetooth.h>
#include <bluetooth/rfcomm.h>
#endif

int sl_serial_open(const char *path, unsigned baud)
{
    struct termios tty;
    speed_t speed;
    int fd;
    switch (baud) {
    case 9600: speed = B9600; break;
    case 19200: speed = B19200; break;
    case 38400: speed = B38400; break;
    case 57600: speed = B57600; break;
    case 115200: speed = B115200; break;
    case 230400: speed = B230400; break;
    case 460800: speed = B460800; break;
    case 921600: speed = B921600; break;
    default:
        errno = EINVAL;
        return -1;
    }
    fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        return -1;
    }
    if (flock(fd, LOCK_EX | LOCK_NB) || ioctl(fd, TIOCEXCL) || tcgetattr(fd, &tty)) {
        goto fail;
    }
    cfmakeraw(&tty);
    tty.c_cflag &= ~(PARENB | CSTOPB | CSIZE | CRTSCTS);
    tty.c_cflag |= CS8 | CLOCAL | CREAD;
    tty.c_iflag &= ~(IXON | IXOFF | IXANY);
    tty.c_cc[VMIN] = 1;
    tty.c_cc[VTIME] = 0;
    if (cfsetispeed(&tty, speed) || cfsetospeed(&tty, speed) ||
        tcsetattr(fd, TCSANOW, &tty) || tcflush(fd, TCIOFLUSH)) {
        goto fail;
    }
    return fd;
fail:
    close(fd);
    return -1;
}

int sl_rfcomm_open(const char *address, unsigned channel, unsigned timeout_ms)
{
#ifdef SL_SERIAL_ONLY
    (void)address;
    (void)channel;
    (void)timeout_ms;
    errno = ENOTSUP;
    fprintf(stderr, "[ERROR] Serial-only build: rebuild without SL_SERIAL_ONLY for laptop RFCOMM.\n");
    return -1;
#else
    struct sockaddr_rc peer = {0};
    struct pollfd pfd;
    int fd, result, error = 0;
    socklen_t length = sizeof(error);
    size_t i;
    uint64_t deadline;
    if (strlen(address) != 17 || channel < 1 || channel > 30) {
        errno = EINVAL;
        return -1;
    }
    for (i = 0; i < 17; ++i) {
        if (i % 3 == 2 ? address[i] != ':' :
            !((address[i] >= '0' && address[i] <= '9') ||
              (address[i] >= 'a' && address[i] <= 'f') ||
              (address[i] >= 'A' && address[i] <= 'F'))) {
            errno = EINVAL;
            return -1;
        }
    }
    peer.rc_family = AF_BLUETOOTH;
    peer.rc_channel = (uint8_t)channel;
    if (str2ba(address, &peer.rc_bdaddr)) {
        return -1;
    }
    fd = socket(AF_BLUETOOTH, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, BTPROTO_RFCOMM);
    if (fd < 0) {
        return -1;
    }
    result = connect(fd, (struct sockaddr *)&peer, sizeof(peer));
    if (result < 0 && errno != EINPROGRESS) {
        goto fail;
    }
    deadline = sl_now() + timeout_ms;
    pfd = (struct pollfd){fd, POLLOUT, 0};
    while (result < 0) {
        uint64_t now = sl_now();
        if (now >= deadline) {
            errno = ETIMEDOUT;
            goto fail;
        }
        result = poll(&pfd, 1, (int)(deadline - now));
        if (result < 0 && errno == EINTR) {
            continue;
        }
        if (result <= 0 || getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length) || error) {
            if (error) {
                errno = error;
            }
            goto fail;
        }
    }
    return fd;
fail:
    close(fd);
    return -1;
#endif
}
