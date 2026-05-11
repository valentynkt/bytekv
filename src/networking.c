#include "networking.h"
#include "command.h"
#include "config.h"
#include "event_loop.h"
#include "server.h"
#include "util.h"
#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static void on_write(event_loop_t *el, int fd);
static void on_read(event_loop_t *el, int fd);
static void process_buffer(event_loop_t *el, int fd);

static void remove_client(event_loop_t *el, int fd) {
  printf("client disconnected (fd=%d)\n", fd);
  el_remove(el, fd);
  close(fd);
  server.clients[fd] = (client_t){0};
  server.clients_count--;
}

static bool queue_write(event_loop_t *el, int fd, const char *data,
                        size_t len) {
  client_t *c = &server.clients[fd];

  if (c->wlen + len > WBUF_SIZE && c->woff > 0) {
    size_t pending = c->wlen - c->woff;
    memmove(c->wbuf, c->wbuf + c->woff, pending);
    c->wlen = pending;
    c->woff = 0;
  }

  if (c->wlen + len > WBUF_SIZE)
    return false;

  bool buf_was_empty = (c->wlen == 0);

  memcpy(c->wbuf + c->wlen, data, len);
  c->wlen += len;

  /* Fast path: try direct write if no prior data was pending */
  if (buf_was_empty) {
    ssize_t n = write(fd, c->wbuf + c->woff, c->wlen - c->woff);
    if (n == -1) {
      if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
        perror("write");
        remove_client(el, fd);
        return false;
      }
      /* EAGAIN/EINTR: write nothing, fall through to register handler */
    } else {
      c->woff += n;
      if (c->woff == c->wlen) {
        /* everything sent, skip kqueue */
        c->woff = 0;
        c->wlen = 0;
        return true;
      }
    }
  }

  /* Slow path: data remains unsent, let the event loop drain it */
  el_add_write(el, fd, on_write);
  return true;
}

static bool send_framed(event_loop_t *el, int fd, const char *payload,
                        uint32_t payload_len) {
  char frame[FRAME_HDR_SIZE + MSG_MAX];
  uint32_t net_len = htonl(payload_len);
  memcpy(frame, &net_len, FRAME_HDR_SIZE);
  memcpy(frame + FRAME_HDR_SIZE, payload, payload_len);
  return queue_write(el, fd, frame, FRAME_HDR_SIZE + payload_len);
}

static void process_buffer(event_loop_t *el, int fd) {
  client_t *c = &server.clients[fd];

  while (c->len >= FRAME_HDR_SIZE) {
    uint32_t net_len;
    memcpy(&net_len, c->buf, FRAME_HDR_SIZE);
    uint32_t payload_len = ntohl(net_len);

    if (payload_len > MSG_MAX) {
      remove_client(el, fd);
      return;
    }
    if (c->len < FRAME_HDR_SIZE + payload_len)
      return;

    char resp[MSG_MAX];
    char *payload = c->buf + FRAME_HDR_SIZE;
    size_t rlen = command_execute(payload, payload_len, resp, sizeof(resp));
    if (!send_framed(el, fd, resp, (uint32_t)rlen))
      return;

    size_t frame_size = FRAME_HDR_SIZE + payload_len;
    memmove(c->buf, c->buf + frame_size, c->len - frame_size);
    c->len -= frame_size;
  }
}

static void on_write(event_loop_t *el, int fd) {
  client_t *c = &server.clients[fd];

  ssize_t n = write(fd, c->wbuf + c->woff, c->wlen - c->woff);
  if (n == -1) {
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
      return;
    perror("write");
    remove_client(el, fd);
    return;
  }

  c->woff += n;

  if (c->woff == c->wlen) {
    c->woff = 0;
    c->wlen = 0;
    el_remove_write(el, fd);

    process_buffer(el, fd);
  }
}

static void on_read(event_loop_t *el, int fd) {
  client_t *c = &server.clients[fd];

  if (!c->active)
    return;

  ssize_t n = read(fd, c->buf + c->len, sizeof(c->buf) - c->len);

  if (n == 0) {
    remove_client(el, fd);
    return;
  }
  if (n == -1) {
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
      return;
    perror("read");
    remove_client(el, fd);
    return;
  }

  c->last_active_ms = server.now_ms;
  c->len += n;
  process_buffer(el, fd);
}

void on_accept(event_loop_t *el, int server_fd) {
  int fd = accept(server_fd, NULL, NULL);
  if (fd == -1) {
    if (errno != EAGAIN && errno != EWOULDBLOCK)
      perror("accept");
    return;
  }
  if (fd >= MAX_FDS) {
    fprintf(stderr, "fd %d >= MAX_FDS, rejecting\n", fd);
    close(fd);
    return;
  }
  if (set_non_blocking(fd) == -1) {
    close(fd);
    return;
  }

  server.clients[fd] =
      (client_t){.active = true, .last_active_ms = server.now_ms};
  server.clients_count++;
  printf("client connected (fd=%d)\n", fd);

  if (el_add(el, fd, on_read) == -1) {
    close(fd);
    server.clients[fd] = (client_t){0};
    server.clients_count--;
  }
}
static void check_client_timeouts(event_loop_t *el) {
  size_t found = 0;
  for (int i = 0; i < MAX_FDS && found < server.clients_count; i++) {
    if (!server.clients[i].active)
      continue;
    found++;
    int64_t idle_s = (server.now_ms - server.clients[i].last_active_ms) / 1000;
    if (idle_s >= server.config.client_timeout_s) {
      printf("client timed out (fd=%d, idle=%llds)\n", i, (long long)idle_s);
      remove_client(el, i);
    }
  }
}

void client_timeouts_cron(event_loop_t *el) {
  static int64_t last_check_ms = 0;
  int interval_ms =
      server.config.client_timeout_check_hz * 1000 / server.config.hz;
  if (!run_every_ms(&last_check_ms, server.now_ms, interval_ms))
    return;
  check_client_timeouts(el);
}

int create_listener(int port, int backlog) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd == -1) {
    perror("socket");
    return -1;
  }

  int opt = 1;
  if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) == -1) {
    perror("setsockopt");
    goto fail;
  }

  struct sockaddr_in addr = {
      .sin_family = AF_INET,
      .sin_port = htons(port),
      .sin_addr.s_addr = INADDR_ANY,
  };

  if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
    perror("bind");
    goto fail;
  }

  if (listen(fd, backlog) == -1) {
    perror("listen");
    goto fail;
  }

  if (set_non_blocking(fd) == -1) {
    close(fd);
    return -1;
  }
  return fd;

fail:
  close(fd);
  return -1;
}
