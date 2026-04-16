#ifndef NETWORKING_H
#define NETWORKING_H

#include "config.h"
#include "event_loop.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
  bool active;
  int64_t last_active_ms;
  char buf[MSG_MAX + FRAME_HDR_SIZE]; /* read buffer */
  size_t len;                         /* read buffer: bytes accumulated */
  char wbuf[WBUF_SIZE];               /* write buffer */
  size_t wlen;                        /* write buffer: total bytes queued */
  size_t woff;                        /* write buffer: bytes already sent */
} client_t;

void on_accept(event_loop_t *el, int fd);
void client_timeouts_cron(event_loop_t *el);
int create_listener(int port, int backlog);

#endif
