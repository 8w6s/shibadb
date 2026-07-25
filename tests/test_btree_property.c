#include "btree.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#define MODEL_SIZE ((size_t)128)
#define OPERATION_COUNT ((size_t)400)

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

int main(void)
{
    sdb_pager pager;
    sdb_btree tree;
    uint8_t salt[16] = {11U};
    uint8_t file_id[16] = {12U};
    bool present[MODEL_SIZE] = {false};
    uint32_t versions[MODEL_SIZE] = {0U};
    uint32_t random_state = UINT32_C(0x91f00d5a);
    size_t operation;

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
        if ((operation % 50U) == 49U) {
            size_t model_id;
            assert(sdb_pager_close(&pager) == SDB_OK);
            assert(sdb_pager_open(test_path, &pager) == SDB_OK);
            assert(sdb_btree_open(&pager, &tree) == SDB_OK);
            for (model_id = 0U; model_id < MODEL_SIZE; ++model_id) {
                verify_one(
                    &tree,
                    model_id,
                    present[model_id],
                    versions[model_id]
                );
            }
        }
    }
    assert(sdb_pager_close(&pager) == SDB_OK);
    (void)remove(wal_path);
    (void)remove(test_path);
    (void)puts("btree property tests: ok");
    return 0;
}
