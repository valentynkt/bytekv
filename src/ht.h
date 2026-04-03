#ifndef HT_H
#define HT_H
#include <stddef.h>

typedef struct ht_entry {
    char *key;
    char *val;
    struct ht_entry *next;
} ht_entry_t;

typedef struct ht {
    ht_entry_t **table;
    size_t size;
    size_t sizemask;
    size_t used;
} ht_t;

typedef struct ht_iter {
    ht_t *ht;
    int index;
    ht_entry_t *entry;
    ht_entry_t *next_entry;
} ht_iter_t;

ht_t *ht_create(void);
int ht_set(ht_t *ht, const char *key, const char *value);
char *ht_get(ht_t *ht, const char *key);
int ht_del(ht_t *ht, const char *key);
int ht_free(ht_t *ht);
int ht_resize(ht_t *ht, size_t new_size);

ht_iter_t *ht_iter_create(ht_t *ht);
ht_entry_t *ht_iter_next(ht_iter_t *iter);

#endif
