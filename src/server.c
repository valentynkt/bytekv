#include "server.h"
#include "command.h"
#include "event_loop.h"
#include "util.h"
#include <arpa/inet.h>
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
server_t server;

static void on_write(event_loop_t *el, int fd);
static void on_read(event_loop_t *el, int fd);
static void on_shutdown(event_loop_t *el, int fd);

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
        /* Everything sent — skip kqueue entirely */
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

  c->len += n;
  process_buffer(el, fd);
}
static void on_shutdown(event_loop_t *el, int fd) {
  unsigned char buf[16];
  for (;;) {
    ssize_t n = read(fd, buf, sizeof(buf));
    if (n <= 0)
      break;
    for (ssize_t i = 0; i < n; i++) {
      switch (buf[i]) {
      case SIGINT:
        printf("Received SIGINT, shutting down...\n");
        break;
      case SIGTERM:
        printf("Received SIGTERM, shutting down...\n");
        break;
      default:
        printf("Received signal %d, shutting down...\n", buf[i]);
        break;
      }
    }
  }
  el->stop = 1;
}

static void on_accept(event_loop_t *el, int server_fd) {
  int fd = accept(server_fd, NULL, NULL);
  if (fd == -1) {
    if (errno != EAGAIN && errno != EWOULDBLOCK)
      perror("accept");
    return;
  }
  if (fd >= MAX_CLIENTS_FD) {
    fprintf(stderr, "fd %d >= MAX_CLIENTS_FD, rejecting\n", fd);
    close(fd);
    return;
  }
  if (set_non_blocking(fd) == -1) {
    close(fd);
    return;
  }

  server.clients[fd] = (client_t){.active = true};
  server.clients_count++;
  printf("client connected (fd=%d)\n", fd);

  if (el_add(el, fd, on_read) == -1) {
    close(fd);
    server.clients[fd] = (client_t){0};
    server.clients_count--;
  }
}
static volatile sig_atomic_t shutdown_signaled = 0;

static void sigShutdownHandler(int sig) {
  if (shutdown_signaled && sig == SIGINT) {
    static const char msg[] = "FORCE SHUTDOWN\n";
    write(STDERR_FILENO, msg, sizeof(msg) - 1);
    _exit(1);
  }
  shutdown_signaled = 1;

  unsigned char byte = (unsigned char)sig;
  write(server.pipe[1], &byte, 1);
}
static void setupSignalHandlers(void) {
  struct sigaction sig_act;
  sigemptyset(&sig_act.sa_mask);
  sig_act.sa_flags = 0;
  sig_act.sa_handler = sigShutdownHandler;
  sigaction(SIGINT, &sig_act, NULL);
  sigaction(SIGTERM, &sig_act, NULL);
}
void before_sleep(void) {
  // active expiry
  printf("[LOG] Before Sleep Started\n. kq: %d", server.el.kq);

  if (server.active_expire) {
    active_expire();
  }
  // AOF flushing
}

int init_server(void) {
  setupSignalHandlers();

  int server_fd = create_listener();
  if (server_fd == -1)
    return EXIT_FAILURE;

  if (set_non_blocking(server_fd) == -1) {
    close(server_fd);
    return EXIT_FAILURE;
  }

  printf("[framing] listening on port %d\n", PORT);

  if (el_init(&server.el) == -1) {
    close(server_fd);
    return EXIT_FAILURE;
  }
  server.el.before_sleep_proc = before_sleep;
  server.active_expire = true;
  if (pipe(server.pipe) == -1) {
    perror("pipe");
    close(server_fd);
    return EXIT_FAILURE;
  }

  if (set_non_blocking(server.pipe[0]) == -1 ||
      set_non_blocking(server.pipe[1]) == -1) {
    close(server_fd);
    close(server.pipe[0]);
    close(server.pipe[1]);
    return EXIT_FAILURE;
  }

  server.server_fd = server_fd;
  int pipe_rd = server.pipe[0];
  if (el_add(&server.el, pipe_rd, on_shutdown) == -1) {
    goto shutdown;
  }

  if (el_add(&server.el, server_fd, on_accept) == -1) {
    goto shutdown;
  }

  return EXIT_SUCCESS;

shutdown:
  close(server_fd);
  el_cleanup(&server.el);
  return EXIT_FAILURE;
}

int run_networking(void) {
  command_init();

  if (init_server() == EXIT_FAILURE) {
    command_shutdown();
    return EXIT_FAILURE;
  }

  el_run(&server.el);

  for (int fd = 0; fd < MAX_CLIENTS_FD; fd++) {
    if (server.clients[fd].active) {
      shutdown(fd, SHUT_WR);
      close(fd);
    }
  }
  close(server.server_fd);
  close(server.pipe[0]);
  close(server.pipe[1]);
  el_cleanup(&server.el);
  command_shutdown();
  return EXIT_SUCCESS;
}
