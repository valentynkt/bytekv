#ifndef DB_H
#define DB_H
#include "ht.h"
#include <stdint.h>

typedef struct db {
    ht_t *keyspace;
    ht_t *expires;
} db_t;

db_t *db_create(void);
void db_free(db_t *db);
int db_set(db_t *db, const char *key, const char *value);
int db_setex(db_t *db, const char *key, const char *value, int64_t expire_at);
char *db_get(db_t *db, const char *key);
int db_del(db_t *db, const char *key);

#endif
