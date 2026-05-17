#ifndef UTIL_H
#define UTIL_H

#include <stdbool.h>
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

/* Rate-limit a periodic task to wall-clock intervals.
   `*last_ms` holds the last fire time (initialize to 0). `now_ms` should be a
   monotonic timestamp (e.g. server.now_ms). Returns true at most once per
   `interval_ms`, advancing `*last_ms` on each fire. This replaces the brittle
   `cronloops % hz` pattern, which over-fires under event-loop load. */
bool run_every_ms(int64_t *last_ms, int64_t now_ms, int interval_ms);

int open_aof(const char *path);

#endif
