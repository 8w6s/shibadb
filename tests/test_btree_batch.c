#include "btree.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#define BATCH_ITEMS ((size_t)240)
#define CRASH_ITEMS ((size_t)120)
#define KEY_SIZE ((size_t)48)
#define VALUE_SIZE ((size_t)128)

static const char *test_path = "test-btree-batch.tmp";
static const char *wal_path = "test-btree-batch.tmp.wal";

static void make_key(size_t id, uint8_t key[KEY_SIZE])
{
    const int written = snprintf((char *)key, KEY_SIZE, "batch-%08zu:", id);
    assert(written == 15);
    (void)memset(key + (size_t)written, (int)(id & 0xffU),
        KEY_SIZE - (size_t)written);
}

static void make_value(size_t id, uint8_t value[VALUE_SIZE])
{
    size_t index;
    for (index = 0U; index < VALUE_SIZE; ++index) {
        value[index] = (uint8_t)((id * 17U + index * 29U) & 0xffU);
    }
}

static void create_tree(sdb_pager *pager, sdb_btree *tree)
{
    uint8_t salt[16] = {21U};
    uint8_t file_id[16] = {22U};
    (void)remove(test_path);
    (void)remove(wal_path);
    assert(sdb_pager_create(
        test_path, 4096U, salt, file_id, pager
    ) == SDB_OK);
    assert(sdb_btree_create(pager, tree) == SDB_OK);
}

static bool tree_has_all_or_none(sdb_btree *tree, size_t count)
{
    size_t present = 0U;
    size_t id;
    for (id = 0U; id < count; ++id) {
        uint8_t key[KEY_SIZE];
        uint8_t value[VALUE_SIZE];
        size_t value_size = 0U;
        sdb_status status;
        make_key(id, key);
        status = sdb_btree_get(
            tree, key, sizeof(key), value, sizeof(value), &value_size
        );
        if (status == SDB_OK) {
            uint8_t expected[VALUE_SIZE];
            make_value(id, expected);
            assert(value_size == sizeof(expected));
            assert(memcmp(value, expected, sizeof(value)) == 0);
            ++present;
        } else {
            assert(status == SDB_E_NOT_FOUND);
        }
    }
    assert(present == 0U || present == count);
    return present == count;
}

static void stage_items(sdb_btree *tree, size_t count, bool commit)
{
    sdb_btree_batch batch;
    size_t id;
    assert(sdb_btree_batch_begin(tree, &batch) == SDB_OK);
    for (id = 0U; id < count; ++id) {
        uint8_t key[KEY_SIZE];
        uint8_t value[VALUE_SIZE];
        uint8_t actual[VALUE_SIZE];
        size_t actual_size = 0U;
        make_key(id, key);
        make_value(id, value);
        assert(sdb_btree_batch_put(
            &batch, key, sizeof(key), value, sizeof(value)
        ) == SDB_OK);
        assert(sdb_btree_batch_get(
            &batch,
            key,
            sizeof(key),
            actual,
            sizeof(actual),
            &actual_size
        ) == SDB_OK);
        assert(actual_size == sizeof(value));
        assert(memcmp(actual, value, sizeof(value)) == 0);
        assert(sdb_btree_get(
            tree, key, sizeof(key), actual, sizeof(actual), &actual_size
        ) == SDB_E_NOT_FOUND);
    }
    if (commit) {
        assert(sdb_btree_batch_commit(&batch) == SDB_OK);
    } else {
        sdb_btree_batch_abort(&batch);
    }
}

static void verify_crash_result(void)
{
    sdb_pager pager;
    sdb_btree tree;
    sdb_btree_verify_result verify;
    assert(sdb_pager_open(test_path, &pager) == SDB_OK);
    assert(sdb_btree_open(&pager, &tree) == SDB_OK);
    (void)tree_has_all_or_none(&tree, CRASH_ITEMS);
    assert(sdb_btree_verify(&tree, &verify) == SDB_OK);
    assert(sdb_pager_close(&pager) == SDB_OK);
}

