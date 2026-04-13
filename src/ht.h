#ifndef HT_H
#define HT_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum { HT_VAL_STR, HT_VAL_I64 } ht_val_type_t;

typedef union {
  char *str;
  int64_t i64;
} ht_val_t;

typedef struct ht_entry {
  char *key;
  ht_val_t val;
  struct ht_entry *next;
} ht_entry_t;

typedef struct ht {
  ht_entry_t **table;
  ht_val_type_t val_type;
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

ht_t *ht_create(ht_val_type_t val_type);

int ht_set_str(ht_t *ht, const char *key, const char *value);
int ht_set_i64(ht_t *ht, const char *key, int64_t value);
char *ht_get_str(ht_t *ht, const char *key);
int64_t *ht_get_i64(ht_t *ht, const char *key);

bool ht_exists(ht_t *ht, const char *key);
int ht_del(ht_t *ht, const char *key);
int ht_free(ht_t *ht);
int ht_resize(ht_t *ht, size_t new_size);

ht_entry_t *ht_get_bucket(ht_t *ht, const size_t idx);

ht_iter_t *ht_iter_create(ht_t *ht);
ht_entry_t *ht_iter_next(ht_iter_t *iter);

#endif
