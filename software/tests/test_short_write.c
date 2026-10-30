/* Force short writes to exercise full-duplex backpressure in CLI tests. */
#include <stddef.h>
#include <sys/types.h>

ssize_t __real_write(int fd, const void *buffer, size_t length);

ssize_t __wrap_write(int fd, const void *buffer, size_t length)
{
    if (fd >= 3 && length > 64) {
        length = 64;
    }
    return __real_write(fd, buffer, length);
}
