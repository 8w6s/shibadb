#include "shibadb.h"
#include "shibadb_engine.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static const uint8_t NS[] = "ns";

static void remove_db(const char *path)
{
    char wal[512];
    (void)remove(path);
    (void)snprintf(wal, sizeof(wal), "%s.wal", path);
    (void)remove(wal);
}

static void put(sdb_database *db, const char *key, const char *value)
{
    assert(sdb_kv_put(
        db, NS, 2U, (const uint8_t *)key, strlen(key),
        (const uint8_t *)value, strlen(value)
    ) == SDB_OK);
}

static void expect(sdb_database *db, const char *key, const char *value)
{
    uint8_t buf[64];
    size_t got = 0U;
    assert(sdb_kv_get(
        db, NS, 2U, (const uint8_t *)key, strlen(key),
        buf, sizeof(buf), &got
    ) == SDB_OK);
    assert(got == strlen(value));
    assert(memcmp(buf, value, got) == 0);
}

/*
 * Regression: an in-place overwrite on the SAME handle after compact() must not
 * lose data. compact adopted a target tree whose in-memory next_object_
 * generation was still 0 (created on an empty tree; rows copied raw via
 * batch_put never advance the counter), so the next allocation reused a live
 * generation and object_put's delete of the "previous" version erased the chunk
 * it had just written. The fix re-seeds the counter from the adopted tree. Here
 * k1 is overwritten post-compact and must survive; the untouched k2 is a control.
 */
static void test_overwrite_after_compact_keeps_data(void)
{
    const char *path = "test-compact-gen-reseed.tmp";
    sdb_database_options opt;
    sdb_database *db = NULL;
    sdb_compact_result cr;

    remove_db(path);
    sdb_database_options_init(&opt);
    assert(sdb_database_create(path, &opt, &db) == SDB_OK);
    put(db, "k1", "AAAA");
    put(db, "k2", "BBBB");
    assert(sdb_database_compact(db, &opt, &cr) == SDB_OK);

    put(db, "k1", "CCCC");
    expect(db, "k1", "CCCC");
    expect(db, "k2", "BBBB");
    assert(sdb_database_close(db) == SDB_OK);

    /*
     * The counter-row rewrite must not have persisted a rewound counter: a
     * reopen and further writes stay collision-free and data stays intact.
     */
    assert(sdb_database_open(path, &opt, &db) == SDB_OK);
    expect(db, "k1", "CCCC");
    expect(db, "k2", "BBBB");
    put(db, "k3", "DDDD");
    expect(db, "k1", "CCCC");
    expect(db, "k3", "DDDD");
    assert(sdb_database_close(db) == SDB_OK);
    remove_db(path);
}

/*
 * Stronger check: many overwrites on the post-compact handle. Under the bug the
 * reused generations collide across successive writes and progressively corrupt
 * the store; all keys must read back their latest value.
 */
static void test_repeated_overwrites_after_compact(void)
{
    const char *path = "test-compact-gen-reseed-many.tmp";
    sdb_database_options opt;
    sdb_database *db = NULL;
    sdb_compact_result cr;
    int i;

    remove_db(path);
    sdb_database_options_init(&opt);
    assert(sdb_database_create(path, &opt, &db) == SDB_OK);
    for (i = 0; i < 32; ++i) {
        char key[16];
        (void)snprintf(key, sizeof(key), "key-%d", i);
        put(db, key, "v0");
    }
    assert(sdb_database_compact(db, &opt, &cr) == SDB_OK);
    for (i = 0; i < 32; ++i) {
        char key[16];
        (void)snprintf(key, sizeof(key), "key-%d", i);
        put(db, key, "v1-updated");
    }
    for (i = 0; i < 32; ++i) {
        char key[16];
        (void)snprintf(key, sizeof(key), "key-%d", i);
        expect(db, key, "v1-updated");
    }
    assert(sdb_database_close(db) == SDB_OK);
    remove_db(path);
}

int main(void)
{
    test_overwrite_after_compact_keeps_data();
    test_repeated_overwrites_after_compact();
    (void)puts("compact generation reseed: ok");
    return 0;
}
