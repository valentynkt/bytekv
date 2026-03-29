#ifndef UTIL_H
#define UTIL_H

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define PORT 9999
#define BACKLOG 5
#define MSG_MAX 4096

ssize_t write_all(int fd, const char *buf, size_t len);
int set_non_blocking(int fd);
int create_listener(void);

#endif
