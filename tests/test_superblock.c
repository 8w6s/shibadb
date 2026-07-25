#include "shibadb.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

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

static void test_magic_and_version_are_distinct_errors(void)
{
    uint8_t page[4096];
    sdb_superblock_v1 input = sample_superblock();
    sdb_superblock_v1 output;
    assert(sdb_superblock_v1_encode(&input, page, sizeof(page)) == SDB_OK);
    page[0] ^= UINT8_C(1);
    assert(sdb_superblock_v1_decode(page, sizeof(page), &output) == SDB_E_BAD_MAGIC);

    assert(sdb_superblock_v1_encode(&input, page, sizeof(page)) == SDB_OK);
    page[8] = UINT8_C(2);
    assert(
        sdb_superblock_v1_decode(page, sizeof(page), &output)
        == SDB_E_UNSUPPORTED_VERSION
    );
}

static void test_invalid_configuration(void)
{
    uint8_t page[4096];
    sdb_superblock_v1 value = sample_superblock();
    value.page_size = 5000U;
    assert(sdb_superblock_v1_encode(&value, page, sizeof(page)) == SDB_E_INVALID_ARGUMENT);
    /*
     * Salt is only required when the encryption flag is set (S.7): an
     * unencrypted database is allowed to carry a zero salt, but an
     * encrypted one is not.
     */
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
    test_magic_and_version_are_distinct_errors();
    test_invalid_configuration();
    test_encryption_metadata_roundtrip();
    (void)puts("superblock tests: ok");
    return 0;
}
