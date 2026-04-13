#include "server.h"
#include "networking.h"
#include "util.h"
#include <signal.h>
#include <stdint.h>
#include <sys/socket.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

server_t server;

/* --- housekeeping --- */

static void active_expire(void) {
  int64_t start = server.db->now_ms;
  int64_t current = start;
  server_config_t *cfg = &server.config;
  while (current - start < cfg->active_expire_budget_ms) {
    size_t expired =
        db_active_sweep(server.db, cfg->active_expire_keys_per_round);
    bool below_threshold =
        expired * 100 / cfg->active_expire_keys_per_round <
        (size_t)cfg->active_expire_percent;
    if (below_threshold)
      break;
    current = now_ms();
  }
}

static void before_sleep(void) {
  server.db->now_ms = now_ms();
  server.cronloops++;

  if (server.config.active_expire_enabled) {
    active_expire();
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

static void init_server_config(void) {
  server.config.port = CONFIG_DEFAULT_PORT;
  server.config.tcp_backlog = CONFIG_DEFAULT_TCP_BACKLOG;
  server.config.hz = CONFIG_DEFAULT_HZ;
  server.config.active_expire_enabled = CONFIG_DEFAULT_ACTIVE_EXPIRE_ENABLED;
  server.config.active_expire_percent = CONFIG_DEFAULT_ACTIVE_EXPIRE_PERCENT;
  server.config.active_expire_keys_per_round =
      CONFIG_DEFAULT_ACTIVE_EXPIRE_KEYS_PER_ROUND;
  server.config.active_expire_budget_ms =
      CONFIG_DEFAULT_ACTIVE_EXPIRE_BUDGET_MS;
}

static int init_server(void) {
  init_server_config();
  setup_signal_handlers();

  int server_fd =
      create_listener(server.config.port, server.config.tcp_backlog);
  if (server_fd == -1)
    return EXIT_FAILURE;

  if (set_non_blocking(server_fd) == -1) {
    close(server_fd);
    return EXIT_FAILURE;
  }

  printf("[bytekv] listening on port %d (hz=%d)\n", server.config.port,
         server.config.hz);

  if (el_init(&server.el) == -1) {
    close(server_fd);
    return EXIT_FAILURE;
  }
  server.el.before_sleep_proc = before_sleep;
  server.el.poll_timeout_ms = 1000 / server.config.hz;

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

int server_main(void) {
  server.db = db_create();

  if (init_server() == EXIT_FAILURE) {
    db_free(server.db);
    return EXIT_FAILURE;
  }

  el_run(&server.el);
  shutdown_server();
  return EXIT_SUCCESS;
}
