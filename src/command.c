#include "command.h"
#include "aof.h"
#include "server.h"
#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define MAX_TOKENS 8

/* types */

typedef struct {
  char *buf;
  size_t cap;
  size_t len;
} respbuf;

typedef struct {
  int argc;
  char **argv;
  respbuf resp;
} command_ctx_t;

typedef void cmd_proc_t(command_ctx_t *ctx);

typedef struct {
  const char *name;
  cmd_proc_t *proc;
  int arity;
  aof_opcode_t aof_op; /* 0 = read-only, don't log to AOF */
} command_entry_t;

/* response helpers */

static void resp_add(respbuf *r, const char *s) {
  size_t slen = strlen(s);
  size_t avail = r->cap - r->len;
  if (slen > avail)
    slen = avail;
  memcpy(r->buf + r->len, s, slen);
  r->len += slen;
}

__attribute__((format(printf, 2, 3))) static void
resp_addf(respbuf *r, const char *fmt, ...) {
  size_t avail = r->cap - r->len;
  if (avail == 0)
    return;
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(r->buf + r->len, avail, fmt, ap);
  va_end(ap);
  if (n > 0) {
    if ((size_t)n >= avail)
      n = (int)(avail - 1);
    r->len += (size_t)n;
  }
}

/* argument parsing */

static bool parse_seconds(const char *s, int64_t *out) {
  char *end;
  int64_t seconds = strtoll(s, &end, 10);
  if (end == s || *end != '\0' || seconds <= 0)
    return false;
  *out = seconds;
  return true;
}

/* command handlers */

static void cmd_get(command_ctx_t *ctx) {
  const char *key = ctx->argv[1];

  char *val = db_get(server.db, key, server.now_realtime_ms);
  if (val == NULL) {
    resp_add(&ctx->resp, "-ERR key not found\n");
    return;
  }
  resp_addf(&ctx->resp, "+%s\n", val);
}

static void cmd_ttl(command_ctx_t *ctx) {
  const char *key = ctx->argv[1];

  int64_t ttl = db_get_ttl(server.db, key, server.now_realtime_ms);
  if (ttl == -2) {
    resp_add(&ctx->resp, "-ERR key not found\n");
    return;
  }
  if (ttl == -1) {
    resp_add(&ctx->resp, "-ERR key does not have TTL\n");
    return;
  }
  resp_addf(&ctx->resp, "+ Key - %s\nTTL = %" PRId64 "\n", key, ttl);
}

static void cmd_set(command_ctx_t *ctx) {
  const char *key = ctx->argv[1];
  const char *value = ctx->argv[2];

  if (db_set(server.db, key, value) == EXIT_FAILURE) {
    resp_add(&ctx->resp, "-ERR SET COMMAND\n");
    return;
  }
  aof_append(AOF_OP_SET, key, strlen(key), value, strlen(value), 0);
  resp_add(&ctx->resp, "+OK\n");
}

static void cmd_setex(command_ctx_t *ctx) {
  const char *key = ctx->argv[1];
  const char *ttl_str = ctx->argv[2];
  const char *value = ctx->argv[3];

  int64_t seconds;
  if (!parse_seconds(ttl_str, &seconds)) {
    resp_add(&ctx->resp, "-ERR invalid expire time\n");
    return;
  }
  int64_t expire_at = server.now_realtime_ms + (seconds * 1000);
  if (db_setex(server.db, key, value, expire_at) == EXIT_FAILURE) {
    resp_add(&ctx->resp, "-ERR SETEX COMMAND\n");
    return;
  }
  aof_append(AOF_OP_SETEX, key, strlen(key), value, strlen(value), expire_at);
  resp_add(&ctx->resp, "+OK\n");
}

static void cmd_expire(command_ctx_t *ctx) {
  const char *key = ctx->argv[1];
  const char *ttl_str = ctx->argv[2];

  int64_t seconds;
  if (!parse_seconds(ttl_str, &seconds)) {
    resp_add(&ctx->resp, "-ERR invalid expire time\n");
    return;
  }
  int64_t expire_at = server.now_realtime_ms + (seconds * 1000);
  if (db_key_expire(server.db, key, expire_at) == EXIT_FAILURE) {
    resp_add(&ctx->resp, "-ERR EXPIRE COMMAND\n");
    return;
  }
  aof_append(AOF_OP_EXPIRE, key, strlen(key), NULL, 0, expire_at);
  resp_add(&ctx->resp, "+OK\n");
}

static void cmd_persist(command_ctx_t *ctx) {
  const char *key = ctx->argv[1];

  if (db_persist(server.db, key) == EXIT_FAILURE) {
    resp_add(&ctx->resp, "-ERR PERSIST COMMAND\n");
    return;
  }
  aof_append(AOF_OP_PERSIST, key, strlen(key), NULL, 0, 0);
  resp_add(&ctx->resp, "+OK\n");
}

