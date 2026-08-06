#include "btree.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MODEL_SIZE ((size_t)256)
#define OPERATION_COUNT ((size_t)2000)
#define SEED_COUNT ((size_t)8)
#define REOPEN_INTERVAL ((size_t)250)

static const char *test_path = "test-btree-property.tmp";
static const char *wal_path = "test-btree-property.tmp.wal";

static uint32_t next_random(uint32_t *state)
{
    *state = (*state * UINT32_C(1103515245)) + UINT32_C(12345);
    return *state;
}

static void make_key(size_t id, uint8_t key[32])
{
    int written;
    (void)memset(key, (int)('A' + (id % 17U)), 32U);
    written = snprintf((char *)key, 32U, "key-%08zu:", id);
    assert(written == 13);
}

static void make_value(uint32_t version, uint8_t value[80])
{
    size_t index;
    for (index = 0U; index < 80U; ++index) {
        value[index] = (uint8_t)((version + (uint32_t)index) & 0xffU);
    }
}

static void verify_one(
    sdb_btree *tree, size_t id, bool present, uint32_t version
)
{
    uint8_t key[32];
    uint8_t actual[80];
    uint8_t expected[80];
    size_t actual_size;
    sdb_status status;
    make_key(id, key);
    status = sdb_btree_get(
        tree, key, sizeof(key), actual, sizeof(actual), &actual_size
    );
    if (!present) {
        assert(status == SDB_E_NOT_FOUND);
        return;
    }
    make_value(version, expected);
    assert(status == SDB_OK);
    assert(actual_size == sizeof(expected));
    assert(memcmp(actual, expected, sizeof(actual)) == 0);
}

/*
 * Prove FULL equivalence between the shadow model and the tree, in both
 * directions:
 *   model -> tree : every key the model believes present is found with the
 *                   expected value, and every absent key is missing.
 *   tree  -> model: a cursor walk visits exactly the present keys, in strictly
 *                   ascending order, each mapping to a present model slot with
 *                   the expected value. The visited count and the verify
 *                   entry_count must both equal the model's present count, so
 *                   the tree can hold neither a stale/extra key nor a phantom
 *                   duplicate.
 */
static void verify_full(
    sdb_btree *tree, const bool *present, const uint32_t *versions
)
{
    sdb_btree_verify_result verify;
    sdb_btree_cursor cursor;
    uint8_t previous_key[32];
    size_t present_count = 0U;
    size_t scanned = 0U;
    bool have_previous = false;
    size_t id;
    for (id = 0U; id < MODEL_SIZE; ++id) {
        verify_one(tree, id, present[id], versions[id]);
        if (present[id]) {
            ++present_count;
        }
    }
    (void)memset(&verify, 0, sizeof(verify));
    assert(sdb_btree_verify(tree, &verify) == SDB_OK);
    assert(verify.entry_count == (uint64_t)present_count);
    assert(sdb_btree_cursor_first(tree, &cursor) == SDB_OK);
    while (cursor.valid) {
        uint8_t actual_key[32];
        uint8_t actual_value[80];
        uint8_t expected_value[80];
        size_t key_size = 0U;
        size_t value_size = 0U;
        size_t entry_id = MODEL_SIZE;
        assert(sdb_btree_cursor_read(
            &cursor,
            actual_key,
            sizeof(actual_key),
            &key_size,
            actual_value,
            sizeof(actual_value),
            &value_size
        ) == SDB_OK);
        assert(key_size == sizeof(actual_key));
        if (have_previous) {
            assert(memcmp(previous_key, actual_key, sizeof(actual_key)) < 0);
        }
        (void)memcpy(previous_key, actual_key, sizeof(actual_key));
        have_previous = true;
        /* Portable, warning-clean parse of the "key-<digits>:" id (avoids
         * MSVC's C4996 deprecation of sscanf). */
        assert(memcmp(actual_key, "key-", 4U) == 0);
        entry_id = (size_t)strtoull((const char *)actual_key + 4, NULL, 10);
        assert(entry_id < MODEL_SIZE);
        assert(present[entry_id]);
        make_value(versions[entry_id], expected_value);
        assert(value_size == sizeof(expected_value));
        assert(memcmp(actual_value, expected_value, sizeof(expected_value))
            == 0);
        ++scanned;
        assert(sdb_btree_cursor_next(&cursor) == SDB_OK);
    }
    sdb_btree_cursor_close(&cursor);
    assert(scanned == present_count);
}

int main(void)
{
    static const uint32_t seeds[SEED_COUNT] = {
        UINT32_C(0x91f00d5a),
        UINT32_C(0x1234abcd),
        UINT32_C(0xdeadbeef),
        UINT32_C(0x0badf00d),
        UINT32_C(0xfeedface),
        UINT32_C(0xc0ffee11),
        UINT32_C(0x5eed0007),
        UINT32_C(0xa5a5a5a5)
    };
    uint8_t salt[16] = {11U};
    uint8_t file_id[16] = {12U};
    size_t seed_index;

    for (seed_index = 0U; seed_index < SEED_COUNT; ++seed_index) {
        sdb_pager pager;
        sdb_btree tree;
        bool present[MODEL_SIZE];
        uint32_t versions[MODEL_SIZE];
        uint32_t random_state = seeds[seed_index];
        size_t operation;

        (void)memset(present, 0, sizeof(present));
        (void)memset(versions, 0, sizeof(versions));
        (void)remove(test_path);
        (void)remove(wal_path);
        assert(sdb_pager_create(
            test_path, 4096U, salt, file_id, &pager
        ) == SDB_OK);
        assert(sdb_btree_create(&pager, &tree) == SDB_OK);
        for (operation = 0U; operation < OPERATION_COUNT; ++operation) {
            const uint32_t random = next_random(&random_state);
            const size_t id = (size_t)(random % (uint32_t)MODEL_SIZE);
            uint8_t key[32];
            make_key(id, key);
            if ((random % 5U) == 0U) {
                const sdb_status expected =
                    present[id] ? SDB_OK : SDB_E_NOT_FOUND;
                assert(sdb_btree_delete(
                    &tree, key, sizeof(key)
                ) == expected);
                present[id] = false;
            } else {
                uint8_t value[80];
                ++versions[id];
                make_value(versions[id], value);
                assert(sdb_btree_put(
                    &tree, key, sizeof(key), value, sizeof(value)
                ) == SDB_OK);
                present[id] = true;
            }
            verify_one(&tree, id, present[id], versions[id]);
            if ((operation % REOPEN_INTERVAL) == (REOPEN_INTERVAL - 1U)) {
                assert(sdb_pager_close(&pager) == SDB_OK);
                assert(sdb_pager_open(test_path, &pager) == SDB_OK);
                assert(sdb_btree_open(&pager, &tree) == SDB_OK);
                verify_full(&tree, present, versions);
            }
        }
        verify_full(&tree, present, versions);
        assert(sdb_pager_close(&pager) == SDB_OK);
    }
    (void)remove(wal_path);
    (void)remove(test_path);
    (void)puts("btree property tests: ok");
    return 0;
}
