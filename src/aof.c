#include "aof.h"
#include "config.h"
#include "server.h"
#include "util.h"
#include <stdio.h>

void aof_cron(void) {
  if (!server.config.aof_enabled)
    return;
  if (server.config.aof_policy != AOF_POLICY_PERTICK)
    return;
  if (!server.aof_buf_dirty)
    return;
  /* stagger by 1 to avoid colliding with client_timeouts_cron,
     which fires on cronloops % N == 0 */
  if ((server.cronloops + 1) % server.config.aof_check_hz != 0)
    return;

  if (durable_flush(server.aof_fd) == -1) {
    perror("aof flush");
    return;
  }
  server.aof_buf_dirty = false;
}
