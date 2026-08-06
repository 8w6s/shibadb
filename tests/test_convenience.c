/*
 * Coverage for the SDB_ENGINE_API_VERSION 2 convenience layer: allocating get,
 * existence, count, put-if-absent, compare-and-swap, increment, and atomic
 * batches. Uses only the public engine API.
 */

#include "shibadb_engine.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static const uint8_t NS[] = "items";
#define NS_SIZE (sizeof(NS) - 1U)

static void cleanup(const char *path)
{
    char sidecar[256];
    remove(path);
    (void)snprintf(sidecar, sizeof(sidecar), "%s.lock", path);
    remove(sidecar);
    (void)snprintf(sidecar, sizeof(sidecar), "%s.wal", path);
    remove(sidecar);
}

static void put(sdb_database *db, const char *key, const char *value)
{
    assert(sdb_kv_put(db, NS, NS_SIZE, (const uint8_t *)key, strlen(key),
                      (const uint8_t *)value, strlen(value)) == SDB_OK);
}

static void put_i64(sdb_database *db, const char *key, int64_t value)
{
    uint8_t buffer[8];
    int i;
    uint64_t raw = (uint64_t)value;
    for (i = 0; i < 8; ++i) {
        buffer[i] = (uint8_t)((raw >> (8 * i)) & 0xFFU);
    }
    assert(sdb_kv_put(db, NS, NS_SIZE, (const uint8_t *)key, strlen(key),
                      buffer, sizeof(buffer)) == SDB_OK);
}

typedef struct query_ctx {
    int count;
    size_t total_body;
} query_ctx;

static bool query_visitor(
    void *context,
    const uint8_t *document_id,
    size_t document_id_size,
    const uint8_t *document,
    size_t document_size
)
{
    query_ctx *q = (query_ctx *)context;
    (void)document_id;
    (void)document_id_size;
    (void)document;
    q->count++;
    q->total_body += document_size;
    return true;
}

