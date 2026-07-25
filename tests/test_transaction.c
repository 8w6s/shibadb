#include "pager.h"
#include "wal.h"
#include "internal.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *test_path = "test-transaction.tmp";
static const char *wal_path = "test-transaction.tmp.wal";

static void assert_payload(
    sdb_pager *pager, uint64_t page_id, const uint8_t *expected, size_t size
)
{
    uint8_t page[4096];
    sdb_page_view view;
    assert(sdb_pager_read(
        pager, page_id, page, sizeof(page), &view
    ) == SDB_OK);
    assert(view.payload_size == size);
    assert(memcmp(view.payload, expected, size) == 0);
}

static void downgrade_wal_to_v1(void)
{
    sdb_file file;
    uint64_t size_u64;
    uint8_t *bytes;
    size_t size;
    uint8_t *commit;
    assert(sdb_file_open_existing(wal_path, true, &file) == SDB_OK);
    assert(sdb_file_size(&file, &size_u64) == SDB_OK);
    assert(size_u64 <= (uint64_t)SIZE_MAX);
    size = (size_t)size_u64;
    assert(size >= SDB_WAL_HEADER_SIZE + SDB_WAL_COMMIT_SIZE);
    bytes = (uint8_t *)malloc(size);
    assert(bytes != NULL);
    assert(sdb_file_read_full(&file, 0U, bytes, size) == SDB_OK);
    sdb_write_u16_le(bytes + 4U, UINT16_C(1));
    (void)memset(bytes + 40U, 0, 20U);
    sdb_write_u32_le(bytes + 60U, 0U);
    sdb_write_u32_le(
        bytes + 60U,
        sdb_crc32_zeroed_range(bytes, 64U, 60U, 4U)
    );
    commit = bytes + size - SDB_WAL_COMMIT_SIZE;
    sdb_write_u16_le(commit + 4U, UINT16_C(1));
    sdb_write_u32_le(
        commit + 16U, sdb_crc32(bytes, size - SDB_WAL_COMMIT_SIZE)
    );
    sdb_write_u32_le(commit + 20U, 0U);
    sdb_write_u32_le(
        commit + 20U,
        sdb_crc32_zeroed_range(commit, SDB_WAL_COMMIT_SIZE, 20U, 4U)
    );
    assert(sdb_file_write_full(&file, 0U, bytes, size) == SDB_OK);
    assert(sdb_file_sync(&file) == SDB_OK);
    assert(sdb_file_close(&file) == SDB_OK);
    free(bytes);
}

static void duplicate_second_wal_record(void)
{
    const size_t record_size =
        SDB_WAL_RECORD_HEADER_SIZE + (size_t)4096U;
    sdb_file file;
    uint64_t size_u64;
    uint8_t *bytes;
    uint8_t *commit;
    size_t size;
    assert(sdb_file_open_existing(wal_path, true, &file) == SDB_OK);
    assert(sdb_file_size(&file, &size_u64) == SDB_OK);
    assert(size_u64 <= (uint64_t)SIZE_MAX);
    size = (size_t)size_u64;
    assert(size == SDB_WAL_HEADER_SIZE
        + (2U * record_size) + SDB_WAL_COMMIT_SIZE);
    bytes = (uint8_t *)malloc(size);
    assert(bytes != NULL);
    assert(sdb_file_read_full(&file, 0U, bytes, size) == SDB_OK);
    (void)memcpy(
        bytes + SDB_WAL_HEADER_SIZE + record_size,
        bytes + SDB_WAL_HEADER_SIZE,
        record_size
    );
    commit = bytes + size - SDB_WAL_COMMIT_SIZE;
    sdb_write_u32_le(
        commit + 16U, sdb_crc32(bytes, size - SDB_WAL_COMMIT_SIZE)
    );
    sdb_write_u32_le(commit + 20U, 0U);
    sdb_write_u32_le(
        commit + 20U,
        sdb_crc32_zeroed_range(
            commit, SDB_WAL_COMMIT_SIZE, 20U, sizeof(uint32_t)
        )
    );
    assert(sdb_file_write_full(&file, 0U, bytes, size) == SDB_OK);
    assert(sdb_file_sync(&file) == SDB_OK);
    assert(sdb_file_close(&file) == SDB_OK);
    free(bytes);
}

