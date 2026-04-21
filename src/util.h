#ifndef UTIL_H
#define UTIL_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include <fcntl.h>

ssize_t read_exact(int fd, char *buf, size_t len);
ssize_t write_all(int fd, const char *buf, size_t len);
int set_non_blocking(int fd);
int create_listener(int port, int backlog);

/* Monotonic milliseconds, immune to NTP/DST jumps. */
int64_t now_ms(void);

/* Wall-clock milliseconds since Unix epoch. */
int64_t realtime_ms(void);

int durable_flush(int fd);
int open_aof(const char *path);

#endif
