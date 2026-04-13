#ifndef SERVER_H
#define SERVER_H

#include <stdbool.h>
#include <string.h>

#include "event_loop.h"
#define MAX_CLIENTS_FD 1024
#define MSG_MAX 4096
#define FRAME_HDR_SIZE 4
#define WBUF_SIZE ((MSG_MAX + FRAME_HDR_SIZE) * 2)

typedef struct {
    bool active;
    char buf[MSG_MAX + FRAME_HDR_SIZE]; /* read buffer */
    size_t len;                         /* read buffer: bytes accumulated */
    char wbuf[WBUF_SIZE];               /* write buffer */
    size_t wlen;                        /* write buffer: total bytes queued */
    size_t woff;                        /* write buffer: bytes already sent */
} client_t;

typedef struct {
    int server_fd;
    event_loop_t el;
    client_t clients[MAX_CLIENTS_FD];
    size_t clients_count;
    int pipe[2];
    bool active_expire;
} server_t;

int run_networking(void);

#endif