int main(void)
{
    sdb_pager pager;
    sdb_txn txn;
    uint8_t salt[16] = {3U};
    uint8_t file_id[16] = {4U};
    static const uint8_t old_a[] = "old-a";
    static const uint8_t old_b[] = "old-b";
    static const uint8_t new_a[] = "new-a";
    static const uint8_t new_b[] = "new-b";
    static const uint8_t recovered_a[] = "recovered-a";
    static const uint8_t recovered_b[] = "recovered-b";
    static const uint8_t reserved_value[] = "transaction-reserved";
    uint8_t encoded_a[4096];
    uint8_t encoded_b[4096];
    sdb_wal_page wal_pages[2];
    sdb_superblock_v1 superblock;
    uint64_t first;
    uint64_t second;
    uint64_t recovery_lsn;
    uint64_t reserved;

    (void)remove(test_path);
    (void)remove(wal_path);
    assert(sdb_pager_create(
        test_path, 4096U, salt, file_id, &pager
    ) == SDB_OK);
    assert(sdb_pager_allocate(&pager, &first) == SDB_OK);
    assert(sdb_pager_allocate(&pager, &second) == SDB_OK);
    assert(sdb_pager_write(
        &pager, first, (uint16_t)SDB_PAGE_TYPE_DATA, 0U,
        old_a, sizeof(old_a)
    ) == SDB_OK);
    assert(sdb_pager_write(
        &pager, second, (uint16_t)SDB_PAGE_TYPE_DATA, 0U,
        old_b, sizeof(old_b)
    ) == SDB_OK);

    assert(sdb_txn_begin(&pager, &txn) == SDB_OK);
    {
        sdb_txn nested;
        uint64_t forbidden;
        assert(sdb_txn_begin(&pager, &nested) == SDB_E_INVALID_ARGUMENT);
        assert(sdb_pager_allocate(
            &pager, &forbidden
        ) == SDB_E_INVALID_ARGUMENT);
        assert(sdb_pager_close(&pager) == SDB_E_INVALID_ARGUMENT);
    }
    assert(sdb_txn_put(
        &txn, first, (uint16_t)SDB_PAGE_TYPE_DATA, new_a, sizeof(new_a)
    ) == SDB_OK);
    assert(sdb_txn_put(
        &txn, second, (uint16_t)SDB_PAGE_TYPE_DATA, new_b, sizeof(new_b)
    ) == SDB_OK);
    assert(sdb_txn_commit(&txn) == SDB_OK);
    assert(pager.superblock.checkpoint_lsn == 1U);
    assert_payload(&pager, first, new_a, sizeof(new_a));
    assert_payload(&pager, second, new_b, sizeof(new_b));

    assert(sdb_txn_begin(&pager, &txn) == SDB_OK);
    assert(sdb_txn_put(
        &txn, first, (uint16_t)SDB_PAGE_TYPE_DATA, old_a, sizeof(old_a)
    ) == SDB_OK);
    sdb_txn_abort(&txn);
    assert_payload(&pager, first, new_a, sizeof(new_a));

    /* Aborted reservations never advance the durable allocation watermark. */
    reserved = pager.superblock.next_page_id;
    assert(sdb_txn_begin(&pager, &txn) == SDB_OK);
    {
        uint64_t allocated;
        assert(sdb_txn_allocate(&txn, &allocated) == SDB_OK);
        assert(allocated == reserved);
        assert(sdb_txn_put(
            &txn,
            allocated,
            (uint16_t)SDB_PAGE_TYPE_DATA,
            reserved_value,
            sizeof(reserved_value)
        ) == SDB_OK);
    }
    sdb_txn_abort(&txn);
    assert(pager.superblock.next_page_id == reserved);

    /* A reservation without a staged page image cannot be committed. */
    assert(sdb_txn_begin(&pager, &txn) == SDB_OK);
    {
        uint64_t allocated;
        assert(sdb_txn_allocate(&txn, &allocated) == SDB_OK);
        assert(allocated == reserved);
    }
    assert(sdb_txn_commit(&txn) == SDB_E_INVALID_ARGUMENT);
    assert(pager.superblock.next_page_id == reserved);

    /* A committed reservation and its page become visible together. */
    assert(sdb_txn_begin(&pager, &txn) == SDB_OK);
    {
        uint64_t allocated;
        assert(sdb_txn_allocate(&txn, &allocated) == SDB_OK);
        assert(allocated == reserved);
        assert(sdb_txn_put(
            &txn,
            allocated,
            (uint16_t)SDB_PAGE_TYPE_DATA,
            reserved_value,
            sizeof(reserved_value)
        ) == SDB_OK);
    }
    assert(sdb_txn_commit(&txn) == SDB_OK);
    assert(pager.superblock.next_page_id == reserved + 1U);
    assert_payload(
        &pager, reserved, reserved_value, sizeof(reserved_value)
    );

    /* A freelist reservation is likewise restored by abort. */
    assert(sdb_pager_free(&pager, reserved) == SDB_OK);
    assert(pager.superblock.freelist_page == reserved);
    assert(sdb_txn_begin(&pager, &txn) == SDB_OK);
    {
        uint64_t allocated;
        assert(sdb_txn_allocate(&txn, &allocated) == SDB_OK);
        assert(allocated == reserved);
    }
    sdb_txn_abort(&txn);
    assert(pager.superblock.freelist_page == reserved);
    assert(sdb_txn_begin(&pager, &txn) == SDB_OK);
    {
        uint64_t allocated;
        assert(sdb_txn_allocate(&txn, &allocated) == SDB_OK);
        assert(allocated == reserved);
        assert(sdb_txn_put(
            &txn,
            allocated,
            (uint16_t)SDB_PAGE_TYPE_DATA,
            reserved_value,
            sizeof(reserved_value)
        ) == SDB_OK);
    }
    assert(sdb_txn_commit(&txn) == SDB_OK);
    assert(pager.superblock.freelist_page == 0U);
    assert_payload(
        &pager, reserved, reserved_value, sizeof(reserved_value)
    );

    /*
     * Simulate a crash after WAL commit but before either database page was
     * applied. Reopen must replay both pages and advance the checkpoint.
     */
    superblock = pager.superblock;
    recovery_lsn = superblock.checkpoint_lsn + 1U;
    assert(sdb_page_encode(
        encoded_a, sizeof(encoded_a), (uint16_t)SDB_PAGE_TYPE_DATA,
        first, recovery_lsn, recovered_a, sizeof(recovered_a)
    ) == SDB_OK);
    assert(sdb_page_encode(
        encoded_b, sizeof(encoded_b), (uint16_t)SDB_PAGE_TYPE_DATA,
        second, recovery_lsn, recovered_b, sizeof(recovered_b)
    ) == SDB_OK);
    wal_pages[0].page_id = first;
    wal_pages[0].bytes = encoded_a;
    wal_pages[1].page_id = second;
    wal_pages[1].bytes = encoded_b;
    assert(sdb_pager_close(&pager) == SDB_OK);
    assert(sdb_wal_write_committed(
        wal_path, &superblock, recovery_lsn, wal_pages, 2U
    ) == SDB_OK);
    downgrade_wal_to_v1();

    assert(sdb_pager_open(test_path, &pager) == SDB_OK);
    assert(pager.superblock.checkpoint_lsn == recovery_lsn);
    assert_payload(&pager, first, recovered_a, sizeof(recovered_a));
    assert_payload(&pager, second, recovered_b, sizeof(recovered_b));
    assert(sdb_pager_close(&pager) == SDB_OK);

    /*
     * A checksummed committed WAL with a duplicate record is rejected before
     * any page is applied. Removing it must expose the unchanged checkpoint.
     */
    assert(sdb_pager_open(test_path, &pager) == SDB_OK);
    superblock = pager.superblock;
    recovery_lsn = superblock.checkpoint_lsn + 1U;
    assert(sdb_page_encode(
        encoded_a, sizeof(encoded_a), (uint16_t)SDB_PAGE_TYPE_DATA,
        first, recovery_lsn, new_a, sizeof(new_a)
    ) == SDB_OK);
    assert(sdb_page_encode(
        encoded_b, sizeof(encoded_b), (uint16_t)SDB_PAGE_TYPE_DATA,
        second, recovery_lsn, new_b, sizeof(new_b)
    ) == SDB_OK);
    wal_pages[0].page_id = first;
    wal_pages[0].bytes = encoded_a;
    wal_pages[1].page_id = second;
    wal_pages[1].bytes = encoded_b;
    assert(sdb_pager_close(&pager) == SDB_OK);
    assert(sdb_wal_write_committed(
        wal_path, &superblock, recovery_lsn, wal_pages, 2U
    ) == SDB_OK);
    duplicate_second_wal_record();
    assert(sdb_pager_open(test_path, &pager) == SDB_E_CORRUPT);
    assert(!pager.open);
    assert(sdb_file_remove(wal_path, false) == SDB_OK);
    assert(sdb_pager_open(test_path, &pager) == SDB_OK);
    assert_payload(&pager, first, recovered_a, sizeof(recovered_a));
    assert_payload(&pager, second, recovered_b, sizeof(recovered_b));
    assert(sdb_pager_close(&pager) == SDB_OK);

    /* Oversized sparse WAL files are rejected before allocation or parsing. */
    {
        sdb_file oversized;
        assert(sdb_file_create_new(wal_path, &oversized) == SDB_OK);
        assert(sdb_file_resize(
            &oversized, SDB_WAL_MAX_BYTES + 1U
        ) == SDB_OK);
        assert(sdb_file_sync(&oversized) == SDB_OK);
        assert(sdb_file_close(&oversized) == SDB_OK);
    }
    assert(sdb_pager_open(test_path, &pager) == SDB_E_CORRUPT);
    assert(!pager.open);

    (void)remove(wal_path);
    (void)remove(test_path);
    (void)puts("transaction tests: ok");
    return 0;
}
