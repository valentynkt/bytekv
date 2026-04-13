#include "command.h"
#include "server.h"
#include "util.h"
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

  char *val = db_get(server.db, key);
  if (val == NULL) {
    resp_add(&ctx->resp, "-ERR key not found\n");
    return;
  }
  resp_addf(&ctx->resp, "+%s\n", val);
}

static void cmd_ttl(command_ctx_t *ctx) {
  const char *key = ctx->argv[1];

  int64_t ttl = db_get_ttl(server.db, key);
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

  if (db_set(server.db, key, value) == EXIT_FAILURE)
    resp_add(&ctx->resp, "-ERR SET COMMAND\n");
  else
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
  int64_t expire_at = server.db->now_ms + (seconds * 1000);
  if (db_setex(server.db, key, value, expire_at) == EXIT_FAILURE)
    resp_add(&ctx->resp, "-ERR SETEX COMMAND\n");
  else
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
  int64_t expire_at = server.db->now_ms + (seconds * 1000);
  if (db_key_expire(server.db, key, expire_at) == EXIT_FAILURE)
    resp_add(&ctx->resp, "-ERR EXPIRE COMMAND\n");
  else
    resp_add(&ctx->resp, "+OK\n");
}

static void cmd_persist(command_ctx_t *ctx) {
  const char *key = ctx->argv[1];

  if (db_persist(server.db, key) == EXIT_FAILURE)
    resp_add(&ctx->resp, "-ERR PERSIST COMMAND\n");
  else
    resp_add(&ctx->resp, "+OK\n");
}

static void cmd_del(command_ctx_t *ctx) {
  const char *key = ctx->argv[1];

  if (db_del(server.db, key) == EXIT_FAILURE)
    resp_add(&ctx->resp, "-ERR DEL COMMAND\n");
  else
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
  int64_t uptime_s = (server.db->now_ms - server.db->start_ms) / 1000;
  resp_addf(&ctx->resp,
            "Uptime: %" PRId64 " seconds\n"
            "DB Size: %zu\n"
            "DB Keys Used: %zu\n",
            uptime_s, server.db->keyspace->size, server.db->keyspace->used);
}

/* command table */

static command_entry_t command_table[] = {
    {"GET", cmd_get, 2},       {"TTL", cmd_ttl, 2},
    {"SET", cmd_set, -3},      {"SETEX", cmd_setex, 4},
    {"EXPIRE", cmd_expire, 3}, {"PERSIST", cmd_persist, 2},
    {"DEL", cmd_del, 2},       {"KEYS", cmd_keys, 1},
    {"INFO", cmd_info, 1},     {NULL, NULL, 0},
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

#define ACTIVE_EXPIRE_TRESHOLD_PERS 25
#define ACTIVE_EXPIRE_NUM_ITEMS 10
#define ACTIVE_EXPIRE_MS_TIMEOUT 5000

void active_expire(void) {
  printf("[LOG] Active Expiry Started\n");
  server.db->now_ms = now_ms();
  int64_t start = server.db->now_ms;
  int64_t current = start;
  while (current - start < ACTIVE_EXPIRE_MS_TIMEOUT) {
    size_t expired = db_active_sweep(server.db, ACTIVE_EXPIRE_NUM_ITEMS);
    bool is_below_treshold =
        expired * 100 / ACTIVE_EXPIRE_NUM_ITEMS < ACTIVE_EXPIRE_TRESHOLD_PERS;
    if (is_below_treshold) {
      break;
    }
    current = now_ms();
  }
}

void command_init(void) {}

void command_shutdown(void) {}

size_t command_execute(const char *payload, size_t len, char *out,
                       size_t out_cap) {
  server.db->now_ms = now_ms();

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
