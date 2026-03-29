#include "command.h"
#include "util.h"
#include <stdio.h>
#include <string.h>
#define MAX_TOKENS 8
static int tokenize_command(char *cmd, char **argv, int max_tokens)
{
    char *p = cmd;
    char **ap;
    int argc = 0;

    for (ap = argv; (*ap = strsep(&p, " \t")) != NULL;) {

        if (**ap != '\0') {
            argc++;
            if (++ap >= &argv[max_tokens]) {
                break;
            }
        }
    }
    return argc;
}

size_t command_execute(const char *payload, size_t len, char *out, size_t out_cap)
{
    printf("cmd: '%.*s'\n", (int)len, payload);
    char cmd[MSG_MAX + 1];
    memcpy(cmd, payload, len);
    cmd[len] = '\0';
    char *argv[MAX_TOKENS];
    int argc = tokenize_command(cmd, argv, MAX_TOKENS);
    if (argc < 2) {
        printf("argc is wrong: %d\n", argc);
    }
    const char *resp = "+OK\n";

    size_t rlen = strlen(resp);
    if (rlen > out_cap)
        rlen = out_cap;
    memcpy(out, resp, rlen);
    return rlen;
}
