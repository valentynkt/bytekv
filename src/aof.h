#ifndef AOF_H
#define AOF_H

#include <stddef.h>
#include <stdint.h>

#include "config.h"

/* AOF opcodes, one per mutation command, written as a single byte. */
typedef enum {
  AOF_OP_SET = 1,
  AOF_OP_DEL = 2,
  AOF_OP_SETEX = 3,
  AOF_OP_EXPIRE = 4,
  AOF_OP_PERSIST = 5,
} aof_opcode_t;

/* AOF rewrite lifecycle. The child pid is tracked separately as a kernel
   handle; this enum records what we want to do when the child terminates.
     IDLE     - no child; cron may start one if thresholds are met
     ACTIVE   - child running, diff buffer accepting writes, finalize on success
     ABORTING - we signaled the child after a diff-buffer overflow; on reap
                the temp file is discarded and we go back to IDLE */
typedef enum {
  AOF_RW_IDLE = 0,
  AOF_RW_ACTIVE,
  AOF_RW_ABORTING,
} aof_rewrite_state_t;

/* --- AOF record layout ---
   All multi-byte integers are big-endian (network byte order).

   ┌───────────┬──────────────┬────────┬─────────┬─────────┬───────────┬─────────────┐
   │ 8B CRC64  │ 4B body_len  │ 1B op  │ 2B klen │ 4B vlen │ 8B expire │ key +
   value │
   └───────────┴──────────────┴────────┴─────────┴─────────┴───────────┴─────────────┘
   |--------- header (12B) ---------|  |---------------- body (body_len)
   ------------|

   CRC64 covers everything from body_len through end of body.
   body_len = 1 + 2 + 4 + 8 + key_len + val_len  (everything after body_len). */

#define AOF_CRC_SIZE 8
#define AOF_BODYLEN_SIZE 4
#define AOF_HEADER_SIZE (AOF_CRC_SIZE + AOF_BODYLEN_SIZE) /* 12 bytes */
#define AOF_BODY_FIXED 15 /* 1 (op) + 2 (klen) + 4 (vlen) + 8 (expire) */
#define AOF_RECORD_MAX MSG_MAX

/* Periodic fsync for pertick policy. */
void aof_cron(void);
void aof_rewrite_cron(void);
int aof_rewrite_start(void);

/* Append one mutation to the AOF. Returns 0 on success, -1 on error. */
int aof_append(aof_opcode_t op, const char *key, size_t key_len,
               const char *value, size_t val_len, int64_t expire_at);

/* Replay AOF on startup; truncates at first corrupt record.
   Returns 0 on success (or empty file), -1 on fatal error. */
int aof_load(const char *path);

#endif
