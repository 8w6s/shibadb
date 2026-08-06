
#include "btree_page.h"
#include "internal.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#define HDR_SIZE ((size_t)20)

static void write_magic(uint8_t *buf)
{
    buf[0] = 'S'; buf[1] = 'B'; buf[2] = 'T'; buf[3] = '1';
}

static void test_null_and_short(void)
{
    sdb_btree_node out;
    uint8_t buf[HDR_SIZE];
    (void)memset(buf, 0, sizeof(buf));

    assert(sdb_btree_node_decode(NULL, HDR_SIZE, &out) == SDB_E_INVALID_ARGUMENT);

    assert(sdb_btree_node_decode(buf, HDR_SIZE, NULL) == SDB_E_INVALID_ARGUMENT);

    assert(sdb_btree_node_decode(buf, HDR_SIZE - 1U, &out)
        == SDB_E_INVALID_ARGUMENT);
    (void)puts("null/short reject: ok");
}

static void test_bad_magic(void)
{
    sdb_btree_node out;
    uint8_t buf[HDR_SIZE];
    (void)memset(buf, 0, sizeof(buf));
    buf[0] = 'X'; buf[1] = 'X'; buf[2] = 'X'; buf[3] = 'X';
    assert(sdb_btree_node_decode(buf, HDR_SIZE, &out) == SDB_E_BAD_MAGIC);
    (void)puts("bad magic: ok");
}

static void test_bad_kind(void)
{
    sdb_btree_node out;
    uint8_t buf[HDR_SIZE];
    (void)memset(buf, 0, sizeof(buf));
    write_magic(buf);

    sdb_write_u16_le(buf + 4U, 99U);
    sdb_write_u16_le(buf + 6U, 0U);
    sdb_write_u32_le(buf + 8U, 0U);
    sdb_write_u64_le(buf + 12U, 0U);
    assert(sdb_btree_node_decode(buf, HDR_SIZE, &out) == SDB_E_CORRUPT);
    (void)puts("bad kind: ok");
}

static void test_reserved_bytes_nonzero(void)
{
    sdb_btree_node out;
    uint8_t buf[HDR_SIZE];
    (void)memset(buf, 0, sizeof(buf));
    write_magic(buf);
    sdb_write_u16_le(buf + 4U, (uint16_t)SDB_BTREE_LEAF);
    sdb_write_u16_le(buf + 6U, 0U);

    sdb_write_u32_le(buf + 8U, 0xdeadbeefU);
    sdb_write_u64_le(buf + 12U, 0U);
    assert(sdb_btree_node_decode(buf, HDR_SIZE, &out) == SDB_E_CORRUPT);
    (void)puts("reserved bytes non-zero: ok");
}

static void test_internal_zero_first_child(void)
{
    sdb_btree_node out;
    uint8_t buf[HDR_SIZE];
    (void)memset(buf, 0, sizeof(buf));
    write_magic(buf);
    sdb_write_u16_le(buf + 4U, (uint16_t)SDB_BTREE_INTERNAL);
    sdb_write_u16_le(buf + 6U, 0U);
    sdb_write_u32_le(buf + 8U, 0U);
    sdb_write_u64_le(buf + 12U, 0U);
    assert(sdb_btree_node_decode(buf, HDR_SIZE, &out) == SDB_E_CORRUPT);
    (void)puts("internal zero first_child: ok");
}

static void test_count_exceeds_capacity(void)
{
    sdb_btree_node out;
    uint8_t buf[HDR_SIZE];
    (void)memset(buf, 0, sizeof(buf));
    write_magic(buf);
    sdb_write_u16_le(buf + 4U, (uint16_t)SDB_BTREE_LEAF);

    sdb_write_u16_le(buf + 6U, 999U);
    sdb_write_u32_le(buf + 8U, 0U);
    sdb_write_u64_le(buf + 12U, 0U);
    assert(sdb_btree_node_decode(buf, HDR_SIZE, &out) == SDB_E_CORRUPT);
    (void)puts("count > capacity: ok");
}

