#ifndef COMMAND_H
#define COMMAND_H

#include <stddef.h>
#include <stdint.h>

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

typedef size_t kvCommandProc(command_ctx_t *ctx);

struct kvCommand {
    const char *name;
    kvCommandProc *proc;
    int arity;
};

size_t command_execute(const char *payload, size_t len, char *out, size_t out_cap);

#endif
