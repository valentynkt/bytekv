#include "command.h"
#include "dict.h"
#include "util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#define MAX_TOKENS 8
#define LOAD_FACTOR 0.75

static dictht *db = NULL;
static size_t write_resp(char *out, size_t out_cap, const char *resp)
{
    size_t len = strlen(resp);
    if (len > out_cap)
        // remaining place
        len = out_cap;
    memcpy(out, resp, len);
    return len;
}

static size_t cmd_get(command_ctx_t *ctx)
{
    char *val = dictGet(db, ctx->argv[1]);
    if (val == NULL)
        return write_resp(ctx->out, ctx->out_cap, "-ERR key not found\n");

    return (size_t)snprintf(ctx->out, ctx->out_cap, "+%s\n", val);
}

static size_t cmd_set(command_ctx_t *ctx)
{
    int res = dictSet(db, ctx->argv[1], ctx->argv[2]);
    if (res == EXIT_FAILURE)
        return write_resp(ctx->out, ctx->out_cap, "-ERR SET COMMAND\n");
    return write_resp(ctx->out, ctx->out_cap, "+OK\n");
}
static size_t cmd_keys(command_ctx_t *ctx)
{
    dictIterator *iter = dictGetIterator(db);
    dictEntry *db_elem;
    size_t len = 0;
    int count = 0;
    while ((db_elem = dictIteratorNext(iter)) != NULL) {
        int n = snprintf(ctx->out + len, ctx->out_cap - len, "%d) %s\n", count + 1, db_elem->key);
        if (n > 0) {
            len += (size_t)n;
        }
        count++;
    }
    free(iter);
    return len;
}

static size_t cmd_del(command_ctx_t *ctx)
{
    int res = dictDel(db, ctx->argv[1]);
    if (res == EXIT_FAILURE) {
        return write_resp(ctx->out, ctx->out_cap, "-ERR DEL COMMAND\n");
    }
    return write_resp(ctx->out, ctx->out_cap, "+OK\n");
}

static struct kvCommand kvCommandTable[] = {
    {"GET", cmd_get, 2},   {"SET", cmd_set, -3}, {"DEL", cmd_del, 2},
    {"KEYS", cmd_keys, 1}, {NULL, NULL, 0},
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
        db = dictCreate();
    }
    char cmd[MSG_MAX + 1];
    memcpy(cmd, payload, len);
    cmd[len] = '\0';

    char *argv[MAX_TOKENS];
    int argc = tokenize_command(cmd, argv, MAX_TOKENS);

    if (argc == 0)
        return write_resp(out, out_cap, "-ERR empty command\n");

    struct kvCommand *command = lookupCommand(argv[0]);
    if (command == NULL)
        return write_resp(out, out_cap, "-ERR unknown command\n");

    if ((command->arity > 0 && argc != command->arity) ||
        (command->arity < 0 && argc < -(command->arity)))
        return write_resp(out, out_cap, "-ERR wrong number of arguments\n");

    command_ctx_t ctx = {argc, argv, out, out_cap};
    return command->proc(&ctx);
}
