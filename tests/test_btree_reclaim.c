#include "btree.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BASE_KEYS ((size_t)1024)
#define CHURN_ROUNDS ((size_t)6)
#define KEY_SIZE ((size_t)80)
#define VALUE_SIZE ((size_t)128)

static const char *test_path = "test-btree-reclaim.tmp";
static const char *wal_path = "test-btree-reclaim.tmp.wal";

static void make_key(size_t id, uint8_t key[KEY_SIZE])
{
    int written;
    (void)memset(key, (int)('a' + (id % 23U)), KEY_SIZE);
    written = snprintf((char *)key, KEY_SIZE, "%08zu:", id);
    assert(written == 9);
}

static void make_value(size_t id, uint8_t value[VALUE_SIZE])
{
    size_t index;
    for (index = 0U; index < VALUE_SIZE; ++index) {
        value[index] = (uint8_t)((id * 31U + index) & 0xffU);
    }
}

static uint32_t next_random(uint32_t *state)
{
    *state = (*state * UINT32_C(1664525)) + UINT32_C(1013904223);
    return *state;
}

static void shuffle(size_t *array, size_t count, uint32_t *state)
{
    size_t i;
    for (i = count; i > 1U; --i) {
        size_t other = (size_t)(next_random(state) % (uint32_t)i);
        size_t temporary = array[i - 1U];
        array[i - 1U] = array[other];
        array[other] = temporary;
    }
}

static uint64_t next_page_id(const sdb_pager *pager)
{
    return pager->superblock.next_page_id;
}

/*
 * Reconcile the on-disk page population against the tree's reachable set.
 * Every allocated page id in [1, next_page_id) must be accounted for exactly
 * once: either reachable from the root (walked by sdb_btree_verify_pages) or
 * sitting on the freelist. A page that is both reachable and free is a
 * double-free; a page that is neither is a leak; a freelist that revisits a
 * page is a corrupted (cyclic) freelist. All three trip an assert here.
 */
static void reconcile_pages(sdb_pager *pager, sdb_btree *tree)
{
    const size_t page_slots = (size_t)pager->superblock.next_page_id;
    bool *reachable;
    bool *freed;
    uint8_t *page;
    sdb_btree_verify_result result;
    uint64_t current;
    size_t reachable_count = 0U;
    size_t free_count = 0U;
    size_t page_id;

    reachable = (bool *)calloc(page_slots, sizeof(bool));
    freed = (bool *)calloc(page_slots, sizeof(bool));
    page = (uint8_t *)malloc((size_t)pager->superblock.page_size);
    assert(reachable != NULL && freed != NULL && page != NULL);

    (void)memset(&result, 0, sizeof(result));
    assert(sdb_btree_verify_pages(
        tree, &result, reachable, page_slots
    ) == SDB_OK);

    current = pager->superblock.freelist_page;
    while (current != 0U) {
        sdb_page_view view;
        assert(current < (uint64_t)page_slots);
        assert(!reachable[(size_t)current]);
        assert(!freed[(size_t)current]);
        freed[(size_t)current] = true;
        ++free_count;
        assert(sdb_pager_read(
            pager,
            current,
            page,
            (size_t)pager->superblock.page_size,
            &view
        ) == SDB_OK);
        assert(view.type == (uint16_t)SDB_PAGE_TYPE_FREE);
        current = sdb_read_u64_le(view.payload);
    }

    for (page_id = 1U; page_id < page_slots; ++page_id) {
        if (reachable[page_id]) {
            ++reachable_count;
        }
    }
    assert(reachable_count + free_count == page_slots - 1U);

    free(page);
    free(freed);
    free(reachable);
}

