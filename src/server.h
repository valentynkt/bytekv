#ifndef SERVER_H
#define SERVER_H

#include <stdbool.h>

#include "config.h"
#include "db.h"
#include "event_loop.h"

typedef struct {
    bool active;
    char buf[MSG_MAX + FRAME_HDR_SIZE]; /* read buffer */
    size_t len;                         /* read buffer: bytes accumulated */
    char wbuf[WBUF_SIZE];               /* write buffer */
    size_t wlen;                        /* write buffer: total bytes queued */
    size_t woff;                        /* write buffer: bytes already sent */
} client_t;

typedef struct {
    int port;
    int tcp_backlog;
    int hz;
    bool active_expire_enabled;
    int active_expire_percent;
    int active_expire_keys_per_round;
    int active_expire_budget_ms;
} server_config_t;

typedef struct {
    server_config_t config;
    int server_fd;
    event_loop_t el;
    client_t clients[MAX_FDS];
    size_t clients_count;
    int pipe[2];
    db_t *db;
} server_t;

extern server_t server;

int run_networking(void);

#endif
