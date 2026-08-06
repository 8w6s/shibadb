#include "btree.h"
#include "internal.h"
#include "pager.h"
#include "superblock_store.h"

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

static void corrupt_superblock_byte(uint64_t offset)
{
    sdb_file file;
    uint8_t byte;
    assert(sdb_file_open_existing(test_path, true, &file) == SDB_OK);
    assert(sdb_file_read_full(&file, offset, &byte, 1U) == SDB_OK);
    byte ^= UINT8_C(0x40);
    assert(sdb_file_write_full(&file, offset, &byte, 1U) == SDB_OK);
    assert(sdb_file_sync(&file) == SDB_OK);
    assert(sdb_file_close(&file) == SDB_OK);
}

static void assert_authenticated_control_tamper_refused(void)
{
    sdb_file file;
    uint8_t slots[2][SDB_SUPERBLOCK_SLOT_SIZE];
    size_t slot;
    sdb_pager rejected;
    assert(sdb_file_open_existing(test_path, true, &file) == SDB_OK);
    for (slot = 0U; slot < 2U; ++slot) {
        uint8_t *header = slots[slot];
        assert(sdb_file_read_full(
            &file,
            (uint64_t)slot * (uint64_t)SDB_SUPERBLOCK_SLOT_SIZE,
            header,
            SDB_SUPERBLOCK_SLOT_SIZE
        ) == SDB_OK);
        /*
         * root_page is outside key-wrap AAD. Re-seal the public CRC so only
         * the keyed header authenticator can detect this modification.
         */
        header[40U] ^= UINT8_C(0x04);
        sdb_write_u32_le(header + SDB_SUPERBLOCK_CHECKSUM_OFFSET, 0U);
        sdb_write_u32_le(
            header + SDB_SUPERBLOCK_CHECKSUM_OFFSET,
            sdb_crc32(header, SDB_SUPERBLOCK_HEADER_SIZE)
        );
        assert(sdb_file_write_full(
            &file,
            (uint64_t)slot * (uint64_t)SDB_SUPERBLOCK_SLOT_SIZE,
            header,
            SDB_SUPERBLOCK_SLOT_SIZE
        ) == SDB_OK);
    }
    assert(sdb_file_sync(&file) == SDB_OK);
    assert(sdb_file_close(&file) == SDB_OK);
    assert(sdb_pager_open_encrypted(
        test_path, old_password, sizeof(old_password) - 1U, &rejected
    ) == SDB_E_CORRUPT);

    /*
     * Undo only the controlled header mutation and CRC rewrite; the original
     * keyed tags at bytes 160..191 were never touched.
     */
    assert(sdb_file_open_existing(test_path, true, &file) == SDB_OK);
    for (slot = 0U; slot < 2U; ++slot) {
        uint8_t *header = slots[slot];
        header[40U] ^= UINT8_C(0x04);
        sdb_write_u32_le(header + SDB_SUPERBLOCK_CHECKSUM_OFFSET, 0U);
        sdb_write_u32_le(
            header + SDB_SUPERBLOCK_CHECKSUM_OFFSET,
            sdb_crc32(header, SDB_SUPERBLOCK_HEADER_SIZE)
        );
        assert(sdb_file_write_full(
            &file,
            (uint64_t)slot * (uint64_t)SDB_SUPERBLOCK_SLOT_SIZE,
            header,
            SDB_SUPERBLOCK_SLOT_SIZE
        ) == SDB_OK);
    }
    assert(sdb_file_sync(&file) == SDB_OK);
    assert(sdb_file_close(&file) == SDB_OK);
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
    assert((pager.superblock.flags & SDB_FLAG_HEADER_AUTH) != 0U);
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

    assert_authenticated_control_tamper_refused();
    corrupt_superblock_byte(40U);
    assert(sdb_pager_open(test_path, &pager) == SDB_E_AUTHENTICATION);
    assert(sdb_pager_open_encrypted(
        test_path,
        (const uint8_t *)"wrong",
        5U,
        &pager
    ) == SDB_E_AUTHENTICATION);
    {
        sdb_superblock_read_result mirrors;
        assert(sdb_superblock_store_read(test_path, &mirrors) == SDB_OK);
        assert(mirrors.valid_mirror_count == 1U);
    }
    assert(sdb_pager_open_encrypted(
        test_path,
        old_password,
        sizeof(old_password) - 1U,
        &pager
    ) == SDB_OK);
    {
        sdb_superblock_read_result mirrors;
        assert(sdb_superblock_store_read_file(
            &pager.file, &mirrors
        ) == SDB_OK);
        assert(mirrors.valid_mirror_count == 2U);
    }
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
        static const uint8_t encrypted_magic[4] = {
            (uint8_t)'S', (uint8_t)'E', (uint8_t)'N', (uint8_t)'1'
        };
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
        /*
         * Positive control for the negative scan above. Without it, a
         * contains_bytes that always returned false (or a file read that
         * silently produced an empty buffer) would pass the assertion
         * vacuously. The encrypted-envelope magic MUST be present on disk:
         * this proves the scanner really inspects the bytes and that pages
         * are stored as encrypted envelopes rather than plaintext.
         */
        assert(contains_bytes(
            bytes,
            (size_t)file_size,
            encrypted_magic,
            sizeof(encrypted_magic)
        ));
        free(bytes);
        assert(sdb_file_close(&file) == SDB_OK);
    }

    (void)remove(wal_path);
    (void)remove(test_path);
    (void)puts("encryption tests: ok");
    return 0;
}
