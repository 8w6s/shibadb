#include "page_cache.h"
#include "pager.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

/*
 * Count how many valid entries currently hold `page_id`. The no-double-
 * membership invariant requires this to be 0 or 1 for every id, always.
 */
static size_t count_membership(const sdb_page_cache *cache, uint64_t page_id)
{
    size_t index;
    size_t hits = 0U;
    for (index = 0U; index < cache->capacity; ++index) {
        if (cache->entries[index].valid
            && cache->entries[index].page_id == page_id) {
            hits += 1U;
        }
    }
    return hits;
}

static size_t count_valid(const sdb_page_cache *cache)
{
    size_t index;
    size_t valid = 0U;
    for (index = 0U; index < cache->capacity; ++index) {
        if (cache->entries[index].valid) {
            valid += 1U;
        }
    }
    return valid;
}

/* Original behavioural scenario, preserved as a regression check. */
static void test_basic(void)
{
    sdb_page_cache cache;
    uint8_t page[32];
    uint8_t output[32];
    uint64_t id;

    assert(sdb_page_cache_init(&cache, 3U, sizeof(page)) == SDB_OK);
    assert(!sdb_page_cache_get(&cache, 1U, output));
    for (id = 1U; id <= 3U; ++id) {
        (void)memset(page, (int)id, sizeof(page));
        sdb_page_cache_put(&cache, id, page);
    }
    assert(sdb_page_cache_get(&cache, 1U, output));
    assert(output[0] == 1U);

    (void)memset(page, 4, sizeof(page));
    sdb_page_cache_put(&cache, 4U, page);
    assert(!sdb_page_cache_get(&cache, 2U, output));
    assert(sdb_page_cache_get(&cache, 4U, output));
    assert(output[31] == 4U);

    sdb_page_cache_remove(&cache, 1U);
    assert(!sdb_page_cache_get(&cache, 1U, output));
    sdb_page_cache_destroy(&cache);
}

/*
 * Hash correctness across many ids that fit in capacity: every put is
 * retrievable, updates overwrite in place (no duplicate), removes evict
 * exactly one and leave the rest reachable (backward-shift deletion must
 * not orphan probed-over entries).
 */
static void test_hash_many(void)
{
    sdb_page_cache cache;
    uint8_t page[16];
    uint8_t output[16];
    uint64_t id;
    const uint64_t count = 200U;

    assert(sdb_page_cache_init(&cache, (size_t)count, sizeof(page)) == SDB_OK);
    for (id = 1U; id <= count; ++id) {
        (void)memset(page, (int)(id & 0xFFU), sizeof(page));
        sdb_page_cache_put(&cache, id, page);
    }
    for (id = 1U; id <= count; ++id) {
        assert(sdb_page_cache_get(&cache, id, output));
        assert(output[0] == (uint8_t)(id & 0xFFU));
        assert(count_membership(&cache, id) == 1U);
    }

    /* Update in place must not create a second slot. */
    (void)memset(page, 0xAB, sizeof(page));
    sdb_page_cache_put(&cache, 100U, page);
    assert(count_membership(&cache, 100U) == 1U);
    assert(sdb_page_cache_get(&cache, 100U, output));
    assert(output[0] == 0xAB);

    /* Remove every even id; odds must survive backward-shift deletion. */
    for (id = 2U; id <= count; id += 2U) {
        sdb_page_cache_remove(&cache, id);
    }
    for (id = 1U; id <= count; ++id) {
        bool present = sdb_page_cache_get(&cache, id, output);
        if ((id & 1U) == 1U) {
            assert(present);
            assert(count_membership(&cache, id) == 1U);
        } else {
            assert(!present);
            assert(count_membership(&cache, id) == 0U);
        }
    }
    sdb_page_cache_destroy(&cache);
}

/*
 * Force a long collision chain: ids that share the same hash home slot.
 * With power-of-two slot capacity and a multiplicative hash, ids spaced by
 * the slot count collide on the low bits; but any dense id range already
 * produces heavy probing, so we simply pack the cache and prove that
 * removing from the middle of a probe chain keeps the tail reachable.
 */
