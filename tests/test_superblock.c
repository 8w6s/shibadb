#include "shibadb.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/*
 * The superblock header layout (offsets are stable on-disk contract):
 *   [10,12)  header_size (u16)
 *   [20,24)  reserved (must be zero)
 *   [88,92)  CRC32 checksum (over the header with this field zeroed)
 *   [156,160) reserved tail (must be zero)
 * This file is compiled without src/ on the include path, so it cannot pull
 * in internal.h; the CRC32 below mirrors src/checksum.c exactly (reflected
 * poly 0xedb88320, init/final 0xffffffff) and is proven equivalent by a
 * self-check before any corruption case relies on it.
 */
#define TEST_SB_CHECKSUM_OFFSET ((size_t)88)

static uint32_t test_crc32(const uint8_t *data, size_t size)
{
    uint32_t crc = UINT32_C(0xffffffff);
    size_t index;
    unsigned int bit;
    for (index = 0U; index < size; ++index) {
        crc ^= (uint32_t)data[index];
        for (bit = 0U; bit < 8U; ++bit) {
            const uint32_t mask = (uint32_t)(0U - (crc & UINT32_C(1)));
            crc = (crc >> 1U) ^ (UINT32_C(0xedb88320) & mask);
        }
    }
    return ~crc;
}

static void reseal_checksum(uint8_t *page)
{
    uint8_t header[SDB_SUPERBLOCK_HEADER_SIZE];
    uint32_t crc;
    (void)memcpy(header, page, SDB_SUPERBLOCK_HEADER_SIZE);
    (void)memset(header + TEST_SB_CHECKSUM_OFFSET, 0, sizeof(uint32_t));
    crc = test_crc32(header, SDB_SUPERBLOCK_HEADER_SIZE);
    page[TEST_SB_CHECKSUM_OFFSET + 0U] = (uint8_t)(crc & 0xffU);
    page[TEST_SB_CHECKSUM_OFFSET + 1U] = (uint8_t)((crc >> 8U) & 0xffU);
    page[TEST_SB_CHECKSUM_OFFSET + 2U] = (uint8_t)((crc >> 16U) & 0xffU);
    page[TEST_SB_CHECKSUM_OFFSET + 3U] = (uint8_t)((crc >> 24U) & 0xffU);
}

static sdb_superblock_v1 sample_superblock(void)
{
    sdb_superblock_v1 value;
    size_t index;
    (void)memset(&value, 0, sizeof(value));
    value.page_size = 4096U;
    value.generation = UINT64_C(7);
    value.checkpoint_lsn = UINT64_C(42);
    value.root_page = UINT64_C(2);
    value.freelist_page = UINT64_C(9);
    value.next_page_id = UINT64_C(10);
    for (index = 0U; index < SDB_SALT_SIZE; ++index) {
        value.salt[index] = (uint8_t)(index + 1U);
        value.file_id[index] = (uint8_t)(0xa0U + index);
    }
    return value;
}

static void test_roundtrip(void)
{
    uint8_t page[4096];
    sdb_superblock_v1 input = sample_superblock();
    sdb_superblock_v1 output;
    assert(sdb_superblock_v1_encode(&input, page, sizeof(page)) == SDB_OK);
    assert(sdb_superblock_v1_decode(page, sizeof(page), &output) == SDB_OK);
    assert(output.page_size == input.page_size);
    assert(output.generation == input.generation);
    assert(output.checkpoint_lsn == input.checkpoint_lsn);
    assert(output.root_page == input.root_page);
    assert(output.freelist_page == input.freelist_page);
    assert(output.next_page_id == input.next_page_id);
    assert(memcmp(output.salt, input.salt, SDB_SALT_SIZE) == 0);
    assert(memcmp(output.file_id, input.file_id, SDB_FILE_ID_SIZE) == 0);
}

static void test_short_buffers(void)
{
    uint8_t page[SDB_SUPERBLOCK_HEADER_SIZE];
    sdb_superblock_v1 value = sample_superblock();
    assert(sdb_superblock_v1_encode(
        &value, page, SDB_SUPERBLOCK_HEADER_SIZE - 1U
    ) == SDB_E_BUFFER_TOO_SMALL);
    assert(sdb_superblock_v1_decode(
        page, SDB_SUPERBLOCK_HEADER_SIZE - 1U, &value
    ) == SDB_E_BUFFER_TOO_SMALL);
}

