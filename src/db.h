#ifndef DB_H
#define DB_H
#include "ht.h"
#include <stddef.h>
#include <stdint.h>

typedef struct db {
  ht_t *keyspace;
  ht_t *expires;
  int64_t start_ms;
} db_t;

db_t *db_create(void);
void db_free(db_t *db);
int db_set(db_t *db, const char *key, const char *value);
int db_setex(db_t *db, const char *key, const char *value, int64_t expire_at);
char *db_get(db_t *db, const char *key);
int db_del(db_t *db, const char *key);
int db_key_expire(db_t *db, const char *key, int64_t expire_at);
int64_t db_get_ttl(db_t *db, const char *key);
int db_persist(db_t *db, const char *key);
size_t db_active_sweep(db_t *db, size_t sample_size);
#endif
