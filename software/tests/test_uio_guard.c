/* Prove that an unconfirmed FPGA state is rejected before opening UIO. */
#define _POSIX_C_SOURCE 200809L
#include "../aead_uio_api.h"
#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static const char *manager_state;
static unsigned map_reads, device_opens;

FILE *__wrap_fopen(const char *path, const char *mode)
{
    const char *value = NULL;
    FILE *file;
    (void)mode;
    if (!strcmp(path, "/sys/class/fpga_manager/fpga0/state")) {
        value = manager_state;
    }
    else {
        ++map_reads;
        if (strstr(path, "/addr")) value = "0xa0000000\n";
        if (strstr(path, "/size")) value = "0x10000000\n";
        if (strstr(path, "/offset")) value = "0\n";
    }
    if (!value) {
        errno = ENOENT;
        return NULL;
    }
    file = tmpfile();
    assert(file && fputs(value, file) >= 0);
    rewind(file);
    return file;
}

int __wrap_open(const char *path, int flags, ...)
{
    (void)flags;
    assert(!strcmp(path, "/dev/uio12"));
    ++device_opens;
    errno = ENODEV;
    return -1;
}

int main(void)
{
    const char *states[] = {NULL, "unknown\n", "", "operating\nextra", "operating\n"};
    struct aead_uio_device device;
    size_t i;
    for (i = 0; i < sizeof(states) / sizeof(states[0]); ++i) {
        manager_state = states[i];
        map_reads = device_opens = 0;
        assert(aead_uio_open(&device, "/dev/uio12") == -1);
        assert(device.fd == -1 && !device.owns_fd && !device.mapping);
        if (i == 4) {
            assert(map_reads == 3 && device_opens == 1);
        }
        else {
            assert(map_reads == 0 && device_opens == 0);
        }
        aead_uio_close(&device);
    }
    puts("PASS FPGA state guard before UIO open");
    return 0;
}