static void test_corruption_is_rejected(void)
{
    uint8_t page[4096];
    sdb_superblock_v1 input = sample_superblock();
    sdb_superblock_v1 output;
    assert(sdb_superblock_v1_encode(&input, page, sizeof(page)) == SDB_OK);
    page[40] ^= UINT8_C(0x80);
    assert(sdb_superblock_v1_decode(page, sizeof(page), &output) == SDB_E_CORRUPT);
}

static void test_reserved_bytes_rejected(void)
{
    uint8_t page[4096];
    sdb_superblock_v1 input = sample_superblock();
    sdb_superblock_v1 output;

    /*
     * Self-check: resealing an untouched encode must reproduce the stored
     * checksum, proving test_crc32/reseal_checksum match the library. Without
     * this a wrong CRC would make decode fail on the checksum gate and the
     * reserved-byte assertions below would pass for the wrong reason.
     */
    assert(sdb_superblock_v1_encode(&input, page, sizeof(page)) == SDB_OK);
    {
        uint8_t saved[4];
        (void)memcpy(saved, page + TEST_SB_CHECKSUM_OFFSET, sizeof(saved));
        reseal_checksum(page);
        assert(memcmp(saved, page + TEST_SB_CHECKSUM_OFFSET, sizeof(saved)) == 0);
    }

    /* nonzero byte in the reserved region [20,24), checksum still valid */
    assert(sdb_superblock_v1_encode(&input, page, sizeof(page)) == SDB_OK);
    page[20] = (uint8_t)0x01;
    reseal_checksum(page);
    assert(sdb_superblock_v1_decode(page, sizeof(page), &output) == SDB_E_CORRUPT);

    /* nonzero byte in the reserved tail [156,160), checksum still valid */
    assert(sdb_superblock_v1_encode(&input, page, sizeof(page)) == SDB_OK);
    page[SDB_SUPERBLOCK_HEADER_SIZE - 1U] = (uint8_t)0x01;
    reseal_checksum(page);
    assert(sdb_superblock_v1_decode(page, sizeof(page), &output) == SDB_E_CORRUPT);
}

static void test_wrong_header_size_rejected(void)
{
    uint8_t page[4096];
    sdb_superblock_v1 input = sample_superblock();
    sdb_superblock_v1 output;
    const uint16_t bad = (uint16_t)(SDB_SUPERBLOCK_HEADER_SIZE + 1U);

    /*
     * Corrupt the header_size field (offset 10) and reseal so the checksum
     * stays valid: the rejection must come from the header_size gate itself,
     * SDB_E_CORRUPT, not from an invalidated checksum.
     */
    assert(sdb_superblock_v1_encode(&input, page, sizeof(page)) == SDB_OK);
    page[10] = (uint8_t)(bad & 0xffU);
    page[11] = (uint8_t)((bad >> 8U) & 0xffU);
    reseal_checksum(page);
    assert(sdb_superblock_v1_decode(page, sizeof(page), &output) == SDB_E_CORRUPT);
}

static void test_magic_and_version_are_distinct_errors(void)
{
    uint8_t page[4096];
    sdb_superblock_v1 input = sample_superblock();
    sdb_superblock_v1 output;
    assert(sdb_superblock_v1_encode(&input, page, sizeof(page)) == SDB_OK);
    page[0] ^= UINT8_C(1);
    assert(sdb_superblock_v1_decode(page, sizeof(page), &output) == SDB_E_BAD_MAGIC);

    /*
     * An unknown future version (not V1, not V2) is still rejected as
     * SDB_E_UNSUPPORTED_VERSION at the decode gate, which sits before the
     * checksum verification, so a raw byte poke needs no reseal.
     */
    assert(sdb_superblock_v1_encode(&input, page, sizeof(page)) == SDB_OK);
    page[8] = UINT8_C(99);
    assert(
        sdb_superblock_v1_decode(page, sizeof(page), &output)
        == SDB_E_UNSUPPORTED_VERSION
    );
}

/*
 * The current on-disk format is V2, so a freshly encoded superblock must stamp
 * version 2 at offset 8. A V1 image (produced by a pre-layout-change build)
 * must still DECODE cleanly — decode is layout-agnostic and used by the
 * migration path to read a legacy file; the refusal to OPEN a V1 file with the
 * V2 object-key layout lives one layer up, in sdb_database_open, keyed off the
 * format_version surfaced in sdb_superblock_read_result (covered by the engine
 * and legacy-fixture tests), not here.
 */