static void test_entry_key_size_zero(void)
{
    sdb_btree_node out;
    uint8_t buf[64];
    (void)memset(buf, 0, sizeof(buf));
    write_magic(buf);
    sdb_write_u16_le(buf + 4U, (uint16_t)SDB_BTREE_LEAF);
    sdb_write_u16_le(buf + 6U, 1U);
    sdb_write_u32_le(buf + 8U, 0U);
    sdb_write_u64_le(buf + 12U, 0U);

    sdb_write_u16_le(buf + HDR_SIZE, 0U);
    sdb_write_u32_le(buf + HDR_SIZE + 2U, 0U);
    assert(sdb_btree_node_decode(buf, sizeof(buf), &out) == SDB_E_CORRUPT);
    (void)puts("entry key_size == 0: ok");
}

static void test_internal_entry_reserved_nonzero(void)
{
    sdb_btree_node out;
    uint8_t buf[64];
    (void)memset(buf, 0, sizeof(buf));
    write_magic(buf);
    sdb_write_u16_le(buf + 4U, (uint16_t)SDB_BTREE_INTERNAL);
    sdb_write_u16_le(buf + 6U, 1U);
    sdb_write_u32_le(buf + 8U, 0U);
    sdb_write_u64_le(buf + 12U, 42U);

    sdb_write_u16_le(buf + HDR_SIZE, 4U);
    sdb_write_u16_le(buf + HDR_SIZE + 2U, 0xffffU);
    sdb_write_u64_le(buf + HDR_SIZE + 4U, 43U);
    buf[HDR_SIZE + 12U] = 'k';
    buf[HDR_SIZE + 13U] = 'e';
    buf[HDR_SIZE + 14U] = 'y';
    buf[HDR_SIZE + 15U] = '1';
    assert(sdb_btree_node_decode(buf, sizeof(buf), &out) == SDB_E_CORRUPT);
    (void)puts("internal entry reserved non-zero: ok");
}

static void test_key_size_exceeds_remaining(void)
{
    sdb_btree_node out;

    uint8_t buf[32];
    (void)memset(buf, 0, sizeof(buf));
    write_magic(buf);
    sdb_write_u16_le(buf + 4U, (uint16_t)SDB_BTREE_LEAF);
    sdb_write_u16_le(buf + 6U, 1U);
    sdb_write_u32_le(buf + 8U, 0U);
    sdb_write_u64_le(buf + 12U, 0U);

    sdb_write_u16_le(buf + HDR_SIZE, 100U);
    sdb_write_u32_le(buf + HDR_SIZE + 2U, 0U);
    assert(sdb_btree_node_decode(buf, sizeof(buf), &out) == SDB_E_CORRUPT);
    (void)puts("key_size > remaining: ok");
}

static void test_internal_entry_zero_right_child(void)
{
    sdb_btree_node out;
    uint8_t buf[40];
    (void)memset(buf, 0, sizeof(buf));
    write_magic(buf);
    sdb_write_u16_le(buf + 4U, (uint16_t)SDB_BTREE_INTERNAL);
    sdb_write_u16_le(buf + 6U, 1U);
    sdb_write_u32_le(buf + 8U, 0U);
    sdb_write_u64_le(buf + 12U, 42U);
    sdb_write_u16_le(buf + HDR_SIZE, 1U);
    sdb_write_u16_le(buf + HDR_SIZE + 2U, 0U);
    sdb_write_u64_le(buf + HDR_SIZE + 4U, 0U);
    buf[HDR_SIZE + 12U] = 'x';
    assert(sdb_btree_node_decode(buf, sizeof(buf), &out) == SDB_E_CORRUPT);
    (void)puts("internal right_child == 0: ok");
}

static void test_trailing_bytes(void)
{
    sdb_btree_node out;

    uint8_t buf[HDR_SIZE + 8];
    (void)memset(buf, 0, sizeof(buf));
    write_magic(buf);
    sdb_write_u16_le(buf + 4U, (uint16_t)SDB_BTREE_LEAF);
    sdb_write_u16_le(buf + 6U, 0U);
    sdb_write_u32_le(buf + 8U, 0U);
    sdb_write_u64_le(buf + 12U, 0U);

    buf[HDR_SIZE] = 0x55;
    assert(sdb_btree_node_decode(buf, sizeof(buf), &out) == SDB_E_CORRUPT);
    (void)puts("trailing bytes: ok");
}

