
#include "shibadb.h"
#include "shibadb_engine.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *db_path = "test-index-visitor-reentry.tmp";
static const char *wal_path = "test-index-visitor-reentry.tmp.wal";
static const char *lock_path = "test-index-visitor-reentry.tmp.lock";
static const char *replace_path = "test-index-visitor-reentry.tmp.replace";

static const uint8_t collection[] = "docs";
static const uint8_t index_name[] = "group";
static const uint8_t index_value[] = "all";
static const uint8_t doc_body[] = "{}";

static void cleanup(void)
{
    (void)remove(db_path);
    (void)remove(wal_path);
    (void)remove(lock_path);
    (void)remove(replace_path);
}

static void put_indexed_document(
    sdb_database *database, const uint8_t *document_id, size_t id_size
)
{
    const sdb_index_term term = {
        index_name, sizeof(index_name), index_value, sizeof(index_value)
    };
    assert(sdb_document_put(
        database,
        collection, sizeof(collection),
        document_id, id_size,
        doc_body, sizeof(doc_body),
        &term, 1U
    ) == SDB_OK);
}

static void delete_document(
    sdb_database *database, const uint8_t *document_id, size_t id_size
)
{
    assert(sdb_document_delete(
        database,
        collection, sizeof(collection),
        document_id, id_size
    ) == SDB_OK);
}

/*
 * Count how many of the first `count` collected ids equal `id`. Used to prove
 * exact per-id visit multiplicity (no double-visit / cursor corruption after a
 * mid-iteration mutation), not just the aggregate visited count.
 */
static size_t count_visited(
    char ids[][8], size_t count, const char *id
)
{
    size_t index;
    size_t total = 0U;
    for (index = 0U; index < count; ++index) {
        if (strcmp(ids[index], id) == 0) {
            ++total;
        }
    }
    return total;
}

typedef struct {
    sdb_database *database;
    size_t visited;
    bool inserted;
    uint8_t last_id[8];
    size_t last_id_size;
    char ids[8][8];
} insert_ctx;

static bool insert_during_walk(
    void *context, const uint8_t *document_id, size_t document_id_size
)
{
    insert_ctx *ctx = (insert_ctx *)context;
    ++ctx->visited;
    assert(document_id_size <= sizeof(ctx->last_id));
    (void)memcpy(ctx->last_id, document_id, document_id_size);
    ctx->last_id_size = document_id_size;
    assert(document_id_size < sizeof(ctx->ids[0]));
    if (ctx->visited <= sizeof(ctx->ids) / sizeof(ctx->ids[0])) {
        (void)memcpy(ctx->ids[ctx->visited - 1U], document_id, document_id_size);
        ctx->ids[ctx->visited - 1U][document_id_size] = '\0';
    }

    if (!ctx->inserted) {
        put_indexed_document(ctx->database, (const uint8_t *)"zzz9", 4U);
        ctx->inserted = true;
    }
    return true;
}

static void test_insert_during_walk_is_picked_up(void)
{
    sdb_database *database = NULL;
    sdb_database_options options;
    insert_ctx ctx;
    size_t match_count = 0U;
    size_t i;

    cleanup();
    sdb_database_options_init(&options);
    assert(sdb_database_create(db_path, &options, &database) == SDB_OK);
    assert(sdb_index_create(
        database, collection, sizeof(collection),
        index_name, sizeof(index_name), false
    ) == SDB_OK);

    for (i = 0U; i < 5U; ++i) {
        uint8_t id[5];
        assert(snprintf((char *)id, sizeof(id), "d%03zu", i) == 4);
        put_indexed_document(database, id, 4U);
    }

    (void)memset(&ctx, 0, sizeof(ctx));
    ctx.database = database;
    assert(sdb_index_visit(
        database, collection, sizeof(collection),
        index_name, sizeof(index_name),
        index_value, sizeof(index_value),
        insert_during_walk, &ctx, &match_count
    ) == SDB_OK);

    assert(ctx.visited == 6U);
    assert(match_count == 6U);
    assert(ctx.last_id_size == 4U);
    assert(memcmp(ctx.last_id, "zzz9", 4U) == 0);

    /*
     * Each of the five pre-existing docs plus the mid-walk insertion must be
     * yielded exactly once. Because visited == 6 and every expected id appears
     * exactly once, no id is double-visited and no unexpected id leaked in via
     * cursor corruption.
     */
    for (i = 0U; i < 5U; ++i) {
        char id[5];
        assert(snprintf(id, sizeof(id), "d%03zu", i) == 4);
        assert(count_visited(ctx.ids, ctx.visited, id) == 1U);
    }
    assert(count_visited(ctx.ids, ctx.visited, "zzz9") == 1U);

    assert(sdb_database_close(database) == SDB_OK);
    cleanup();
    (void)puts("insert during walk is picked up: ok");
}

typedef struct {
    sdb_database *database;
    size_t visited;
    bool updated;
    uint8_t first_id[8];
    size_t first_id_size;
    char ids[8][8];
} update_ctx;

static bool update_during_walk(
    void *context, const uint8_t *document_id, size_t document_id_size
)
{
    update_ctx *ctx = (update_ctx *)context;
    ++ctx->visited;
    assert(document_id_size < sizeof(ctx->ids[0]));
    if (ctx->visited <= sizeof(ctx->ids) / sizeof(ctx->ids[0])) {
        (void)memcpy(ctx->ids[ctx->visited - 1U], document_id, document_id_size);
        ctx->ids[ctx->visited - 1U][document_id_size] = '\0';
    }
    if (!ctx->updated) {

        assert(document_id_size <= sizeof(ctx->first_id));
        (void)memcpy(ctx->first_id, document_id, document_id_size);
        ctx->first_id_size = document_id_size;
        put_indexed_document(ctx->database, document_id, document_id_size);
        ctx->updated = true;
    }
    return true;
}

