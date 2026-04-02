#ifndef DICT_H
#define DICT_H
#include <stddef.h>

typedef struct dictEntry {
    char *key;
    char *val;
    struct dictEntry *next;
} dictEntry;

typedef struct dictht {
    dictEntry **table;
    size_t size;
    size_t sizemask;
    size_t used;
} dictht;
typedef struct dictIterator {
    dictht *db;
    int index;
    dictEntry *entry;
    dictEntry *nextEntry;
} dictIterator;

dictht *dictCreate(void);
int dictSet(dictht *d, const char *key, const char *value);
char *dictGet(dictht *d, const char *key);
int dictDel(dictht *d, const char *key);
int dictFree(dictht *d);
int dictResize(dictht *d, size_t new_size);

dictIterator *dictGetIterator(dictht *db);
dictEntry *dictIteratorNext(dictIterator *iterator);

#endif
