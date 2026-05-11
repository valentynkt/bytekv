#ifndef SERVER_H
#define SERVER_H

#include <stdbool.h>
#include <sys/types.h>

#include "aof.h"
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
  int client_timeout_s;
  int client_timeout_check_hz;
  int aof_check_hz;
  char *aof_filename;
  bool aof_enabled;
  aof_policy_t aof_policy;
  int aof_rewrite_min_size;
  int aof_rewrite_growth;
  int aof_rewrite_buf_max_size;
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
  int64_t now_ms;          /* monotonic, for elapsed time */
  int64_t now_realtime_ms; /* wall clock, for persisted timestamps */
  int aof_fd;
  bool aof_buf_dirty;
  aof_rewrite_state_t aof_rewrite_state;
  pid_t aof_rewrite_pid;     /* valid when state != AOF_RW_IDLE */
  char *aof_rewrite_buf;     /* accumulated mutations during ACTIVE */
  size_t aof_rewrite_len;
  size_t aof_rewrite_cap;
  off_t aof_rewrite_last_size;
  char *aof_temp_filename;   /* "<aof_filename>.tmp.<pid>" — child write target */
} server_t;

extern server_t server;

int server_main(const char *configfile);

#endif
