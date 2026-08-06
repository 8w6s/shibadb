#include "btree_page.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    static uint8_t key_a[] = "alpha";
    static uint8_t key_b[] = "beta";
    static uint8_t value_a[] = "one";
    static uint8_t value_b[] = "two";
    sdb_btree_entry entries[2];
    sdb_btree_node input;
    sdb_btree_node decoded;
    uint8_t encoded[256];
    size_t written;

    (void)memset(entries, 0, sizeof(entries));
    entries[0].key = key_a;
    entries[0].key_size = sizeof(key_a);
    entries[0].value = value_a;
    entries[0].value_size = sizeof(value_a);
    entries[1].key = key_b;
    entries[1].key_size = sizeof(key_b);
    entries[1].value = value_b;
    entries[1].value_size = sizeof(value_b);
    (void)memset(&input, 0, sizeof(input));
    input.kind = SDB_BTREE_LEAF;
    input.right_sibling = 19U;
    input.entries = entries;
    input.count = 2U;

    assert(sdb_btree_node_encode(
        &input, encoded, sizeof(encoded), &written
    ) == SDB_OK);
    assert(sdb_btree_node_decode(encoded, written, &decoded) == SDB_OK);
    assert(decoded.kind == SDB_BTREE_LEAF);
    assert(decoded.right_sibling == 19U);
    assert(decoded.count == 2U);
    assert(decoded.entries[1].key_size == sizeof(key_b));
    assert(memcmp(decoded.entries[1].key, key_b, sizeof(key_b)) == 0);
    assert(memcmp(decoded.entries[0].value, value_a, sizeof(value_a)) == 0);
    sdb_btree_node_destroy(&decoded);

    encoded[0] ^= 1U;
    assert(sdb_btree_node_decode(
        encoded, written, &decoded
    ) == SDB_E_BAD_MAGIC);
    encoded[0] ^= 1U;
    encoded[written] = 0xffU;
    assert(sdb_btree_node_decode(
        encoded, written + 1U, &decoded
    ) == SDB_E_CORRUPT);

    {
        static uint8_t internal_key_a[] = "gamma";
        static uint8_t internal_key_b[] = "omega";
        sdb_btree_entry internal_entries[2];
        sdb_btree_node internal_input;
        sdb_btree_node internal_decoded;
        uint8_t internal_encoded[256];
        size_t internal_written;

        (void)memset(internal_entries, 0, sizeof(internal_entries));
        internal_entries[0].key = internal_key_a;
        internal_entries[0].key_size = sizeof(internal_key_a);
        internal_entries[0].right_child = 77U;
        internal_entries[1].key = internal_key_b;
        internal_entries[1].key_size = sizeof(internal_key_b);
        internal_entries[1].right_child = 88U;
        (void)memset(&internal_input, 0, sizeof(internal_input));
        internal_input.kind = SDB_BTREE_INTERNAL;
        internal_input.first_child = 55U;
        internal_input.entries = internal_entries;
        internal_input.count = 2U;

        assert(sdb_btree_node_encode(
            &internal_input,
            internal_encoded,
            sizeof(internal_encoded),
            &internal_written
        ) == SDB_OK);
        assert(sdb_btree_node_decode(
            internal_encoded, internal_written, &internal_decoded
        ) == SDB_OK);
        assert(internal_decoded.kind == SDB_BTREE_INTERNAL);
        assert(internal_decoded.first_child == 55U);
        assert(internal_decoded.right_sibling == 0U);
        assert(internal_decoded.count == 2U);
        assert(internal_decoded.entries[0].right_child == 77U);
        assert(internal_decoded.entries[1].right_child == 88U);
        assert(internal_decoded.entries[0].key_size == sizeof(internal_key_a));
        assert(memcmp(
            internal_decoded.entries[0].key,
            internal_key_a,
            sizeof(internal_key_a)
        ) == 0);
        assert(internal_decoded.entries[1].key_size == sizeof(internal_key_b));
        assert(memcmp(
            internal_decoded.entries[1].key,
            internal_key_b,
            sizeof(internal_key_b)
        ) == 0);
        assert(internal_decoded.entries[0].value == NULL);
        assert(internal_decoded.entries[0].value_size == 0U);
        assert(internal_decoded.entries[1].value == NULL);
        assert(internal_decoded.entries[1].value_size == 0U);
        sdb_btree_node_destroy(&internal_decoded);
    }

    (void)puts("btree page tests: ok");
    return 0;
}
