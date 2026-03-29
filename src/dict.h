
#include <stddef.h>

typedef struct dictEntry {
    char *key;
    char *val;
    struct dictEntry *next;
} dictEntry;

typedef struct dictht {
    dictEntry **table;
    size_t size;
    size_t used;
} dictht;
