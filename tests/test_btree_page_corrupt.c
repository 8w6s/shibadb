/*
 * Coverage-fill tests for corrupt-input paths in
 * sdb_btree_node_decode / sdb_btree_node_destroy. Each test hand-crafts
 * a bad payload that trips one specific corrupt-input branch. Prior to
 * this file, coverage report showed 22 uncovered lines in btree_page.c
 * concentrated in the SDB_E_CORRUPT / SDB_E_BAD_MAGIC / SDB_E_INVALID_ARGUMENT
 * arms; the fuzz corpus exercises them but ctest didn't.
 */

#include "btree_page.h"
#include "internal.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

/* SBT1 magic + header fields; layout mirrors src/btree_page.c. */
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

    /* NULL payload */
    assert(sdb_btree_node_decode(NULL, HDR_SIZE, &out) == SDB_E_INVALID_ARGUMENT);
    /* NULL out */
    assert(sdb_btree_node_decode(buf, HDR_SIZE, NULL) == SDB_E_INVALID_ARGUMENT);
    /* Payload shorter than header */
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
    /* kind = 99 (invalid) */
    sdb_write_u16_le(buf + 4U, 99U);
    sdb_write_u16_le(buf + 6U, 0U);  /* count */
    sdb_write_u32_le(buf + 8U, 0U);  /* reserved */
    sdb_write_u64_le(buf + 12U, 0U); /* first_child/right_sibling */
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
    /* reserved (offset 8) non-zero */
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
    sdb_write_u64_le(buf + 12U, 0U);  /* first_child == 0 → corrupt */
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
    /* count = 999, but payload after header has 0 bytes → capacity 0 */
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
    sdb_write_u16_le(buf + 6U, 1U);   /* count = 1 */
    sdb_write_u32_le(buf + 8U, 0U);
    sdb_write_u64_le(buf + 12U, 0U);
    /* Leaf entry at offset HDR_SIZE (16): key_size(u16), value_size(u32), then bytes */
    sdb_write_u16_le(buf + HDR_SIZE, 0U);  /* key_size = 0 → corrupt */
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
    sdb_write_u64_le(buf + 12U, 42U);  /* first_child = 42 */
    /* Internal entry: key_size(u16), reserved(u16), right_child(u64), then key */
    sdb_write_u16_le(buf + HDR_SIZE, 4U);
    sdb_write_u16_le(buf + HDR_SIZE + 2U, 0xffffU);  /* reserved non-zero → corrupt */
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
    /* Header (20) + leaf-entry fixed prefix (u16 key_size + u32 value_size = 6) = 26. */
    uint8_t buf[32];
    (void)memset(buf, 0, sizeof(buf));
    write_magic(buf);
    sdb_write_u16_le(buf + 4U, (uint16_t)SDB_BTREE_LEAF);
    sdb_write_u16_le(buf + 6U, 1U);
    sdb_write_u32_le(buf + 8U, 0U);
    sdb_write_u64_le(buf + 12U, 0U);
    /* Leaf entry: key_size = 100 with only ~6 bytes remaining */
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
    sdb_write_u64_le(buf + HDR_SIZE + 4U, 0U);  /* right_child = 0 → corrupt */
    buf[HDR_SIZE + 12U] = 'x';
    assert(sdb_btree_node_decode(buf, sizeof(buf), &out) == SDB_E_CORRUPT);
    (void)puts("internal right_child == 0: ok");
}

static void test_trailing_bytes(void)
{
    sdb_btree_node out;
    /* Zero-entry node with extra trailing bytes → remaining != 0 → corrupt */
    uint8_t buf[HDR_SIZE + 8];
    (void)memset(buf, 0, sizeof(buf));
    write_magic(buf);
    sdb_write_u16_le(buf + 4U, (uint16_t)SDB_BTREE_LEAF);
    sdb_write_u16_le(buf + 6U, 0U);
    sdb_write_u32_le(buf + 8U, 0U);
    sdb_write_u64_le(buf + 12U, 0U);
    /* Trailing byte non-zero → hits the remaining != 0 branch */
    buf[HDR_SIZE] = 0x55;
    assert(sdb_btree_node_decode(buf, sizeof(buf), &out) == SDB_E_CORRUPT);
    (void)puts("trailing bytes: ok");
}

static void test_destroy_null(void)
{
    sdb_btree_node_destroy(NULL);  /* line 14-15 early-return coverage */
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
    test_destroy_null();
    (void)puts("btree_page corrupt-input tests: all ok");
    return 0;
}
