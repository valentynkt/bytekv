#include "ht.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#define HT_INITIAL_SIZE 4
#define LOAD_FACTOR 0.75

ht_t *ht_create(void)
{
    ht_t *ht = malloc(sizeof(*ht));
    ht->size = HT_INITIAL_SIZE;
    ht->sizemask = ht->size - 1;
    ht->used = 0;
    ht->table = calloc(ht->size, sizeof(ht_entry_t *));
    return ht;
}

/* djb2 hash — returns raw hash, caller applies sizemask */
static size_t ht_hash(const char *key)
{
    size_t hash = 5381;
    int c;

    while ((c = *key++)) {
        hash = ((hash << 5) + hash) + c; /* hash * 33 + c */
    }
    return hash;
}

int ht_set(ht_t *ht, const char *key, const char *value)
{
    size_t index = ht_hash(key) & ht->sizemask;

    /* Walk chain — check if key already exists */
    ht_entry_t *entry = ht->table[index];
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
    ht_entry_t *new_entry = malloc(sizeof(*new_entry));
    if (new_entry == NULL) {
        return EXIT_FAILURE;
    }
    new_entry->key = strdup(key);
    new_entry->val = strdup(value);
    new_entry->next = ht->table[index];
    ht->table[index] = new_entry;
    ht->used++;
    if ((double)ht->used / ht->size > LOAD_FACTOR)
        ht_resize(ht, ht->size * 2);
    return EXIT_SUCCESS;
}

char *ht_get(ht_t *ht, const char *key)
{
    size_t index = ht_hash(key) & ht->sizemask;
    ht_entry_t *entry = ht->table[index];
    while (entry != NULL) {
        if (strcmp(entry->key, key) == 0)
            return entry->val;
        entry = entry->next;
    }
    return NULL;
}

int ht_del(ht_t *ht, const char *key)
{
    size_t index = ht_hash(key) & ht->sizemask;
    ht_entry_t *prev = NULL;
    ht_entry_t *entry = ht->table[index];
    while (entry != NULL) {
        if (strcmp(entry->key, key) == 0) {
            if (prev == NULL)
                ht->table[index] = entry->next;
            else
                prev->next = entry->next;
            free(entry->key);
            free(entry->val);
            free(entry);
            ht->used--;
            return EXIT_SUCCESS;
        }
        prev = entry;
        entry = entry->next;
    }
    return EXIT_FAILURE;
}

int ht_free(ht_t *ht)
{
    for (size_t i = 0; i < ht->size; i++) {
        ht_entry_t *entry = ht->table[i];
        while (entry != NULL) {
            free(entry->key);
            free(entry->val);
            ht_entry_t *next = entry->next;
            free(entry);
            entry = next;
        }
    }
    free(ht->table);
    free(ht);
    return EXIT_SUCCESS;
}

int ht_resize(ht_t *ht, size_t new_size)
{
    ht_entry_t **new_table = calloc(new_size, sizeof(ht_entry_t *));
    for (size_t i = 0; i < ht->size; i++) {
        ht_entry_t *entry = ht->table[i];
        while (entry != NULL) {
            size_t new_idx = ht_hash(entry->key) & (new_size - 1);
            ht_entry_t *next = entry->next;
            entry->next = new_table[new_idx];
            new_table[new_idx] = entry;
            entry = next;
        }
    }
    free(ht->table);
    ht->table = new_table;
    ht->size = new_size;
    ht->sizemask = new_size - 1;
    return EXIT_SUCCESS;
}

ht_iter_t *ht_iter_create(ht_t *ht)
{
    ht_iter_t *iter = malloc(sizeof(*iter));
    iter->ht = ht;
    iter->index = -1;
    iter->entry = NULL;
    iter->next_entry = NULL;
    return iter;
}

ht_entry_t *ht_iter_next(ht_iter_t *iter)
{
    while (true) {
        if (iter->entry == NULL) {
            iter->index++;
            if ((size_t)iter->index >= iter->ht->size) {
                break;
            }
            iter->entry = iter->ht->table[iter->index];
        } else {
            iter->entry = iter->next_entry;
        }
        if (iter->entry) {
            iter->next_entry = iter->entry->next;
            return iter->entry;
        }
    }
    return NULL;
}
