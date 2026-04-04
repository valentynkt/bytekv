#include "db.h"
#include "ht.h"
#include "util.h"
#include <stdint.h>
#include <stdlib.h>

db_t *db_create(void)
{
    db_t *db = malloc(sizeof(*db));
    db->keyspace = ht_create(HT_VAL_STR);
    db->expires = ht_create(HT_VAL_I64);
    db->now_ms = now_ms();
    db->start_ms = db->now_ms;
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
// ToDo: Probably need better handling, currently returns the same error for the key that does not
// exist at all in expires table, the key that was already expired? and the the key that is not
// expired but error happend so need retry.
int db_persist(db_t *db, const char *key)
{
    return ht_del(db->expires, key);
}

int db_key_expire(db_t *db, const char *key, int64_t expire_at)
{
    if (expire_at <= 0 || ht_get_str(db->keyspace, key) == NULL)
        return EXIT_FAILURE;
    return ht_set_i64(db->expires, key, expire_at);
}
// Errors: -1 : No TTL, -2 : Key don't exist
int64_t db_get_ttl(db_t *db, const char *key)
{
    if (db_get(db, key) == NULL) {
        return -2;
    }
    int64_t *expire_at = ht_get_i64(db->expires, key);
    if (expire_at == NULL) {
        return -1;
    }
    return (*expire_at - db->now_ms) / 1000;
}

char *db_get(db_t *db, const char *key)
{
    // Lazy expirations
    int64_t *expire_at = ht_get_i64(db->expires, key);
    if (expire_at && db->now_ms > *expire_at) {
        db_del(db, key);
        return NULL;
    }
    return ht_get_str(db->keyspace, key);
}

int db_del(db_t *db, const char *key)
{
    ht_del(db->expires, key);
    return ht_del(db->keyspace, key);
}
