
#include "btree.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PAGE_SIZE ((uint32_t)4096)
#define KEY_SIZE ((size_t)320)
#define VALUE_SIZE ((size_t)64)
#define TOTAL_KEYS ((size_t)512)

static const char *test_path = "test-btree-reclaim-overflow.tmp";
static const char *wal_path = "test-btree-reclaim-overflow.tmp.wal";

static void make_key(size_t id, uint8_t *key)
{
    int written = snprintf((char *)key, 16, "%010zu:", id);
    assert(written == 11);
    (void)memset(key + written, (int)('A' + (id % 26U)),
                 KEY_SIZE - (size_t)written);
}

static void make_value(size_t id, uint8_t *value)
{
    size_t index;
    for (index = 0U; index < VALUE_SIZE; ++index) {
        value[index] = (uint8_t)((id * 131U + index) & 0xffU);
    }
}

int main(void)
{
    sdb_pager pager;
    sdb_btree tree;
    uint8_t salt[16] = {7U};
    uint8_t file_id[16] = {8U};
    size_t index;
    sdb_btree_verify_result verify;

    (void)remove(test_path);
    (void)remove(wal_path);
    assert(sdb_pager_create(
        test_path, PAGE_SIZE, salt, file_id, &pager
    ) == SDB_OK);
    assert(sdb_btree_create(&pager, &tree) == SDB_OK);

    for (index = 0U; index < TOTAL_KEYS; ++index) {
        uint8_t key[KEY_SIZE];
        uint8_t value[VALUE_SIZE];
        make_key(index, key);
        make_value(index, value);
        assert(sdb_btree_put(
            &tree, key, sizeof(key), value, sizeof(value)
        ) == SDB_OK);
    }

    (void)memset(&verify, 0, sizeof(verify));
    assert(sdb_btree_verify(&tree, &verify) == SDB_OK);
    assert(verify.entry_count == (uint64_t)TOTAL_KEYS);
    assert(verify.height >= (uint32_t)2);
    fprintf(stderr, "post-insert: height=%u entries=%llu\n",
        verify.height, (unsigned long long)verify.entry_count);

    for (index = 0U; index < TOTAL_KEYS / 2U; ++index) {
        uint8_t key[KEY_SIZE];
        sdb_status status;
        make_key(index, key);
        status = sdb_btree_delete(&tree, key, sizeof(key));
        if (status != SDB_OK) {
            fprintf(stderr,
                "delete[%zu] failed: status=%d (pre-fix: sibling overflow "
                "returns SDB_E_BUFFER_TOO_SMALL)\n", index, (int)status);
            assert(status == SDB_OK);
        }
    }

    (void)memset(&verify, 0, sizeof(verify));
    assert(sdb_btree_verify(&tree, &verify) == SDB_OK);
    assert(verify.entry_count == (uint64_t)(TOTAL_KEYS - TOTAL_KEYS / 2U));

    for (index = TOTAL_KEYS / 2U; index < TOTAL_KEYS; ++index) {
        uint8_t key[KEY_SIZE];
        uint8_t expected[VALUE_SIZE];
        uint8_t actual[VALUE_SIZE];
        size_t actual_size = 0U;
        make_key(index, key);
        make_value(index, expected);
        assert(sdb_btree_get(
            &tree, key, sizeof(key), actual, sizeof(actual), &actual_size
        ) == SDB_OK);
        assert(actual_size == VALUE_SIZE);
        assert(memcmp(actual, expected, VALUE_SIZE) == 0);
    }

    for (index = TOTAL_KEYS / 2U; index < TOTAL_KEYS; ++index) {
        uint8_t key[KEY_SIZE];
        make_key(index, key);
        assert(sdb_btree_delete(&tree, key, sizeof(key)) == SDB_OK);
    }
    (void)memset(&verify, 0, sizeof(verify));
    assert(sdb_btree_verify(&tree, &verify) == SDB_OK);
    assert(verify.entry_count == 0U);

    assert(sdb_pager_close(&pager) == SDB_OK);
    (void)remove(wal_path);
    (void)remove(test_path);
    (void)puts("btree reclaim sibling-overflow: ok");
    return 0;
}