static void test_leaf_keys_out_of_order(void)
{
    sdb_btree_node out;
    uint8_t buf[36];
    (void)memset(buf, 0, sizeof(buf));
    write_magic(buf);
    sdb_write_u16_le(buf + 4U, (uint16_t)SDB_BTREE_LEAF);
    sdb_write_u16_le(buf + 6U, 2U);
    sdb_write_u32_le(buf + 8U, 0U);
    sdb_write_u64_le(buf + 12U, 0U);

    sdb_write_u16_le(buf + HDR_SIZE, 2U);
    sdb_write_u32_le(buf + HDR_SIZE + 2U, 0U);
    buf[HDR_SIZE + 6U] = 'b';
    buf[HDR_SIZE + 7U] = 'b';

    sdb_write_u16_le(buf + HDR_SIZE + 8U, 2U);
    sdb_write_u32_le(buf + HDR_SIZE + 10U, 0U);
    buf[HDR_SIZE + 14U] = 'a';
    buf[HDR_SIZE + 15U] = 'a';
    assert(sdb_btree_node_decode(buf, sizeof(buf), &out) == SDB_E_CORRUPT);
    (void)puts("leaf keys out of order: ok");
}

static void test_leaf_keys_duplicate(void)
{
    sdb_btree_node out;
    uint8_t buf[36];
    (void)memset(buf, 0, sizeof(buf));
    write_magic(buf);
    sdb_write_u16_le(buf + 4U, (uint16_t)SDB_BTREE_LEAF);
    sdb_write_u16_le(buf + 6U, 2U);
    sdb_write_u32_le(buf + 8U, 0U);
    sdb_write_u64_le(buf + 12U, 0U);

    sdb_write_u16_le(buf + HDR_SIZE, 2U);
    sdb_write_u32_le(buf + HDR_SIZE + 2U, 0U);
    buf[HDR_SIZE + 6U] = 'a';
    buf[HDR_SIZE + 7U] = 'a';

    sdb_write_u16_le(buf + HDR_SIZE + 8U, 2U);
    sdb_write_u32_le(buf + HDR_SIZE + 10U, 0U);
    buf[HDR_SIZE + 14U] = 'a';
    buf[HDR_SIZE + 15U] = 'a';
    assert(sdb_btree_node_decode(buf, sizeof(buf), &out) == SDB_E_CORRUPT);
    (void)puts("leaf keys duplicate: ok");
}

static void test_internal_keys_out_of_order(void)
{
    sdb_btree_node out;
    uint8_t buf[46];
    (void)memset(buf, 0, sizeof(buf));
    write_magic(buf);
    sdb_write_u16_le(buf + 4U, (uint16_t)SDB_BTREE_INTERNAL);
    sdb_write_u16_le(buf + 6U, 2U);
    sdb_write_u32_le(buf + 8U, 0U);
    sdb_write_u64_le(buf + 12U, 5U);

    sdb_write_u16_le(buf + HDR_SIZE, 1U);
    sdb_write_u16_le(buf + HDR_SIZE + 2U, 0U);
    sdb_write_u64_le(buf + HDR_SIZE + 4U, 6U);
    buf[HDR_SIZE + 12U] = 'y';

    sdb_write_u16_le(buf + HDR_SIZE + 13U, 1U);
    sdb_write_u16_le(buf + HDR_SIZE + 15U, 0U);
    sdb_write_u64_le(buf + HDR_SIZE + 17U, 7U);
    buf[HDR_SIZE + 25U] = 'x';
    assert(sdb_btree_node_decode(buf, sizeof(buf), &out) == SDB_E_CORRUPT);
    (void)puts("internal keys out of order: ok");
}

static void test_destroy_null(void)
{
    sdb_btree_node_destroy(NULL);
    (void)puts("destroy NULL: ok");
}

int main(void)
{
    test_null_and_short();
    test_bad_magic();
    test_bad_kind();
    test_reserved_bytes_nonzero();
    test_internal_zero_first_child();
    test_count_exceeds_capacity();
    test_entry_key_size_zero();
    test_internal_entry_reserved_nonzero();
    test_key_size_exceeds_remaining();
    test_internal_entry_zero_right_child();
    test_trailing_bytes();
    test_leaf_keys_out_of_order();
    test_leaf_keys_duplicate();
    test_internal_keys_out_of_order();
    test_destroy_null();
    (void)puts("btree_page corrupt-input tests: all ok");
    return 0;
}
