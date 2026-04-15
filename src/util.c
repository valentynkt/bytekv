#include "util.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

ustime_t ustime(void) {
  struct timeval tv;
  ustime_t ust;

  gettimeofday(&tv, NULL);
  ust = ((long long)tv.tv_sec) * 1000000;
  ust += tv.tv_usec;
  return ust;
}

mstime_t mstime(void) { return ustime() / 1000; }

ssize_t write_all(int fd, const char *buf, size_t len) {
  size_t off = 0;
  while (off < len) {
    ssize_t n = write(fd, buf + off, len - off);
    if (n == -1) {
      if (errno == EINTR)
        continue;
      return -1;
    }
    off += n;
  }
  return (ssize_t)off;
}

int set_non_blocking(int fd) {
  int flags = fcntl(fd, F_GETFL);
  if (flags == -1) {
    perror("fcntl F_GETFL");
    return -1;
  }
  if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
    perror("fcntl F_SETFL");
    return -1;
  }
  return 0;
}

int64_t now_ms(void) {
  struct timespec tp;
  clock_gettime(CLOCK_MONOTONIC, &tp);
  int64_t now_in_ms = (tp.tv_sec * 1000) + (tp.tv_nsec / 1000000);
  return now_in_ms;
}
