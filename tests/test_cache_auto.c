/*
 * SDB_CACHE_AUTO and large-cache smoke test.
 *
 * Phase 3 lets cache_bytes = SDB_CACHE_AUTO size the page cache from physical
 * RAM, and lets callers pass a large explicit budget. Neither changes results,
 * only capacity — so this test proves correctness is preserved: it fills a DB
 * under an AUTO-sized cache, reopens (forcing reads back through the resized
 * cache), verifies every value, and repeats with a large explicit cache_bytes.
 */

#include "shibadb_engine.h"
#include "sysinfo.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *kv_path = "test-cache-auto.tmp";
static const char *kv_wal = "test-cache-auto.tmp.wal";
static const char *kv_lock = "test-cache-auto.tmp.lock";
static const uint8_t ns[] = { 'c', 'a', 'c', 'h', 'e', 0U };

static void cleanup(void)
{
    (void)remove(kv_path);
    (void)remove(kv_wal);
    (void)remove(kv_lock);
}

static void make_key(uint32_t i, uint8_t key[4])
{
    key[0] = (uint8_t)(i >> 24U);
    key[1] = (uint8_t)(i >> 16U);
    key[2] = (uint8_t)(i >> 8U);
    key[3] = (uint8_t)i;
}

static void run_with_cache(uint64_t cache_bytes, uint32_t count)
{
    sdb_database_options options;
    sdb_database *db = NULL;
    uint8_t value[64];
    uint8_t out[64];
    size_t out_size;
    uint32_t i;
    sdb_status status;

    cleanup();
    sdb_database_options_init(&options);
    options.cache_bytes = cache_bytes;

    status = sdb_database_create(kv_path, &options, &db);
    assert(status == SDB_OK);
    for (i = 0U; i < count; ++i) {
        uint8_t key[4];
        make_key(i, key);
        (void)memset(value, 0, sizeof(value));
        (void)memcpy(value, &i, sizeof(i));
        status = sdb_kv_put(db, ns, sizeof(ns), key, sizeof(key),
            value, sizeof(value));
        assert(status == SDB_OK);
    }
    status = sdb_database_close(db);
    assert(status == SDB_OK);

    /* Reopen: reads now flow back through a freshly sized cache. */
    db = NULL;
    status = sdb_database_open(kv_path, &options, &db);
    assert(status == SDB_OK);
    for (i = 0U; i < count; ++i) {
        uint8_t key[4];
        uint32_t got_index = 0U;
        make_key(i, key);
        out_size = 0U;
        status = sdb_kv_get(db, ns, sizeof(ns), key, sizeof(key),
            out, sizeof(out), &out_size);
        assert(status == SDB_OK);
        assert(out_size == sizeof(value));
        (void)memcpy(&got_index, out, sizeof(got_index));
        assert(got_index == i);
    }
    status = sdb_database_close(db);
    assert(status == SDB_OK);
    cleanup();
}

int main(void)
{
    const uint64_t ram = sdb_physical_memory_bytes();
    (void)printf("test_cache_auto: physical RAM = %llu MB\n",
        (unsigned long long)(ram / (1024U * 1024U)));

    run_with_cache(SDB_CACHE_AUTO, 2000U);
    run_with_cache((uint64_t)256U * 1024U * 1024U, 2000U);
    run_with_cache(0U, 500U); /* engine default still works */

    (void)printf("cache auto tests: ok\n");
    return 0;
}