static void test_collision_chain(void)
{
    sdb_page_cache cache;
    uint8_t page[8];
    uint8_t output[8];
    uint64_t id;

    assert(sdb_page_cache_init(&cache, 32U, sizeof(page)) == SDB_OK);
    for (id = 1U; id <= 32U; ++id) {
        (void)memset(page, (int)id, sizeof(page));
        sdb_page_cache_put(&cache, id, page);
    }
    /* Remove a scattered subset, then confirm the survivors are intact. */
    sdb_page_cache_remove(&cache, 5U);
    sdb_page_cache_remove(&cache, 6U);
    sdb_page_cache_remove(&cache, 17U);
    for (id = 1U; id <= 32U; ++id) {
        bool present = sdb_page_cache_get(&cache, id, output);
        if (id == 5U || id == 6U || id == 17U) {
            assert(!present);
        } else {
            assert(present);
            assert(output[0] == (uint8_t)id);
            assert(count_membership(&cache, id) == 1U);
        }
    }
    sdb_page_cache_destroy(&cache);
}

/*
 * Eviction: a full cache that keeps receiving new ids must never hold more
 * than `capacity` valid entries and never a duplicate. Referenced pages
 * (touched via get) survive at least one clock pass.
 */
static void test_eviction_bounds(void)
{
    sdb_page_cache cache;
    uint8_t page[8];
    uint8_t output[8];
    uint64_t id;

    assert(sdb_page_cache_init(&cache, 8U, sizeof(page)) == SDB_OK);
    for (id = 1U; id <= 8U; ++id) {
        (void)memset(page, (int)id, sizeof(page));
        sdb_page_cache_put(&cache, id, page);
    }
    assert(count_valid(&cache) == 8U);

    /*
     * Insert 100 more distinct ids; capacity must stay bounded, and each id
     * that is present must be retrievable exactly once.
     */
    for (id = 9U; id <= 108U; ++id) {
        (void)memset(page, (int)(id & 0xFFU), sizeof(page));
        sdb_page_cache_put(&cache, id, page);
        assert(count_valid(&cache) <= 8U);
        assert(count_membership(&cache, id) == 1U);
        assert(sdb_page_cache_get(&cache, id, output));
        assert(output[0] == (uint8_t)(id & 0xFFU));
    }
    sdb_page_cache_destroy(&cache);
}

/*
 * CLOCK second chance: a page repeatedly read stays resident while cold
 * pages get evicted around it.
 */
static void test_clock_second_chance(void)
{
    sdb_page_cache cache;
    uint8_t page[8];
    uint8_t output[8];
    uint64_t id;

    assert(sdb_page_cache_init(&cache, 4U, sizeof(page)) == SDB_OK);
    for (id = 1U; id <= 4U; ++id) {
        (void)memset(page, (int)id, sizeof(page));
        sdb_page_cache_put(&cache, id, page);
    }
    /* Keep id 1 hot across a full round of cold insertions. */
    for (id = 5U; id <= 20U; ++id) {
        assert(sdb_page_cache_get(&cache, 1U, output)); /* set ref bit */
        assert(output[0] == 1U);
        (void)memset(page, (int)(id & 0xFFU), sizeof(page));
        sdb_page_cache_put(&cache, id, page);
    }
    /* id 1, referenced before every eviction, must have survived. */
    assert(sdb_page_cache_get(&cache, 1U, output));
    assert(output[0] == 1U);
    assert(count_membership(&cache, 1U) == 1U);
    sdb_page_cache_destroy(&cache);
}

static void test_byte_config(void)
{
    /* 0 bytes -> default capacity. */
    assert(sdb_pager_cache_capacity(0U, 4096U)
        == (size_t)SDB_PAGER_CACHE_CAPACITY);
    /* Exact multiple -> that many pages. */
    assert(sdb_pager_cache_capacity(4096U * 100U, 4096U) == 100U);
    /* Below the floor -> clamp to MIN. */
    assert(sdb_pager_cache_capacity(4096U * 2U, 4096U)
        == (size_t)SDB_PAGER_CACHE_CAPACITY_MIN);
    /* Sub-page request -> clamp to MIN, never zero. */
    assert(sdb_pager_cache_capacity(100U, 4096U)
        == (size_t)SDB_PAGER_CACHE_CAPACITY_MIN);
    /* Above the ceiling -> clamp to MAX. */
    assert(sdb_pager_cache_capacity(SIZE_MAX, 4096U)
        == (size_t)SDB_PAGER_CACHE_CAPACITY_MAX);
    /* Degenerate page_size -> default (defensive). */
    assert(sdb_pager_cache_capacity(4096U * 100U, 0U)
        == (size_t)SDB_PAGER_CACHE_CAPACITY);
}

int main(void)
{
    test_basic();
    test_hash_many();
    test_collision_chain();
    test_eviction_bounds();
    test_clock_second_chance();
    test_byte_config();
    (void)puts("page cache tests: ok");
    return 0;
}