static void test_format_version_v2_stamped_and_v1_still_decodes(void)
{
    uint8_t page[4096];
    sdb_superblock_v1 input = sample_superblock();
    sdb_superblock_v1 output;

    assert(sdb_superblock_v1_encode(&input, page, sizeof(page)) == SDB_OK);
    assert(page[8] == UINT8_C(2));
    assert(page[9] == UINT8_C(0));
    assert(sdb_superblock_v1_decode(page, sizeof(page), &output) == SDB_OK);

    /*
     * Rewrite the version to V1 and reseal so the checksum stays valid: decode
     * must still accept it (the open-time gate, not decode, refuses V1).
     */
    assert(sdb_superblock_v1_encode(&input, page, sizeof(page)) == SDB_OK);
    page[8] = UINT8_C(1);
    page[9] = UINT8_C(0);
    reseal_checksum(page);
    assert(sdb_superblock_v1_decode(page, sizeof(page), &output) == SDB_OK);
}

static void test_invalid_configuration(void)
{
    uint8_t page[4096];
    sdb_superblock_v1 value = sample_superblock();
    value.page_size = 5000U;
    assert(sdb_superblock_v1_encode(&value, page, sizeof(page)) == SDB_E_INVALID_ARGUMENT);

    value = sample_superblock();
    (void)memset(value.salt, 0, sizeof(value.salt));
    assert(sdb_superblock_v1_encode(&value, page, sizeof(page)) == SDB_OK);

    value = sample_superblock();
    value.flags = SDB_FLAG_ENCRYPTED;
    value.key_wrap_id = 3U;
    value.kdf_iterations = SDB_DEFAULT_KDF_ITERATIONS;
    {
        size_t index;
        for (index = 0U; index < SDB_WRAPPED_KEY_SIZE; ++index) {
            value.wrapped_key[index] = (uint8_t)(0x40U + index);
        }
        for (index = 0U; index < SDB_KEY_WRAP_TAG_SIZE; ++index) {
            value.key_wrap_tag[index] = (uint8_t)(0x60U + index);
        }
    }
    (void)memset(value.salt, 0, sizeof(value.salt));
    assert(sdb_superblock_v1_encode(&value, page, sizeof(page)) == SDB_E_INVALID_ARGUMENT);
}

static void test_encryption_metadata_roundtrip(void)
{
    uint8_t page[4096];
    sdb_superblock_v1 input = sample_superblock();
    sdb_superblock_v1 output;
    size_t index;
    input.flags = SDB_FLAG_ENCRYPTED;
    input.key_wrap_id = 3U;
    input.kdf_iterations = SDB_MIN_KDF_ITERATIONS;
    for (index = 0U; index < SDB_WRAPPED_KEY_SIZE; ++index) {
        input.wrapped_key[index] = (uint8_t)(index + 1U);
    }
    for (index = 0U; index < SDB_KEY_WRAP_TAG_SIZE; ++index) {
        input.key_wrap_tag[index] = (uint8_t)(0x80U + index);
    }
    assert(sdb_superblock_v1_encode(
        &input, page, sizeof(page)
    ) == SDB_OK);
    assert(sdb_superblock_v1_decode(
        page, sizeof(page), &output
    ) == SDB_OK);
    assert(output.flags == SDB_FLAG_ENCRYPTED);
    assert(output.key_wrap_id == input.key_wrap_id);
    assert(output.kdf_iterations == input.kdf_iterations);
    assert(memcmp(
        output.wrapped_key, input.wrapped_key, SDB_WRAPPED_KEY_SIZE
    ) == 0);
    assert(memcmp(
        output.key_wrap_tag, input.key_wrap_tag, SDB_KEY_WRAP_TAG_SIZE
    ) == 0);
}

int main(void)
{
    test_roundtrip();
    test_short_buffers();
    test_corruption_is_rejected();
    test_reserved_bytes_rejected();
    test_wrong_header_size_rejected();
    test_magic_and_version_are_distinct_errors();
    test_format_version_v2_stamped_and_v1_still_decodes();
    test_invalid_configuration();
    test_encryption_metadata_roundtrip();
    (void)puts("superblock tests: ok");
    return 0;
}
