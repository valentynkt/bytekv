#include "util.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

ssize_t read_exact(int fd, char *buf, size_t len) {
  size_t off = 0;
  while (off < len) {
    ssize_t n = read(fd, buf + off, len - off);
    if (n == -1) {
      if (errno == EINTR) {
        continue;
      }
      return -1;
    }
    if (n == 0)
      return (ssize_t)off;
    off += n;
  }
  return (ssize_t)off;
}

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
  return ((int64_t)tp.tv_sec * 1000) + (tp.tv_nsec / 1000000);
}

int64_t realtime_ms(void) {
  struct timespec tp;
  clock_gettime(CLOCK_REALTIME, &tp);
  return ((int64_t)tp.tv_sec * 1000) + (tp.tv_nsec / 1000000);
}
int durable_flush(int fd) {
#if defined(__APPLE__)
  if (fcntl(fd, F_FULLFSYNC) == 0)
    return 0;
  return fsync(fd);
#elif defined(__linux__) // lowercase
  return fdatasync(fd);
#else
  return fsync(fd);
#endif
}

int open_aof(const char *path) {
  if (path == NULL) {
    fprintf(stderr, "[bytekv] open_aof: path is NULL\n");
    return -1;
  }
  int fd = open(path, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0644);
  if (fd == -1) {
    perror("open aof");
    return -1;
  }
  return fd;
}
