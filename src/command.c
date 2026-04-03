#include "command.h"
#include "db.h"
#include "util.h"
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#define MAX_TOKENS 8

static db_t *db = NULL;
static time_t start_time = 0;

/* respbuf helpers */

static void resp_add(respbuf *r, const char *s)
{
    size_t slen = strlen(s);
    size_t avail = r->cap - r->len;
    if (slen > avail)
        slen = avail;
    memcpy(r->buf + r->len, s, slen);
    r->len += slen;
}

__attribute__((format(printf, 2, 3))) static void resp_addf(respbuf *r, const char *fmt, ...)
{
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

/* command handlers */

static size_t cmd_get(command_ctx_t *ctx)
{
    char *val = db_get(db, ctx->argv[1]);
    if (val == NULL) {
        resp_add(&ctx->resp, "-ERR key not found\n");
        return ctx->resp.len;
    }
    resp_addf(&ctx->resp, "+%s\n", val);
    return ctx->resp.len;
}

static size_t cmd_set(command_ctx_t *ctx)
{
    int res = db_set(db, ctx->argv[1], ctx->argv[2]);
    if (res == EXIT_FAILURE)
        resp_add(&ctx->resp, "-ERR SET COMMAND\n");
    else
        resp_add(&ctx->resp, "+OK\n");
    return ctx->resp.len;
}
static size_t cmd_setex(command_ctx_t *ctx)
{
    /* TODO: parse TTL from ctx->argv[3], compute expire_at */
    int64_t expire_at = 0;
    int res = db_setex(db, ctx->argv[1], ctx->argv[2], expire_at);
    if (res == EXIT_FAILURE)
        resp_add(&ctx->resp, "-ERR SETEX COMMAND\n");
    else
        resp_add(&ctx->resp, "+OK\n");
    return ctx->resp.len;
}
static size_t cmd_keys(command_ctx_t *ctx)
{
    ht_iter_t *iter = ht_iter_create(db->keyspace);
    ht_entry_t *entry;
    int count = 0;
    while ((entry = ht_iter_next(iter)) != NULL) {
        resp_addf(&ctx->resp, "%d) %s\n", count + 1, entry->key);
        count++;
    }
    free(iter);
    return ctx->resp.len;
}

static size_t cmd_del(command_ctx_t *ctx)
{
    int res = db_del(db, ctx->argv[1]);
    if (res == EXIT_FAILURE)
        resp_add(&ctx->resp, "-ERR DEL COMMAND\n");
    else
        resp_add(&ctx->resp, "+OK\n");
    return ctx->resp.len;
}

static size_t cmd_info(command_ctx_t *ctx)
{
    time_t uptime = time(NULL) - start_time;
    resp_addf(&ctx->resp,
              "Uptime: %ld seconds\n"
              "DB Size: %d\n"
              "DB Keys Used: %d\n",
              (long)uptime, (int)db->keyspace->size, (int)db->keyspace->used);
    return ctx->resp.len;
}

static struct kvCommand kvCommandTable[] = {
    {"GET", cmd_get, 2},   {"SET", cmd_set, -3},  {"SETEX", cmd_setex, -4}, {"DEL", cmd_del, 2},
    {"KEYS", cmd_keys, 1}, {"INFO", cmd_info, 1}, {NULL, NULL, 0},
};

static struct kvCommand *lookupCommand(const char *name)
{
    for (int i = 0; kvCommandTable[i].name != NULL; i++) {
        if (strcasecmp(name, kvCommandTable[i].name) == 0)
            return &kvCommandTable[i];
    }
    return NULL;
}

// tokenizer
static int tokenize_command(char *cmd, char **argv, int max_tokens)
{
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

// dispatch
size_t command_execute(const char *payload, size_t len, char *out, size_t out_cap)
{
    if (db == NULL) {
        db = db_create();
        start_time = time(NULL);
    }
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

    struct kvCommand *command = lookupCommand(argv[0]);
    if (command == NULL) {
        resp_add(&resp, "-ERR unknown command\n");
        return resp.len;
    }

    if ((command->arity > 0 && argc != command->arity) ||
        (command->arity < 0 && argc < -(command->arity))) {
        resp_add(&resp, "-ERR wrong number of arguments\n");
        return resp.len;
    }

    command_ctx_t ctx = {argc, argv, resp};
    return command->proc(&ctx);
}
