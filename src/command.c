#include "command.h"
#include "util.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>

#define MAX_TOKENS 8

static size_t write_resp(char *out, size_t out_cap, const char *resp)
{
    size_t len = strlen(resp);
    if (len > out_cap)
        len = out_cap;
    memcpy(out, resp, len);
    return len;
}

static size_t cmd_get(command_ctx_t *ctx)
{
    printf("GET key=%s\n", ctx->argv[1]);
    return write_resp(ctx->out, ctx->out_cap, "-ERR not implemented\n");
}

static size_t cmd_set(command_ctx_t *ctx)
{
    printf("SET key=%s value=%s\n", ctx->argv[1], ctx->argv[2]);
    return write_resp(ctx->out, ctx->out_cap, "+OK\n");
}

static struct kvCommand kvCommandTable[] = {
    {"GET", cmd_get, 2},
    {"SET", cmd_set, -3},
    {NULL, NULL, 0},
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
