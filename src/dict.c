#include "dict.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define DICT_INITIAL_SIZE 4

dictht *dictCreate(void)
{
    dictht *dict = malloc(sizeof(*dict));
    dict->size = DICT_INITIAL_SIZE;
    dict->sizemask = dict->size - 1;
    dict->used = 0;
    dict->table = calloc(dict->size, sizeof(dictEntry *));
    return dict;
}
/* djb2 hash — returns raw hash, caller applies sizemask */
static size_t dictGenHashFn(const char *key)
{
    size_t hash = 5381;
    int c;

    while ((c = *key++)) {
        c = tolower(c);
        hash = ((hash << 5) + hash) + c; /* hash * 33 + c */
    }
    return hash;
}

int dictSet(dictht *d, const char *key, const char *value)
{
    size_t index = dictGenHashFn(key) & d->sizemask;

    /* Walk chain — check if key already exists */
    dictEntry *entry = d->table[index];
    while (entry != NULL) {
        if (strcasecmp(entry->key, key) == 0) {
            /* UPDATE: key exists, replace value */
            free(entry->val);
            entry->val = strdup(value);
            return 0;
        }
        entry = entry->next;
    }

    /* INSERT: key not found, prepend to chain head */
    dictEntry *new_entry = malloc(sizeof(*new_entry));
    new_entry->key = strdup(key);
    new_entry->val = strdup(value);
    new_entry->next = d->table[index];
    d->table[index] = new_entry;
    d->used++;
    return 0;
}
