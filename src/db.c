#include "db.h"
#include <stdlib.h>

db_t *db_create(void)
{
    db_t *db = malloc(sizeof(*db));
    db->keyspace = ht_create(HT_VAL_STR);
    db->expires = ht_create(HT_VAL_I64);
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
    return ht_set_str(db->keyspace, key, value);
}

int db_setex(db_t *db, const char *key, const char *value, int64_t expire_at)
{
    if (ht_set_str(db->keyspace, key, value) == EXIT_FAILURE)
        return EXIT_FAILURE;
    if (expire_at > 0)
        ht_set_i64(db->expires, key, expire_at);
    return EXIT_SUCCESS;
}

char *db_get(db_t *db, const char *key)
{
    return ht_get_str(db->keyspace, key);
}

int db_del(db_t *db, const char *key)
{
    ht_del(db->expires, key);
    return ht_del(db->keyspace, key);
}
