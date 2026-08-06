#include "btree.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ITEM_COUNT ((size_t)600)
#define KEY_SIZE ((size_t)96)
#define VALUE_SIZE ((size_t)160)

static const char *test_path = "test-btree.tmp";
static const char *wal_path = "test-btree.tmp.wal";

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

int main(void)
{
    sdb_pager pager;
    sdb_btree tree;
    uint8_t salt[16] = {9U};
    uint8_t file_id[16] = {10U};
    size_t order[ITEM_COUNT];
    uint32_t random_state = UINT32_C(0xb71ee123);
    size_t index;

    (void)remove(test_path);
    (void)remove(wal_path);
    assert(sdb_pager_create(
        test_path, 4096U, salt, file_id, &pager
    ) == SDB_OK);
    assert(sdb_btree_create(&pager, &tree) == SDB_OK);
    for (index = 0U; index < ITEM_COUNT; ++index) {
        order[index] = index;
    }
    for (index = ITEM_COUNT; index > 1U; --index) {
        size_t other = (size_t)(next_random(&random_state) % (uint32_t)index);
        size_t temporary = order[index - 1U];
        order[index - 1U] = order[other];
        order[other] = temporary;
    }
    for (index = 0U; index < ITEM_COUNT; ++index) {
        uint8_t key[KEY_SIZE];
        uint8_t value[VALUE_SIZE];
        make_key(order[index], key);
        make_value(order[index], value);
        assert(sdb_btree_put(
            &tree, key, sizeof(key), value, sizeof(value)
        ) == SDB_OK);
        if ((index % 100U) == 99U) {
            assert(sdb_pager_close(&pager) == SDB_OK);
            assert(sdb_pager_open(test_path, &pager) == SDB_OK);
            assert(sdb_btree_open(&pager, &tree) == SDB_OK);
        }
    }
    for (index = 0U; index < ITEM_COUNT; ++index) {
        uint8_t key[KEY_SIZE];
        uint8_t expected[VALUE_SIZE];
        uint8_t actual[VALUE_SIZE];
        size_t actual_size = 0U;
        make_key(index, key);
        make_value(index, expected);
        assert(sdb_btree_get(
            &tree,
            key,
            sizeof(key),
            actual,
            sizeof(actual),
            &actual_size
        ) == SDB_OK);
        assert(actual_size == sizeof(expected));
        assert(memcmp(actual, expected, sizeof(actual)) == 0);
    }
    {
        uint8_t key[KEY_SIZE];
        uint8_t replacement[] = "replacement";
        uint8_t actual[VALUE_SIZE];
        size_t actual_size;
        make_key(217U, key);
        assert(sdb_btree_put(
            &tree, key, sizeof(key), replacement, sizeof(replacement)
        ) == SDB_OK);
        assert(sdb_btree_get(
            &tree, key, sizeof(key), actual, sizeof(actual), &actual_size
        ) == SDB_OK);
        assert(actual_size == sizeof(replacement));
        assert(memcmp(actual, replacement, sizeof(replacement)) == 0);
    }
    {
        uint8_t missing[KEY_SIZE];
        size_t value_size;
        make_key(999999U, missing);
        assert(sdb_btree_get(
            &tree, missing, sizeof(missing), NULL, 0U, &value_size
        ) == SDB_E_NOT_FOUND);
    }
    {
        sdb_btree_cursor cursor;
        size_t expected_id = 0U;
        static const uint8_t replaced_value[] = "replacement";
        assert(sdb_btree_cursor_first(&tree, &cursor) == SDB_OK);
        while (cursor.valid) {
            uint8_t expected_key[KEY_SIZE];
            uint8_t actual_key[KEY_SIZE];
            uint8_t actual_value[VALUE_SIZE];
            uint8_t expected_value[VALUE_SIZE];
            size_t actual_key_size;
            size_t actual_value_size;
            make_key(expected_id, expected_key);
            assert(sdb_btree_cursor_read(
                &cursor,
                actual_key,
                sizeof(actual_key),
                &actual_key_size,
                actual_value,
                sizeof(actual_value),
                &actual_value_size
            ) == SDB_OK);
            assert(actual_key_size == sizeof(expected_key));
            assert(memcmp(actual_key, expected_key, sizeof(expected_key)) == 0);
            if (expected_id == 217U) {
                assert(actual_value_size == sizeof(replaced_value));
                assert(memcmp(
                    actual_value, replaced_value, sizeof(replaced_value)
                ) == 0);
            } else {
                make_value(expected_id, expected_value);
                assert(actual_value_size == sizeof(expected_value));
                assert(memcmp(
                    actual_value, expected_value, sizeof(expected_value)
                ) == 0);
            }
            ++expected_id;
            assert(sdb_btree_cursor_next(&cursor) == SDB_OK);
        }
        assert(expected_id == ITEM_COUNT);
        sdb_btree_cursor_close(&cursor);
    }
    {
        /*
         * Reverse: last + prev must walk every key in descending order,
         * crossing leaves via the descent path (ITEM_COUNT=600 spans many).
         */
        sdb_btree_cursor cursor;
        size_t expected_id = ITEM_COUNT;
        size_t count = 0U;
        assert(sdb_btree_cursor_last(&tree, &cursor) == SDB_OK);
        while (cursor.valid) {
            uint8_t expected_key[KEY_SIZE];
            uint8_t actual_key[KEY_SIZE];
            uint8_t actual_value[VALUE_SIZE];
            size_t actual_key_size;
            size_t actual_value_size;
            assert(expected_id > 0U);
            --expected_id;
            make_key(expected_id, expected_key);
            assert(sdb_btree_cursor_read(
                &cursor,
                actual_key,
                sizeof(actual_key),
                &actual_key_size,
                actual_value,
                sizeof(actual_value),
                &actual_value_size
            ) == SDB_OK);
            assert(actual_key_size == sizeof(expected_key));
            assert(memcmp(actual_key, expected_key, sizeof(expected_key)) == 0);
            ++count;
            assert(sdb_btree_cursor_prev(&cursor) == SDB_OK);
        }
        assert(count == ITEM_COUNT);
        assert(expected_id == 0U);
        sdb_btree_cursor_close(&cursor);
    }
    {
        sdb_btree_cursor cursor;
        uint8_t seek_key[KEY_SIZE];
        uint8_t actual_key[KEY_SIZE];
        uint8_t expected_key[KEY_SIZE];
        uint8_t actual_value[VALUE_SIZE];
        size_t key_size;
        size_t value_size;
        make_key(216U, seek_key);
        seek_key[8] = (uint8_t)';';
        make_key(217U, expected_key);
        assert(sdb_btree_cursor_seek(
            &tree, seek_key, sizeof(seek_key), &cursor
        ) == SDB_OK);
        assert(cursor.valid);
        assert(sdb_btree_cursor_read(
            &cursor,
            actual_key,
            sizeof(actual_key),
            &key_size,
            actual_value,
            sizeof(actual_value),
            &value_size
        ) == SDB_OK);
        assert(key_size == sizeof(expected_key));
        assert(memcmp(actual_key, expected_key, sizeof(expected_key)) == 0);
        {
            static const uint8_t replaced_value[] = "replacement";
            assert(value_size == sizeof(replaced_value));
            assert(memcmp(
                actual_value, replaced_value, sizeof(replaced_value)
            ) == 0);
        }
        sdb_btree_cursor_close(&cursor);
    }
    for (index = 0U; index < ITEM_COUNT; index += 4U) {
        uint8_t key[KEY_SIZE];
        size_t ignored_size;
        make_key(index, key);
        assert(sdb_btree_delete(&tree, key, sizeof(key)) == SDB_OK);
        assert(sdb_btree_delete(
            &tree, key, sizeof(key)
        ) == SDB_E_NOT_FOUND);
        assert(sdb_btree_get(
            &tree, key, sizeof(key), NULL, 0U, &ignored_size
        ) == SDB_E_NOT_FOUND);
    }
    assert(sdb_pager_close(&pager) == SDB_OK);
    assert(sdb_pager_open(test_path, &pager) == SDB_OK);
    assert(sdb_btree_open(&pager, &tree) == SDB_OK);
    {
        sdb_btree_cursor cursor;
        size_t previous = 0U;
        size_t remaining = 0U;
        assert(sdb_btree_cursor_first(&tree, &cursor) == SDB_OK);
        while (cursor.valid) {
            uint8_t actual_key[KEY_SIZE];
            uint8_t value[VALUE_SIZE];
            uint8_t expected_value[VALUE_SIZE];
            size_t key_size;
            size_t value_size;
            size_t current;
            assert(sdb_btree_cursor_read(
                &cursor,
                actual_key,
                sizeof(actual_key),
                &key_size,
                value,
                sizeof(value),
                &value_size
            ) == SDB_OK);
            /* Portable, warning-clean parse of the 8-digit key prefix
             * (avoids MSVC's C4996 deprecation of sscanf). The key is
             * "%08zu:" so the first byte is always a digit and strtoull
             * stops at the ':'. */
            assert(actual_key[0] >= '0' && actual_key[0] <= '9');
            current = (size_t)strtoull((const char *)actual_key, NULL, 10);
            assert((current % 4U) != 0U);
            if (current == 217U) {
                static const uint8_t replaced_value[] = "replacement";
                assert(value_size == sizeof(replaced_value));
                assert(memcmp(
                    value, replaced_value, sizeof(replaced_value)
                ) == 0);
            } else {
                make_value(current, expected_value);
                assert(value_size == sizeof(expected_value));
                assert(memcmp(
                    value, expected_value, sizeof(expected_value)
                ) == 0);
            }
            if (remaining != 0U) {
                assert(current > previous);
            }
            previous = current;
            ++remaining;
            assert(sdb_btree_cursor_next(&cursor) == SDB_OK);
        }
        assert(remaining == ITEM_COUNT - (ITEM_COUNT / 4U));
        sdb_btree_cursor_close(&cursor);
    }
    assert(sdb_pager_close(&pager) == SDB_OK);
    (void)remove(wal_path);
    (void)remove(test_path);
    (void)puts("btree tests: ok");
    return 0;
}
