#include "db.h"
#include <stdlib.h>

db_t *db_create(void)
{
    db_t *db = malloc(sizeof(*db));
    db->keyspace = ht_create();
    db->expires = ht_create();
    return db;
}

void db_free(db_t *db)
{
    ht_free(db->keyspace);
    ht_free(db->expires);
    free(db);
}

int db_set(db_t *db, const char *key, const char *value)
{
    return ht_set(db->keyspace, key, value);
}

char *db_get(db_t *db, const char *key)
{
    return ht_get(db->keyspace, key);
}

int db_del(db_t *db, const char *key)
{
    ht_del(db->expires, key);
    return ht_del(db->keyspace, key);
}