static void cmd_del(command_ctx_t *ctx) {
  const char *key = ctx->argv[1];

  if (db_del(server.db, key) == EXIT_FAILURE) {
    resp_add(&ctx->resp, "-ERR DEL COMMAND\n");
    return;
  }
  aof_append(AOF_OP_DEL, key, strlen(key), NULL, 0, 0);
  resp_add(&ctx->resp, "+OK\n");
}

static void cmd_keys(command_ctx_t *ctx) {
  ht_iter_t *iter = ht_iter_create(server.db->keyspace);
  ht_entry_t *entry;
  int count = 0;
  while ((entry = ht_iter_next(iter)) != NULL) {
    resp_addf(&ctx->resp, "%d) %s\n", count + 1, entry->key);
    count++;
  }
  free(iter);
}

static void cmd_info(command_ctx_t *ctx) {
  int64_t uptime_s = (server.now_ms - server.db->start_ms) / 1000;
  const char *rewrite_state =
      server.aof_rewrite_state == AOF_RW_IDLE       ? "idle"
      : server.aof_rewrite_state == AOF_RW_ACTIVE   ? "active"
      : server.aof_rewrite_state == AOF_RW_ABORTING ? "aborting"
                                                    : "unknown";
  resp_addf(&ctx->resp,
            "Uptime: %" PRId64 " seconds\n"
            "DB Size: %zu\n"
            "DB Keys Used: %zu\n"
            "AOF Writes: %" PRId64 "\n"
            "AOF Fsyncs: %" PRId64 "\n"
            "AOF Rewrite State: %s\n"
            "AOF Rewrites Completed: %" PRId64 "\n"
            "AOF Rewrites Failed: %" PRId64 "\n"
            "AOF Last Rewrite Duration: %" PRId64 " ms\n",
            uptime_s, server.db->keyspace->size, server.db->keyspace->used,
            server.aof_total_writes, server.aof_total_fsyncs, rewrite_state,
            server.aof_rewrites_completed, server.aof_rewrites_failed,
            server.aof_last_rewrite_duration_ms);
}

static void cmd_bgrewriteaof(command_ctx_t *ctx) {
  if (server.aof_rewrite_state != AOF_RW_IDLE) {
    resp_add(&ctx->resp, "-ERR rewrite already in progress\n");
    return;
  }
  if (aof_rewrite_start() == -1) {
    resp_add(&ctx->resp, "-ERR failed to start rewrite\n");
    return;
  }
  resp_add(&ctx->resp, "+OK background rewrite started\n");
}

/* command table */

static command_entry_t command_table[] = {
    {"GET", cmd_get, 2, 0},
    {"TTL", cmd_ttl, 2, 0},
    {"SET", cmd_set, -3, AOF_OP_SET},
    {"SETEX", cmd_setex, 4, AOF_OP_SETEX},
    {"EXPIRE", cmd_expire, 3, AOF_OP_EXPIRE},
    {"PERSIST", cmd_persist, 2, AOF_OP_PERSIST},
    {"DEL", cmd_del, 2, AOF_OP_DEL},
    {"KEYS", cmd_keys, 1, 0},
    {"INFO", cmd_info, 1, 0},
    {"BGREWRITEAOF", cmd_bgrewriteaof, 1, 0},
    {NULL, NULL, 0, 0},
};

static command_entry_t *lookup_command(const char *name) {
  for (int i = 0; command_table[i].name != NULL; i++) {
    if (strcasecmp(name, command_table[i].name) == 0)
      return &command_table[i];
  }
  return NULL;
}

/* tokenizer */

static int tokenize_command(char *cmd, char **argv, int max_tokens) {
  char *p = cmd;
  char **ap;
  int argc = 0;

  for (ap = argv; (*ap = strsep(&p, " \t")) != NULL;) {
    if (**ap != '\0') {
      argc++;
      if (++ap >= &argv[max_tokens])
        break;
    }
  }
  return argc;
}

/* public API */

size_t command_execute(const char *payload, size_t len, char *out,
                       size_t out_cap) {
  char cmd[MSG_MAX + 1];
  memcpy(cmd, payload, len);
  cmd[len] = '\0';

  char *argv[MAX_TOKENS];
  int argc = tokenize_command(cmd, argv, MAX_TOKENS);

  respbuf resp = {out, out_cap, 0};

  if (argc == 0) {
    resp_add(&resp, "-ERR empty command\n");
    return resp.len;
  }

  command_entry_t *entry = lookup_command(argv[0]);
  if (entry == NULL) {
    resp_add(&resp, "-ERR unknown command\n");
    return resp.len;
  }

  if ((entry->arity > 0 && argc != entry->arity) ||
      (entry->arity < 0 && argc < -(entry->arity))) {
    resp_add(&resp, "-ERR wrong number of arguments\n");
    return resp.len;
  }

  command_ctx_t ctx = {argc, argv, resp};
  entry->proc(&ctx);
  return ctx.resp.len;
}
