#ifndef SECURE_LINK_TRANSPORT_H
#define SECURE_LINK_TRANSPORT_H
int sl_serial_open(const char *path, unsigned baud);
int sl_rfcomm_open(const char *address, unsigned channel, unsigned timeout_ms);
#endif
