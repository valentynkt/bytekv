#include "dict.h"
#include <ctype.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define DICT_INITIAL_SIZE 4
#define LOAD_FACTOR 0.75
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
        if (strcmp(entry->key, key) == 0) {
            /* UPDATE: key exists, replace value */
            free(entry->val);
            entry->val = strdup(value);
            return EXIT_SUCCESS;
        }
        entry = entry->next;
    }

    /* INSERT: key not found, prepend to chain head */
    dictEntry *new_entry = malloc(sizeof(*new_entry));
    if (new_entry == NULL) {
        return EXIT_FAILURE;
    }
    new_entry->key = strdup(key);
    new_entry->val = strdup(value);
    new_entry->next = d->table[index];
    d->table[index] = new_entry;
    d->used++;
    if ((double)d->used / d->size > LOAD_FACTOR)
        dictResize(d, d->size * 2);
    return EXIT_SUCCESS;
}

char *dictGet(dictht *d, const char *key)
{
    size_t index = dictGenHashFn(key) & d->sizemask;
    dictEntry *entry = d->table[index];
    while (entry != NULL) {
        if (strcasecmp(entry->key, key) == 0)
            return entry->val;
        entry = entry->next;
    }
    return NULL;
}

int dictDel(dictht *d, const char *key)
{
    size_t index = dictGenHashFn(key) & d->sizemask;
    dictEntry *prevEntry = NULL;
    dictEntry *entry = d->table[index];
    while (entry != NULL) {
        if (strcasecmp(entry->key, key) == 0) {
            if (prevEntry == NULL)
                d->table[index] = entry->next;
            else
                prevEntry->next = entry->next;
            free(entry->key);
            free(entry->val);
            free(entry);
            d->used--;
            return EXIT_SUCCESS;
        }
        prevEntry = entry;
        entry = entry->next;
    }
    return EXIT_FAILURE;
}
int dictFree(dictht *d)
{
    for (size_t i = 0; i < d->size; i++) {
        dictEntry *entry = d->table[i];
        while (entry != NULL) {
            free(entry->key);
            free(entry->val);
            dictEntry *next = entry->next;
            free(entry);
            entry = next;
        }
    }
    free(d->table);
    free(d);
    return EXIT_SUCCESS;
}

int dictResize(dictht *d, size_t new_size)
{

    dictEntry **new_table = calloc(new_size, sizeof(dictEntry *));
    for (size_t i = 0; i < d->size; i++) {
        dictEntry *entry = d->table[i];
        while (entry != NULL) {

            size_t new_idx = dictGenHashFn(entry->key) & (new_size - 1);
            dictEntry *nextEntry = entry->next;
            entry->next = new_table[new_idx];
            new_table[new_idx] = entry;
            entry = nextEntry;
        }
    }
    free(d->table);
    d->table = new_table;
    d->size = new_size;
    d->sizemask = new_size - 1;
    return EXIT_SUCCESS;
}

dictIterator *dictGetIterator(dictht *db)
{
    dictIterator *iter = malloc(sizeof(*iter));

    iter->db = db;
    iter->index = -1;
    iter->entry = NULL;
    iter->nextEntry = NULL;
    return iter;
}

dictEntry *dictIteratorNext(dictIterator *iter)
{
    while (true) {
        if (iter->entry == NULL) {
            iter->index++;
            if ((size_t)iter->index >= iter->db->size) {
                break;
            }
            iter->entry = iter->db->table[iter->index];
        } else {
            iter->entry = iter->nextEntry;
        }
        if (iter->entry) {
            iter->nextEntry = iter->entry->next;
            return iter->entry;
        }
    }
    return NULL;
}
