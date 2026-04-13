#ifndef UTIL_H
#define UTIL_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>


typedef size_t ustime_t; /* microsecond time type. */
typedef size_t mstime_t; /* millisecond time type. */

ustime_t ustime(void);
mstime_t mstime(void);

ssize_t write_all(int fd, const char *buf, size_t len);
int set_non_blocking(int fd);
int create_listener(int port, int backlog);
int64_t now_ms(void);

#endif
