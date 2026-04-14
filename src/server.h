#ifndef SERVER_H
#define SERVER_H

#include <stdbool.h>

#include "config.h"
#include "db.h"
#include "event_loop.h"
#include "networking.h"

typedef struct {
  int port;
  int tcp_backlog;
  int hz;
  bool active_expire_enabled;
  int active_expire_percent;
  int active_expire_keys_per_round;
} server_config_t;

typedef struct {
  server_config_t config;
  int server_fd;
  event_loop_t el;
  client_t clients[MAX_FDS];
  size_t clients_count;
  int pipe[2];
  int64_t cronloops;
  db_t *db;
  int64_t now_ms;
} server_t;

extern server_t server;

int server_main(void);

#endif
