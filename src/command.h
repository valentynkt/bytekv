#ifndef COMMAND_H
#define COMMAND_H

#include <stddef.h>

size_t command_execute(const char *payload, size_t len, char *out,
                       size_t out_cap);

#endif
