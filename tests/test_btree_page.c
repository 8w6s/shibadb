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

    (void)puts("btree page tests: ok");
    return 0;
}