int main(void)
{
    const char *path = "test_convenience.shiba";
    sdb_database_options options;
    sdb_database *db = NULL;
    sdb_status st;
    bool exists = false;
    uint64_t count = 0U;
    uint8_t *got = NULL;
    size_t got_size = 0U;
    int64_t counter = 0;

    cleanup(path);
    sdb_database_options_init(&options);
    assert(sdb_database_create(path, &options, &db) == SDB_OK);

    /* --- get_alloc + exists --- */
    put(db, "alpha", "hello");
    st = sdb_kv_get_alloc(db, NS, NS_SIZE, (const uint8_t *)"alpha", 5U,
                          &got, &got_size);
    assert(st == SDB_OK);
    assert(got_size == 5U && memcmp(got, "hello", 5U) == 0);
    sdb_free(got);
    got = NULL;

    /* missing key */
    assert(sdb_kv_get_alloc(db, NS, NS_SIZE, (const uint8_t *)"missing", 7U,
                            &got, &got_size) == SDB_E_NOT_FOUND);
    assert(got == NULL && got_size == 0U);

    /* zero-length value: non-NULL buffer, size 0 */
    assert(sdb_kv_put(db, NS, NS_SIZE, (const uint8_t *)"empty", 5U,
                      (const uint8_t *)"", 0U) == SDB_OK);
    assert(sdb_kv_get_alloc(db, NS, NS_SIZE, (const uint8_t *)"empty", 5U,
                            &got, &got_size) == SDB_OK);
    assert(got != NULL && got_size == 0U);
    sdb_free(got);
    got = NULL;

    assert(sdb_kv_exists(db, NS, NS_SIZE, (const uint8_t *)"alpha", 5U,
                         &exists) == SDB_OK && exists);
    assert(sdb_kv_exists(db, NS, NS_SIZE, (const uint8_t *)"missing", 7U,
                         &exists) == SDB_OK && !exists);

    /* --- count / count_prefix --- */
    put(db, "user:1", "a");
    put(db, "user:2", "b");
    put(db, "user:3", "c");
    assert(sdb_kv_count(db, NS, NS_SIZE, &count) == SDB_OK);
    assert(count == 5U); /* alpha, empty, user:1..3 */
    assert(sdb_kv_count_prefix(db, NS, NS_SIZE, (const uint8_t *)"user:", 5U,
                               &count) == SDB_OK);
    assert(count == 3U);

    /* --- put_if_absent --- */
    assert(sdb_kv_put_if_absent(db, NS, NS_SIZE, (const uint8_t *)"cas", 3U,
                                (const uint8_t *)"v1", 2U) == SDB_OK);
    assert(sdb_kv_put_if_absent(db, NS, NS_SIZE, (const uint8_t *)"cas", 3U,
                                (const uint8_t *)"v2", 2U) == SDB_E_CONFLICT);
    st = sdb_kv_get_alloc(db, NS, NS_SIZE, (const uint8_t *)"cas", 3U,
                          &got, &got_size);
    assert(st == SDB_OK && got_size == 2U && memcmp(got, "v1", 2U) == 0);
    sdb_free(got);
    got = NULL;

    /* --- compare_and_swap --- */
    /* wrong expected -> conflict, value unchanged */
    assert(sdb_kv_compare_and_swap(db, NS, NS_SIZE, (const uint8_t *)"cas", 3U,
                                   (const uint8_t *)"XX", 2U,
                                   (const uint8_t *)"v3", 2U)
           == SDB_E_CONFLICT);
    /* correct expected -> swap */
    assert(sdb_kv_compare_and_swap(db, NS, NS_SIZE, (const uint8_t *)"cas", 3U,
                                   (const uint8_t *)"v1", 2U,
                                   (const uint8_t *)"v3", 2U) == SDB_OK);
    st = sdb_kv_get_alloc(db, NS, NS_SIZE, (const uint8_t *)"cas", 3U,
                          &got, &got_size);
    assert(st == SDB_OK && got_size == 2U && memcmp(got, "v3", 2U) == 0);
    sdb_free(got);
    got = NULL;
    /* absent key -> conflict */
    assert(sdb_kv_compare_and_swap(db, NS, NS_SIZE, (const uint8_t *)"nope", 4U,
                                   (const uint8_t *)"", 0U,
                                   (const uint8_t *)"x", 1U)
           == SDB_E_CONFLICT);

    /* --- increment --- */
    assert(sdb_kv_increment(db, NS, NS_SIZE, (const uint8_t *)"ctr", 3U,
                            5, &counter) == SDB_OK && counter == 5);
    assert(sdb_kv_increment(db, NS, NS_SIZE, (const uint8_t *)"ctr", 3U,
                            -2, &counter) == SDB_OK && counter == 3);
    /* non-counter value rejected */
    put(db, "notctr", "abc");
    assert(sdb_kv_increment(db, NS, NS_SIZE, (const uint8_t *)"notctr", 6U,
                            1, NULL) == SDB_E_INVALID_ARGUMENT);
    /* overflow */
    put_i64(db, "max", INT64_MAX);
    assert(sdb_kv_increment(db, NS, NS_SIZE, (const uint8_t *)"max", 3U,
                            1, NULL) == SDB_E_OVERFLOW);

    /* --- batch_apply (atomic) --- */
    {
        sdb_batch_op ops[3];
        memset(ops, 0, sizeof(ops));
        ops[0].kind = SDB_BATCH_OP_KV_PUT;
        ops[0].namespace_name = NS; ops[0].namespace_size = NS_SIZE;
        ops[0].key = (const uint8_t *)"b1"; ops[0].key_size = 2U;
        ops[0].value = (const uint8_t *)"1"; ops[0].value_size = 1U;
        ops[1].kind = SDB_BATCH_OP_KV_PUT;
        ops[1].namespace_name = NS; ops[1].namespace_size = NS_SIZE;
        ops[1].key = (const uint8_t *)"b2"; ops[1].key_size = 2U;
        ops[1].value = (const uint8_t *)"2"; ops[1].value_size = 1U;
        ops[2].kind = SDB_BATCH_OP_KV_DELETE;
        ops[2].namespace_name = NS; ops[2].namespace_size = NS_SIZE;
        ops[2].key = (const uint8_t *)"alpha"; ops[2].key_size = 5U;
        assert(sdb_kv_batch_apply(db, ops, 3U) == SDB_OK);
        assert(sdb_kv_exists(db, NS, NS_SIZE, (const uint8_t *)"b1", 2U,
                             &exists) == SDB_OK && exists);
        assert(sdb_kv_exists(db, NS, NS_SIZE, (const uint8_t *)"alpha", 5U,
                             &exists) == SDB_OK && !exists);
    }
    /* empty batch is a no-op */
    assert(sdb_kv_batch_apply(db, NULL, 0U) == SDB_OK);
    /* bad op kind -> whole batch rejected, nothing written */
    {
        sdb_batch_op bad[2];
        memset(bad, 0, sizeof(bad));
        bad[0].kind = SDB_BATCH_OP_KV_PUT;
        bad[0].namespace_name = NS; bad[0].namespace_size = NS_SIZE;
        bad[0].key = (const uint8_t *)"rollback"; bad[0].key_size = 8U;
        bad[0].value = (const uint8_t *)"x"; bad[0].value_size = 1U;
        bad[1].kind = 999U; /* invalid */
        assert(sdb_kv_batch_apply(db, bad, 2U) == SDB_E_INVALID_ARGUMENT);
        assert(sdb_kv_exists(db, NS, NS_SIZE, (const uint8_t *)"rollback", 8U,
                             &exists) == SDB_OK && !exists);
    }

    /* --- index_query_documents (find-by-field returning bodies) --- */
    {
        const uint8_t coll[] = "people";
        const uint8_t idx[] = "by_role";
        sdb_index_term term;
        query_ctx qc;
        size_t matched = 0U;
        assert(sdb_index_create(db, coll, sizeof(coll) - 1U,
                                idx, sizeof(idx) - 1U, false) == SDB_OK);
        term.index_name = idx;
        term.index_name_size = sizeof(idx) - 1U;
        term.value = (const uint8_t *)"admin";
        term.value_size = 5U;
        assert(sdb_document_put(db, coll, sizeof(coll) - 1U,
                                (const uint8_t *)"u1", 2U,
                                (const uint8_t *)"{\"n\":1}", 7U,
                                &term, 1U) == SDB_OK);
        assert(sdb_document_put(db, coll, sizeof(coll) - 1U,
                                (const uint8_t *)"u2", 2U,
                                (const uint8_t *)"{\"n\":2}", 7U,
                                &term, 1U) == SDB_OK);
        term.value = (const uint8_t *)"user";
        term.value_size = 4U;
        assert(sdb_document_put(db, coll, sizeof(coll) - 1U,
                                (const uint8_t *)"u3", 2U,
                                (const uint8_t *)"{\"n\":3}", 7U,
                                &term, 1U) == SDB_OK);

        qc.count = 0;
        qc.total_body = 0U;
        assert(sdb_index_query_documents(
                   db, coll, sizeof(coll) - 1U, idx, sizeof(idx) - 1U,
                   (const uint8_t *)"admin", 5U, query_visitor, &qc, &matched)
               == SDB_OK);
        assert(matched == 2U && qc.count == 2 && qc.total_body == 14U);

        qc.count = 0;
        qc.total_body = 0U;
        assert(sdb_index_query_documents(
                   db, coll, sizeof(coll) - 1U, idx, sizeof(idx) - 1U,
                   (const uint8_t *)"user", 4U, query_visitor, &qc, &matched)
               == SDB_OK);
        assert(matched == 1U && qc.count == 1);
    }

    assert(sdb_database_close(db) == SDB_OK);
    cleanup(path);
    (void)puts("convenience tests: ok");
    return 0;
}