int main(void)
{
    sdb_pager pager;
    sdb_btree tree;
    uint8_t salt[16] = {5U};
    uint8_t file_id[16] = {6U};
    size_t order[BASE_KEYS];
    uint32_t random_state = UINT32_C(0xc0ffee01);
    uint64_t high_water;
    uint64_t after_churn;
    size_t index;
    size_t round;

    (void)remove(test_path);
    (void)remove(wal_path);
    assert(sdb_pager_create(
        test_path, 4096U, salt, file_id, &pager
    ) == SDB_OK);
    assert(sdb_btree_create(&pager, &tree) == SDB_OK);

    for (index = 0U; index < BASE_KEYS; ++index) {
        order[index] = index;
    }
    shuffle(order, BASE_KEYS, &random_state);
    for (index = 0U; index < BASE_KEYS; ++index) {
        uint8_t key[KEY_SIZE];
        uint8_t value[VALUE_SIZE];
        make_key(order[index], key);
        make_value(order[index], value);
        assert(sdb_btree_put(
            &tree, key, sizeof(key), value, sizeof(value)
        ) == SDB_OK);
    }
    high_water = next_page_id(&pager);

    assert(high_water > (uint64_t)8);

    for (round = 0U; round < CHURN_ROUNDS; ++round) {
        shuffle(order, BASE_KEYS, &random_state);
        for (index = 0U; index < BASE_KEYS; ++index) {
            uint8_t key[KEY_SIZE];
            make_key(order[index], key);
            assert(sdb_btree_delete(&tree, key, sizeof(key)) == SDB_OK);
        }
        after_churn = next_page_id(&pager);
        assert(after_churn <= high_water + (uint64_t)round + 1U);

        shuffle(order, BASE_KEYS, &random_state);
        for (index = 0U; index < BASE_KEYS; ++index) {
            uint8_t key[KEY_SIZE];
            uint8_t value[VALUE_SIZE];
            make_key(order[index], key);
            make_value(order[index], value);
            assert(sdb_btree_put(
                &tree, key, sizeof(key), value, sizeof(value)
            ) == SDB_OK);
        }

        assert(next_page_id(&pager) <= high_water + (uint64_t)4);
    }

    {
        sdb_btree_verify_result result;
        (void)memset(&result, 0, sizeof(result));
        assert(sdb_btree_verify(&tree, &result) == SDB_OK);
        assert(result.entry_count == (uint64_t)BASE_KEYS);
    }

    reconcile_pages(&pager, &tree);

    assert(sdb_pager_close(&pager) == SDB_OK);
    assert(sdb_pager_open(test_path, &pager) == SDB_OK);
    assert(sdb_btree_open(&pager, &tree) == SDB_OK);
    reconcile_pages(&pager, &tree);
    for (index = 0U; index < BASE_KEYS; ++index) {
        uint8_t key[KEY_SIZE];
        uint8_t expected[VALUE_SIZE];
        uint8_t actual[VALUE_SIZE];
        size_t actual_size = 0U;
        make_key(index, key);
        make_value(index, expected);
        assert(sdb_btree_get(
            &tree, key, sizeof(key), actual, sizeof(actual), &actual_size
        ) == SDB_OK);
        assert(actual_size == sizeof(expected));
        assert(memcmp(actual, expected, sizeof(expected)) == 0);
    }

    shuffle(order, BASE_KEYS, &random_state);
    for (index = 0U; index < BASE_KEYS; ++index) {
        uint8_t key[KEY_SIZE];
        make_key(order[index], key);
        assert(sdb_btree_delete(&tree, key, sizeof(key)) == SDB_OK);
    }
    assert(sdb_pager_close(&pager) == SDB_OK);
    assert(sdb_pager_open(test_path, &pager) == SDB_OK);
    assert(sdb_btree_open(&pager, &tree) == SDB_OK);
    {
        sdb_btree_cursor cursor;
        assert(sdb_btree_cursor_first(&tree, &cursor) == SDB_OK);
        assert(!cursor.valid);
        sdb_btree_cursor_close(&cursor);
    }

    assert(sdb_pager_close(&pager) == SDB_OK);
    (void)remove(wal_path);
    (void)remove(test_path);
    (void)puts("btree reclaim tests: ok");
    return 0;
}