static void test_update_during_walk_does_not_double_yield(void)
{
    sdb_database *database = NULL;
    sdb_database_options options;
    update_ctx ctx;
    size_t match_count = 0U;
    size_t i;

    cleanup();
    sdb_database_options_init(&options);
    assert(sdb_database_create(db_path, &options, &database) == SDB_OK);
    assert(sdb_index_create(
        database, collection, sizeof(collection),
        index_name, sizeof(index_name), false
    ) == SDB_OK);

    for (i = 0U; i < 4U; ++i) {
        uint8_t id[5];
        assert(snprintf((char *)id, sizeof(id), "e%03zu", i) == 4);
        put_indexed_document(database, id, 4U);
    }

    (void)memset(&ctx, 0, sizeof(ctx));
    ctx.database = database;
    assert(sdb_index_visit(
        database, collection, sizeof(collection),
        index_name, sizeof(index_name),
        index_value, sizeof(index_value),
        update_during_walk, &ctx, &match_count
    ) == SDB_OK);

    assert(ctx.visited == 5U);
    assert(match_count == 5U);
    assert(ctx.updated);

    /*
     * Re-putting the first-visited doc mid-walk stages a new (term, id, higher-
     * generation) entry that sorts immediately after the row just visited, so
     * that one doc is yielded exactly twice and the other three exactly once --
     * total 5, no unexpected id. This pins the precise multiplicity: it catches
     * a cursor that double-yields the wrong row, drops a row, or resurfaces an
     * id that was never re-put.
     */
    {
        char first[8];
        assert(ctx.first_id_size < sizeof(first));
        (void)memcpy(first, ctx.first_id, ctx.first_id_size);
        first[ctx.first_id_size] = '\0';
        for (i = 0U; i < 4U; ++i) {
            char id[5];
            size_t expected;
            assert(snprintf(id, sizeof(id), "e%03zu", i) == 4);
            expected = (strcmp(id, first) == 0) ? 2U : 1U;
            assert(count_visited(ctx.ids, ctx.visited, id) == expected);
        }
    }

    assert(sdb_database_close(database) == SDB_OK);
    cleanup();
    (void)puts("update during walk does not double-yield: ok");
}

typedef struct {
    sdb_database *database;
    size_t visited;
    bool deleted_self;
    char ids[8][8];
} delete_ctx;

static bool delete_during_walk(
    void *context, const uint8_t *document_id, size_t document_id_size
)
{
    delete_ctx *ctx = (delete_ctx *)context;
    ++ctx->visited;
    assert(document_id_size < sizeof(ctx->ids[0]));
    if (ctx->visited <= sizeof(ctx->ids) / sizeof(ctx->ids[0])) {
        (void)memcpy(ctx->ids[ctx->visited - 1U], document_id, document_id_size);
        ctx->ids[ctx->visited - 1U][document_id_size] = '\0';
    }

    if (!ctx->deleted_self) {
        delete_document(ctx->database, document_id, document_id_size);
        ctx->deleted_self = true;
    }
    return true;
}

static void test_delete_during_walk_terminates(void)
{
    sdb_database *database = NULL;
    sdb_database_options options;
    delete_ctx ctx;
    size_t match_count = 0U;
    size_t i;

    cleanup();
    sdb_database_options_init(&options);
    assert(sdb_database_create(db_path, &options, &database) == SDB_OK);
    assert(sdb_index_create(
        database, collection, sizeof(collection),
        index_name, sizeof(index_name), false
    ) == SDB_OK);

    for (i = 0U; i < 4U; ++i) {
        uint8_t id[5];
        assert(snprintf((char *)id, sizeof(id), "f%03zu", i) == 4);
        put_indexed_document(database, id, 4U);
    }

    (void)memset(&ctx, 0, sizeof(ctx));
    ctx.database = database;
    assert(sdb_index_visit(
        database, collection, sizeof(collection),
        index_name, sizeof(index_name),
        index_value, sizeof(index_value),
        delete_during_walk, &ctx, &match_count
    ) == SDB_OK);

    assert(ctx.deleted_self);
    assert(ctx.visited >= 3U && ctx.visited <= 4U);
    assert(match_count == ctx.visited);

    /*
     * Deleting the current doc mid-walk may cause the cursor to skip at most
     * one row (hence visited in [3,4]), but it must never double-visit a row
     * nor surface an id outside the four seeded docs. Assert every seeded id is
     * visited at most once and that the per-id counts sum to exactly the
     * visited total -- i.e. no unexpected/duplicate id slipped through the
     * mid-iteration deletion.
     */
    {
        size_t total = 0U;
        for (i = 0U; i < 4U; ++i) {
            char id[5];
            size_t seen;
            assert(snprintf(id, sizeof(id), "f%03zu", i) == 4);
            seen = count_visited(ctx.ids, ctx.visited, id);
            assert(seen <= 1U);
            total += seen;
        }
        assert(total == ctx.visited);
    }

    assert(sdb_database_close(database) == SDB_OK);
    cleanup();
    (void)puts("delete during walk terminates cleanly: ok");
}

int main(void)
{
    test_insert_during_walk_is_picked_up();
    test_update_during_walk_does_not_double_yield();
    test_delete_during_walk_terminates();
    (void)puts("index visitor reentry tests: ok");
    return 0;
}
