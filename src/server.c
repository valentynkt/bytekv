#include "server.h"
#include "config.h"
#include "networking.h"
#include "util.h"
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>

server_t server;

/* --- housekeeping --- */

static void active_expire(void) {
  int64_t start = server.now_ms;
  int64_t current = start;
  server_config_t *cfg = &server.config;
  int budget_ms = (1000 / cfg->hz) / 4;
  while (current - start < budget_ms) {
    size_t expired = db_active_sweep(
        server.db, cfg->active_expire_keys_per_round, server.now_ms);
    bool below_threshold = expired * 100 / cfg->active_expire_keys_per_round <
                           (size_t)cfg->active_expire_percent;
    if (below_threshold)
      break;
    current = now_ms();
  }
}

static void before_sleep(void) {
  server.now_ms = now_ms();
  server.cronloops++;

  if (server.config.active_expire_enabled) {
    active_expire();
  }
  if (server.cronloops % server.config.client_timeout_check_hz == 0) {
    check_client_timeouts(&server.el);
  }
}

/* --- signal handling --- */

static volatile sig_atomic_t shutdown_signaled = 0;

static void sig_shutdown_handler(int sig) {
  if (shutdown_signaled && sig == SIGINT) {
    static const char msg[] = "FORCE SHUTDOWN\n";
    write(STDERR_FILENO, msg, sizeof(msg) - 1);
    _exit(1);
  }
  shutdown_signaled = 1;

  unsigned char byte = (unsigned char)sig;
  write(server.pipe[1], &byte, 1);
}

static void setup_signal_handlers(void) {
  struct sigaction sig_act;
  sigemptyset(&sig_act.sa_mask);
  sig_act.sa_flags = 0;
  sig_act.sa_handler = sig_shutdown_handler;
  sigaction(SIGINT, &sig_act, NULL);
  sigaction(SIGTERM, &sig_act, NULL);
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

/* --- lifecycle --- */
static int shutdown_pipe_setup(int server_fd) {
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
  return EXIT_SUCCESS;
}

static int init_server(void) {
  setup_signal_handlers();

  int server_fd =
      create_listener(server.config.port, server.config.tcp_backlog);
  if (server_fd == -1)
    return EXIT_FAILURE;

  printf("[bytekv] listening on port %d (hz=%d)\n", server.config.port,
         server.config.hz);

  if (el_init(&server.el) == -1) {
    close(server_fd);
    return EXIT_FAILURE;
  }
  server.el.before_sleep_proc = before_sleep;

  if (shutdown_pipe_setup(server_fd) == EXIT_FAILURE) {
    return EXIT_FAILURE;
  }

  server.server_fd = server_fd;

  if (el_add(&server.el, server.pipe[0], on_shutdown) == -1)
    goto fail;

  if (el_add(&server.el, server_fd, on_accept) == -1)
    goto fail;

  return EXIT_SUCCESS;

fail:
  close(server_fd);
  el_cleanup(&server.el);
  return EXIT_FAILURE;
}

static void shutdown_server(void) {
  for (int fd = 0; fd < MAX_FDS; fd++) {
    if (server.clients[fd].active) {
      shutdown(fd, SHUT_WR);
      close(fd);
    }
  }
  close(server.server_fd);
  close(server.pipe[0]);
  close(server.pipe[1]);
  el_cleanup(&server.el);
  db_free(server.db);
}

int server_main(const char *configfile) {
  init_server_config();

  if (configfile) {
    if (load_config(configfile) == -1)
      return EXIT_FAILURE;
  }

  server.now_ms = now_ms();
  server.db = db_create(server.now_ms);

  if (init_server() == EXIT_FAILURE) {
    db_free(server.db);
    return EXIT_FAILURE;
  }

  el_run(&server.el);
  shutdown_server();
  return EXIT_SUCCESS;
}
