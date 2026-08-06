
#include "shibadb.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef enum {
    SDB_SB_FIELD_OK = 0,
    SDB_SB_FIELD_NULL_ARG,
    SDB_SB_FIELD_PAGE_SIZE,
    SDB_SB_FIELD_FLAGS,
    SDB_SB_FIELD_NEXT_PAGE_ID,
    SDB_SB_FIELD_FILE_ID,
    SDB_SB_FIELD_SALT,
    SDB_SB_FIELD_KEY_WRAP_ID,
    SDB_SB_FIELD_KDF_ITERATIONS,
    SDB_SB_FIELD_WRAPPED_KEY,
    SDB_SB_FIELD_KEY_WRAP_TAG,
    SDB_SB_FIELD_ENC_MISMATCH
} sdb_sb_field;

extern sdb_sb_field sdb_validate_superblock_field(const sdb_superblock_v1 *sb);

static sdb_superblock_v1 valid_plain(void)
{
    sdb_superblock_v1 v;
    size_t i;
    (void)memset(&v, 0, sizeof(v));
    v.page_size = 4096U;
    v.next_page_id = 1U;
    for (i = 0U; i < SDB_FILE_ID_SIZE; ++i) {
        v.file_id[i] = (uint8_t)(0xa0U + i);
    }
    return v;
}

static sdb_superblock_v1 valid_encrypted(void)
{
    sdb_superblock_v1 v = valid_plain();
    size_t i;
    v.flags = SDB_FLAG_ENCRYPTED;
    v.key_wrap_id = 1U;
    v.kdf_iterations = SDB_MIN_KDF_ITERATIONS;
    for (i = 0U; i < SDB_SALT_SIZE; ++i) {
        v.salt[i] = (uint8_t)(i + 1U);
    }
    for (i = 0U; i < SDB_WRAPPED_KEY_SIZE; ++i) {
        v.wrapped_key[i] = (uint8_t)(0x11U + i);
    }
    for (i = 0U; i < SDB_KEY_WRAP_TAG_SIZE; ++i) {
        v.key_wrap_tag[i] = (uint8_t)(0x22U + i);
    }
    return v;
}

int main(void)
{
    sdb_superblock_v1 v;

    v = valid_plain();
    assert(sdb_validate_superblock_field(&v) == SDB_SB_FIELD_OK);

    v = valid_encrypted();
    assert(sdb_validate_superblock_field(&v) == SDB_SB_FIELD_OK);

    assert(sdb_validate_superblock_field(NULL) == SDB_SB_FIELD_NULL_ARG);

    v = valid_plain();
    v.page_size = 0U;
    assert(sdb_validate_superblock_field(&v) == SDB_SB_FIELD_PAGE_SIZE);

    v = valid_plain();
    v.page_size = 3000U;
    assert(sdb_validate_superblock_field(&v) == SDB_SB_FIELD_PAGE_SIZE);

    v = valid_plain();
    v.flags = 0xdeadbeefU;
    assert(sdb_validate_superblock_field(&v) == SDB_SB_FIELD_FLAGS);

    v = valid_plain();
    v.next_page_id = 0U;
    assert(sdb_validate_superblock_field(&v) == SDB_SB_FIELD_NEXT_PAGE_ID);

    v = valid_plain();
    (void)memset(v.file_id, 0, SDB_FILE_ID_SIZE);
    assert(sdb_validate_superblock_field(&v) == SDB_SB_FIELD_FILE_ID);

    v = valid_encrypted();
    (void)memset(v.salt, 0, SDB_SALT_SIZE);
    assert(sdb_validate_superblock_field(&v) == SDB_SB_FIELD_SALT);

    v = valid_encrypted();
    v.key_wrap_id = 0U;
    assert(sdb_validate_superblock_field(&v) == SDB_SB_FIELD_KEY_WRAP_ID);

    v = valid_encrypted();
    v.kdf_iterations = 1U;
    assert(sdb_validate_superblock_field(&v) == SDB_SB_FIELD_KDF_ITERATIONS);

    v = valid_encrypted();
    v.kdf_iterations = SDB_MAX_KDF_ITERATIONS + 1U;
    assert(sdb_validate_superblock_field(&v) == SDB_SB_FIELD_KDF_ITERATIONS);

    v = valid_encrypted();
    (void)memset(v.wrapped_key, 0, SDB_WRAPPED_KEY_SIZE);
    assert(sdb_validate_superblock_field(&v) == SDB_SB_FIELD_WRAPPED_KEY);

    v = valid_encrypted();
    (void)memset(v.key_wrap_tag, 0, SDB_KEY_WRAP_TAG_SIZE);
    assert(sdb_validate_superblock_field(&v) == SDB_SB_FIELD_KEY_WRAP_TAG);

    v = valid_plain();
    v.key_wrap_id = 1U;
    assert(sdb_validate_superblock_field(&v) == SDB_SB_FIELD_ENC_MISMATCH);

    v = valid_plain();
    v.kdf_iterations = SDB_MIN_KDF_ITERATIONS;
    assert(sdb_validate_superblock_field(&v) == SDB_SB_FIELD_ENC_MISMATCH);

    v = valid_plain();
    v.wrapped_key[0] = 0x99U;
    assert(sdb_validate_superblock_field(&v) == SDB_SB_FIELD_ENC_MISMATCH);

    v = valid_plain();
    v.key_wrap_tag[7] = 0x77U;
    assert(sdb_validate_superblock_field(&v) == SDB_SB_FIELD_ENC_MISMATCH);

    (void)puts("superblock field diagnostics: ok");
    return 0;
}
