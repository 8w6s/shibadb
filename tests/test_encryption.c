#include "btree.h"
#include "pager.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *test_path = "test-encryption.tmp";
static const char *wal_path = "test-encryption.tmp.wal";
static const uint8_t old_password[] = "correct horse battery staple";
static const uint8_t new_password[] = "new long database password";

static void fill_identity(uint8_t salt[16], uint8_t file_id[16])
{
    size_t index;
    for (index = 0U; index < 16U; ++index) {
        salt[index] = (uint8_t)(0x20U + index);
        file_id[index] = (uint8_t)(0xd0U + index);
    }
}

static bool contains_bytes(
    const uint8_t *haystack,
    size_t haystack_size,
    const uint8_t *needle,
    size_t needle_size
)
{
    size_t index;
    if (needle_size > haystack_size) {
        return false;
    }
    for (index = 0U; index <= haystack_size - needle_size; ++index) {
        if (memcmp(haystack + index, needle, needle_size) == 0) {
            return true;
        }
    }
    return false;
}

int main(void)
{
    sdb_pager pager;
    sdb_btree tree;
    uint8_t salt[16];
    uint8_t file_id[16];
    static const uint8_t key[] = "secret-key";
    static const uint8_t value[] =
        "plaintext-marker-that-must-never-appear-on-disk";
    uint8_t actual[128];
    size_t actual_size;

    (void)remove(test_path);
    (void)remove(wal_path);
    fill_identity(salt, file_id);
    assert(sdb_pager_create_encrypted(
        test_path,
        4096U,
        salt,
        file_id,
        old_password,
        sizeof(old_password) - 1U,
        SDB_MIN_KDF_ITERATIONS,
        &pager
    ) == SDB_OK);
    assert(pager.encryption_enabled);
    assert(sdb_btree_create(&pager, &tree) == SDB_OK);
    assert(sdb_btree_put(
        &tree, key, sizeof(key), value, sizeof(value)
    ) == SDB_OK);
    assert(sdb_btree_get(
        &tree, key, sizeof(key), actual, sizeof(actual), &actual_size
    ) == SDB_OK);
    assert(actual_size == sizeof(value));
    assert(memcmp(actual, value, sizeof(value)) == 0);
    assert(sdb_pager_close(&pager) == SDB_OK);

    assert(sdb_pager_open(test_path, &pager) == SDB_E_AUTHENTICATION);
    assert(sdb_pager_open_encrypted(
        test_path,
        (const uint8_t *)"wrong",
        5U,
        &pager
    ) == SDB_E_AUTHENTICATION);
    assert(sdb_pager_open_encrypted(
        test_path,
        old_password,
        sizeof(old_password) - 1U,
        &pager
    ) == SDB_OK);
    assert(sdb_btree_open(&pager, &tree) == SDB_OK);
    assert(sdb_btree_get(
        &tree, key, sizeof(key), actual, sizeof(actual), &actual_size
    ) == SDB_OK);

    assert(sdb_pager_rotate_password(
        &pager,
        new_password,
        sizeof(new_password) - 1U,
        SDB_MIN_KDF_ITERATIONS
    ) == SDB_OK);
    assert(sdb_pager_close(&pager) == SDB_OK);
    assert(sdb_pager_open_encrypted(
        test_path,
        old_password,
        sizeof(old_password) - 1U,
        &pager
    ) == SDB_E_AUTHENTICATION);
    assert(sdb_pager_open_encrypted(
        test_path,
        new_password,
        sizeof(new_password) - 1U,
        &pager
    ) == SDB_OK);
    assert(sdb_btree_open(&pager, &tree) == SDB_OK);
    assert(sdb_btree_get(
        &tree, key, sizeof(key), actual, sizeof(actual), &actual_size
    ) == SDB_OK);
    assert(memcmp(actual, value, sizeof(value)) == 0);
    assert(sdb_pager_close(&pager) == SDB_OK);

    {
        sdb_file file;
        uint64_t file_size;
        uint8_t *bytes;
        assert(sdb_file_open_existing(test_path, false, &file) == SDB_OK);
        assert(sdb_file_size(&file, &file_size) == SDB_OK);
        assert(file_size <= (uint64_t)SIZE_MAX);
        bytes = (uint8_t *)malloc((size_t)file_size);
        assert(bytes != NULL);
        assert(sdb_file_read_full(
            &file, 0U, bytes, (size_t)file_size
        ) == SDB_OK);
        assert(!contains_bytes(
            bytes,
            (size_t)file_size,
            value,
            sizeof(value) - 1U
        ));
        free(bytes);
        assert(sdb_file_close(&file) == SDB_OK);
    }

    (void)remove(wal_path);
    (void)remove(test_path);
    (void)puts("encryption tests: ok");
    return 0;
}