static void test_fault_boundaries(void)
{
    size_t boundary;
    size_t wal_faults = 0U;
    size_t file_faults = 0U;
    for (boundary = 0U; boundary < 6U; ++boundary) {
        sdb_pager pager;
        sdb_btree tree;
        sdb_btree_batch batch;
        size_t id;
        create_tree(&pager, &tree);
        assert(sdb_btree_batch_begin(&tree, &batch) == SDB_OK);
        for (id = 0U; id < CRASH_ITEMS; ++id) {
            uint8_t key[KEY_SIZE];
            uint8_t value[VALUE_SIZE];
            make_key(id, key);
            make_value(id, value);
            assert(sdb_btree_batch_put(
                &batch, key, sizeof(key), value, sizeof(value)
            ) == SDB_OK);
        }
        sdb_wal_fail_after_for_testing(boundary);
        if (sdb_btree_batch_commit(&batch) != SDB_OK) {
            ++wal_faults;
        }
        sdb_wal_clear_failure_for_testing();
        assert(sdb_pager_close(&pager) == SDB_OK);
        verify_crash_result();
    }
    assert(wal_faults > 0U);
    for (boundary = 0U; boundary < 10U; ++boundary) {
        sdb_pager pager;
        sdb_btree tree;
        sdb_btree_batch batch;
        size_t id;
        create_tree(&pager, &tree);
        assert(sdb_btree_batch_begin(&tree, &batch) == SDB_OK);
        for (id = 0U; id < CRASH_ITEMS; ++id) {
            uint8_t key[KEY_SIZE];
            uint8_t value[VALUE_SIZE];
            make_key(id, key);
            make_value(id, value);
            assert(sdb_btree_batch_put(
                &batch, key, sizeof(key), value, sizeof(value)
            ) == SDB_OK);
        }
        sdb_file_fail_after_for_testing(&pager.file, boundary);
        if (sdb_btree_batch_commit(&batch) != SDB_OK) {
            ++file_faults;
        }
        sdb_file_clear_failure_for_testing(&pager.file);
        assert(sdb_pager_close(&pager) == SDB_OK);
        verify_crash_result();
    }
    assert(file_faults > 0U);
}

static void test_encrypted_batch(void)
{
    static const uint8_t password[] = "btree-batch-password";
    uint8_t salt[16] = {31U};
    uint8_t file_id[16] = {32U};
    sdb_pager pager;
    sdb_btree tree;
    sdb_btree_verify_result verify;
    (void)remove(test_path);
    (void)remove(wal_path);
    assert(sdb_pager_create_encrypted(
        test_path,
        4096U,
        salt,
        file_id,
        password,
        sizeof(password) - 1U,
        SDB_MIN_KDF_ITERATIONS,
        &pager
    ) == SDB_OK);
    assert(sdb_btree_create(&pager, &tree) == SDB_OK);
    stage_items(&tree, CRASH_ITEMS, true);
    assert(sdb_pager_close(&pager) == SDB_OK);
    assert(sdb_pager_open_encrypted(
        test_path, password, sizeof(password) - 1U, &pager
    ) == SDB_OK);
    assert(sdb_btree_open(&pager, &tree) == SDB_OK);
    assert(tree_has_all_or_none(&tree, CRASH_ITEMS));
    assert(sdb_btree_verify(&tree, &verify) == SDB_OK);
    assert(sdb_pager_close(&pager) == SDB_OK);
}

int main(void)
{
    sdb_pager pager;
    sdb_btree tree;
    sdb_btree_batch batch;
    sdb_btree_verify_result verify;
    uint64_t initial_next_page;
    size_t id;

    create_tree(&pager, &tree);
    initial_next_page = pager.superblock.next_page_id;
    stage_items(&tree, BATCH_ITEMS, false);
    assert(pager.superblock.next_page_id == initial_next_page);
    assert(!tree_has_all_or_none(&tree, BATCH_ITEMS));
    assert(sdb_btree_verify(&tree, &verify) == SDB_OK);

    assert(sdb_btree_batch_begin(&tree, &batch) == SDB_OK);
    for (id = 0U; id < BATCH_ITEMS; ++id) {
        uint8_t key[KEY_SIZE];
        uint8_t value[VALUE_SIZE];
        make_key(id, key);
        make_value(id, value);
        assert(sdb_btree_batch_put(
            &batch, key, sizeof(key), value, sizeof(value)
        ) == SDB_OK);
    }
    for (id = 0U; id < BATCH_ITEMS; id += 5U) {
        uint8_t key[KEY_SIZE];
        size_t ignored_size;
        make_key(id, key);
        assert(sdb_btree_batch_delete(
            &batch, key, sizeof(key)
        ) == SDB_OK);
        assert(sdb_btree_batch_get(
            &batch, key, sizeof(key), NULL, 0U, &ignored_size
        ) == SDB_E_NOT_FOUND);
    }
    assert(sdb_btree_batch_commit(&batch) == SDB_OK);
    assert(pager.superblock.checkpoint_lsn == 1U);
    assert(sdb_btree_verify(&tree, &verify) == SDB_OK);
    assert(verify.entry_count == BATCH_ITEMS - (BATCH_ITEMS / 5U));
    assert(sdb_pager_close(&pager) == SDB_OK);

    assert(sdb_pager_open(test_path, &pager) == SDB_OK);
    assert(sdb_btree_open(&pager, &tree) == SDB_OK);
    assert(sdb_btree_verify(&tree, &verify) == SDB_OK);
    assert(verify.entry_count == BATCH_ITEMS - (BATCH_ITEMS / 5U));
    assert(sdb_pager_close(&pager) == SDB_OK);

    test_fault_boundaries();
    test_encrypted_batch();
    (void)remove(wal_path);
    (void)remove(test_path);
    (void)puts("btree batch tests: ok");
    return 0;
}
