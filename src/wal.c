#include "wal.h"

#include "crypto.h"
#include "encrypted_page.h"
#include "internal.h"

#include <stdlib.h>
#include <string.h>

#define SDB_WAL_VERSION_V1 UINT16_C(1)
#define SDB_WAL_VERSION UINT16_C(2)
#define SDB_WAL_HEADER_CHECKSUM_OFFSET ((size_t)60)
#define SDB_WAL_COMMIT_CHECKSUM_OFFSET ((size_t)20)

static const uint8_t sdb_wal_magic[4] = {
    (uint8_t)'S', (uint8_t)'W', (uint8_t)'A', (uint8_t)'L'
};
static const uint8_t sdb_wal_commit_magic[4] = {
    (uint8_t)'S', (uint8_t)'C', (uint8_t)'M', (uint8_t)'T'
};
#if SDB_TESTING
static size_t sdb_wal_test_fail_after = SIZE_MAX;
#endif

static sdb_status sdb_wal_id_set_create(
    size_t page_count, uint64_t **slots_out, size_t *capacity_out
)
{
    size_t required;
    size_t capacity = 1U;
    uint64_t *slots;
    if (page_count == 0U || slots_out == NULL || capacity_out == NULL
        || page_count > SIZE_MAX / 2U) {
        return SDB_E_INVALID_ARGUMENT;
    }
    required = page_count * 2U;
    while (capacity < required) {
        if (capacity > SIZE_MAX / 2U) {
            return SDB_E_OVERFLOW;
        }
        capacity *= 2U;
    }
    if (capacity > SIZE_MAX / sizeof(*slots)) {
        return SDB_E_OVERFLOW;
    }
    slots = (uint64_t *)calloc(capacity, sizeof(*slots));
    if (slots == NULL) {
        return SDB_E_INTERNAL;
    }
    *slots_out = slots;
    *capacity_out = capacity;
    return SDB_OK;
}

static bool sdb_wal_id_set_insert(
    uint64_t *slots, size_t capacity, uint64_t page_id
)
{
    size_t index;
    /*
     * Reject the zero page id so the set never confuses "empty slot" with
     * "page 0 inserted" (A1.1). All in-tree callers filter zero already; this
     * guards future callers that don't.
     */
    if (page_id == 0U) {
        return false;
    }
    index = (size_t)(
        (page_id * UINT64_C(11400714819323198485))
        & (uint64_t)(capacity - 1U)
    );
    while (slots[index] != 0U) {
        if (slots[index] == page_id) {
            return false;
        }
        index = (index + 1U) & (capacity - 1U);
    }
    slots[index] = page_id;
    return true;
}

static sdb_status sdb_wal_expected_size(
    size_t page_size, size_t page_count, size_t *size_out
)
{
    size_t record_size;
    size_t records_size;
    size_t without_commit;
    if (!sdb_checked_add_size(
            SDB_WAL_RECORD_HEADER_SIZE, page_size, &record_size
        )
        || !sdb_checked_mul_size(record_size, page_count, &records_size)
        || !sdb_checked_add_size(
            SDB_WAL_HEADER_SIZE, records_size, &without_commit
        )
        || !sdb_checked_add_size(
            without_commit, SDB_WAL_COMMIT_SIZE, size_out
        )) {
        return SDB_E_OVERFLOW;
    }
    return SDB_OK;
}

static sdb_status sdb_wal_open_reset(const char *path, sdb_file *file_out)
{
    sdb_status status = sdb_file_open_existing(path, true, file_out);
    bool created = false;
    if (status != SDB_OK) {
        status = sdb_file_create_new(path, file_out);
        created = status == SDB_OK;
    }
    if (status == SDB_OK) {
        status = sdb_file_resize(file_out, 0U);
    }
    if (status == SDB_OK && created) {
        status = sdb_file_sync_parent_directory(path);
    }
    if (status != SDB_OK) {
        (void)sdb_file_close(file_out);
    }
    return status;
}

sdb_status sdb_wal_write_committed(
    const char *wal_path,
    const sdb_superblock_v1 *superblock,
    uint64_t txn_id,
    const sdb_wal_page *pages,
    size_t page_count
)
{
    sdb_file file;
    uint8_t *buffer;
    uint8_t *cursor;
    size_t total_size;
    size_t data_size;
    size_t index;
    uint32_t checksum;
    sdb_status status;
    sdb_status close_status = SDB_OK;
    bool opened = false;

    if (wal_path == NULL || superblock == NULL || pages == NULL
        || txn_id == 0U || page_count == 0U
        || page_count > (size_t)SDB_WAL_MAX_RECORDS
        || page_count > (size_t)UINT32_MAX) {
        return SDB_E_INVALID_ARGUMENT;
    }
    status = sdb_wal_expected_size(
        (size_t)superblock->page_size, page_count, &total_size
    );
    if (status != SDB_OK || (uint64_t)total_size > SDB_WAL_MAX_BYTES) {
        return status != SDB_OK ? status : SDB_E_OVERFLOW;
    }
    {
        uint64_t *page_ids;
        size_t id_capacity;
        status = sdb_wal_id_set_create(
            page_count, &page_ids, &id_capacity
        );
        if (status != SDB_OK) {
            return status;
        }
        for (index = 0U; index < page_count; ++index) {
            sdb_page_view view;
            if (pages[index].page_id == 0U || pages[index].bytes == NULL
                || pages[index].page_id >= superblock->next_page_id
                || !sdb_wal_id_set_insert(
                    page_ids, id_capacity, pages[index].page_id
                )
                || sdb_page_decode(
                    pages[index].bytes,
                    (size_t)superblock->page_size,
                    pages[index].page_id,
                    &view
                ) != SDB_OK
                || view.page_lsn != txn_id) {
                free(page_ids);
                return SDB_E_INVALID_ARGUMENT;
            }
        }
        free(page_ids);
    }
    buffer = (uint8_t *)calloc(total_size, 1U);
    if (buffer == NULL) {
        return SDB_E_INTERNAL;
    }
    (void)memcpy(buffer, sdb_wal_magic, sizeof(sdb_wal_magic));
    sdb_write_u16_le(buffer + 4U, SDB_WAL_VERSION);
    sdb_write_u16_le(buffer + 6U, (uint16_t)SDB_WAL_HEADER_SIZE);
    sdb_write_u64_le(buffer + 8U, txn_id);
    sdb_write_u32_le(buffer + 16U, superblock->page_size);
    sdb_write_u32_le(buffer + 20U, (uint32_t)page_count);
    (void)memcpy(buffer + 24U, superblock->file_id, SDB_FILE_ID_SIZE);
    sdb_write_u64_le(buffer + 40U, superblock->next_page_id);
    sdb_write_u64_le(buffer + 48U, superblock->freelist_page);
    checksum = sdb_crc32_zeroed_range(
        buffer,
        SDB_WAL_HEADER_SIZE,
        SDB_WAL_HEADER_CHECKSUM_OFFSET,
        sizeof(uint32_t)
    );
    sdb_write_u32_le(buffer + SDB_WAL_HEADER_CHECKSUM_OFFSET, checksum);
    cursor = buffer + SDB_WAL_HEADER_SIZE;
    for (index = 0U; index < page_count; ++index) {
        sdb_write_u64_le(cursor, pages[index].page_id);
        (void)memcpy(
            cursor + SDB_WAL_RECORD_HEADER_SIZE,
            pages[index].bytes,
            (size_t)superblock->page_size
        );
        cursor += SDB_WAL_RECORD_HEADER_SIZE + (size_t)superblock->page_size;
    }
    data_size = (size_t)(cursor - buffer);
    (void)memcpy(cursor, sdb_wal_commit_magic, sizeof(sdb_wal_commit_magic));
    sdb_write_u16_le(cursor + 4U, SDB_WAL_VERSION);
    sdb_write_u16_le(cursor + 6U, (uint16_t)SDB_WAL_COMMIT_SIZE);
    sdb_write_u64_le(cursor + 8U, txn_id);
    sdb_write_u32_le(cursor + 16U, sdb_crc32(buffer, data_size));
    checksum = sdb_crc32_zeroed_range(
        cursor,
        SDB_WAL_COMMIT_SIZE,
        SDB_WAL_COMMIT_CHECKSUM_OFFSET,
        sizeof(uint32_t)
    );
    sdb_write_u32_le(cursor + SDB_WAL_COMMIT_CHECKSUM_OFFSET, checksum);

    status = sdb_wal_open_reset(wal_path, &file);
    if (status == SDB_OK) {
        opened = true;
#if SDB_TESTING
        sdb_file_fail_after_for_testing(&file, sdb_wal_test_fail_after);
#endif
        /*
         * The commit trailer is written only after the body is durable.
         * A torn/missing trailer is therefore always an uncommitted WAL.
         */
        status = sdb_file_write_full(&file, 0U, buffer, data_size);
    }
    if (status == SDB_OK) {
        status = sdb_file_sync(&file);
    }
    if (status == SDB_OK) {
        status = sdb_file_write_full(
            &file, (uint64_t)data_size, cursor, SDB_WAL_COMMIT_SIZE
        );
    }
    if (status == SDB_OK) {
        status = sdb_file_sync(&file);
    }
    if (opened) {
        close_status = sdb_file_close(&file);
    }
    free(buffer);
    return status != SDB_OK ? status : close_status;
}

static sdb_status sdb_wal_apply(
    const uint8_t *buffer,
    size_t page_size,
    size_t page_count,
    uint64_t txn_id,
    uint64_t next_page_id,
    sdb_file *database
)
{
    const uint64_t data_offset =
        (uint64_t)(SDB_SUPERBLOCK_SLOT_SIZE * SDB_SUPERBLOCK_SLOT_COUNT);
    const uint8_t *cursor = buffer + SDB_WAL_HEADER_SIZE;
    size_t index;
    for (index = 0U; index < page_count; ++index) {
        uint64_t page_id = sdb_read_u64_le(cursor);
        uint64_t relative;
        uint64_t offset;
        sdb_page_view view;
        sdb_status status;
        if (page_id == 0U || page_id >= next_page_id
            || page_id - 1U > UINT64_MAX / (uint64_t)page_size) {
            return SDB_E_CORRUPT;
        }
        relative = (page_id - 1U) * (uint64_t)page_size;
        if (relative > UINT64_MAX - data_offset) {
            return SDB_E_CORRUPT;
        }
        offset = data_offset + relative;
        status = sdb_page_decode(
            cursor + SDB_WAL_RECORD_HEADER_SIZE,
            page_size,
            page_id,
            &view
        );
        if (status != SDB_OK || view.page_lsn != txn_id) {
            return SDB_E_CORRUPT;
        }
        status = sdb_file_write_full(
            database,
            offset,
            cursor + SDB_WAL_RECORD_HEADER_SIZE,
            page_size
        );
        if (status != SDB_OK) {
            return status;
        }
        cursor += SDB_WAL_RECORD_HEADER_SIZE + page_size;
    }
    return sdb_file_sync(database);
}

static sdb_status sdb_wal_validate_records(
    const uint8_t *buffer,
    size_t page_size,
    size_t page_count,
    uint64_t txn_id,
    uint64_t next_page_id,
    const sdb_superblock_v1 *superblock,
    const uint8_t *data_key
)
{
    const uint8_t *cursor = buffer + SDB_WAL_HEADER_SIZE;
    uint64_t *page_ids;
    uint8_t *decrypt_scratch = NULL;
    size_t id_capacity;
    size_t index;
    sdb_status status = sdb_wal_id_set_create(
        page_count, &page_ids, &id_capacity
    );
    if (status != SDB_OK) {
        return status;
    }
    /*
     * Under encryption, each record's page image is an SEN1 envelope.
     * Before we let recover install it into the DB file, verify the
     * Poly1305 tag against the current DB's data_key. Without this
     * pass, an attacker who can write only to <db>.wal (WORM DB +
     * writable sidecar) could craft a CRC-valid WAL whose page
     * bodies are ciphertext under a foreign key: recover would
     * write those pages, and any subsequent AEAD failure at read
     * time would be surfaced only lazily. The tag verification here
     * makes replay itself refuse the transaction.
     *
     * The scratch buffer is a single page-size allocation reused
     * across the loop; the decrypted plaintext is thrown away
     * because we only care whether the tag verifies.
     */
    if (data_key != NULL) {
        decrypt_scratch = (uint8_t *)malloc(page_size);
        if (decrypt_scratch == NULL) {
            free(page_ids);
            return SDB_E_INTERNAL;
        }
    }
    for (index = 0U; index < page_count; ++index) {
        const uint64_t page_id = sdb_read_u64_le(cursor);
        sdb_page_view view;
        if (page_id == 0U || page_id >= next_page_id
            || !sdb_wal_id_set_insert(page_ids, id_capacity, page_id)
            || sdb_page_decode(
                cursor + SDB_WAL_RECORD_HEADER_SIZE,
                page_size,
                page_id,
                &view
            ) != SDB_OK
            || view.page_lsn != txn_id) {
            free(decrypt_scratch);
            free(page_ids);
            return SDB_E_CORRUPT;
        }
        if (data_key != NULL && view.type == (uint16_t)SDB_PAGE_TYPE_ENCRYPTED) {
            uint16_t plaintext_type_out;
            size_t plaintext_size_out;
            const sdb_status decrypt_status =
                sdb_encrypted_page_decrypt(
                    superblock, data_key, &view,
                    decrypt_scratch, page_size,
                    &plaintext_type_out, &plaintext_size_out
                );
            if (decrypt_status != SDB_OK) {
                sdb_secure_zero(decrypt_scratch, page_size);
                free(decrypt_scratch);
                free(page_ids);
                return SDB_E_CORRUPT;
            }
        }
        cursor += SDB_WAL_RECORD_HEADER_SIZE + page_size;
    }
    if (decrypt_scratch != NULL) {
        sdb_secure_zero(decrypt_scratch, page_size);
        free(decrypt_scratch);
    }
    free(page_ids);
    return SDB_OK;
}

sdb_status sdb_wal_recover(
    const char *wal_path,
    sdb_file *database,
    const sdb_superblock_v1 *superblock,
    const uint8_t *data_key,
    uint64_t *txn_id_out,
    uint64_t *next_page_id_out,
    uint64_t *freelist_page_out,
    bool *replayed_out
)
{
    sdb_file file;
    uint64_t file_size_u64;
    uint8_t header[SDB_WAL_HEADER_SIZE];
    uint8_t *buffer;
    size_t file_size;
    size_t expected_size;
    size_t data_size;
    size_t page_count;
    uint64_t txn_id;
    uint64_t target_next_page_id;
    uint64_t target_freelist_page;
    uint16_t wal_version;
    const uint8_t *commit;
    uint32_t checksum;
    sdb_status status;

    if (wal_path == NULL || database == NULL || superblock == NULL
        || txn_id_out == NULL || next_page_id_out == NULL
        || freelist_page_out == NULL || replayed_out == NULL) {
        return SDB_E_INVALID_ARGUMENT;
    }
    *txn_id_out = 0U;
    *next_page_id_out = superblock->next_page_id;
    *freelist_page_out = superblock->freelist_page;
    *replayed_out = false;
    status = sdb_file_open_existing(wal_path, false, &file);
    if (status != SDB_OK) {
        /*
         * A missing WAL is the healthy "no crash recovery needed" case
         * and we return SDB_OK with replayed_out=false. But ANY other
         * open failure — permission, too-many-open-files, transient
         * I/O — used to collapse to the same SDB_OK, silently dropping
         * any transactions whose commit records had been fsynced. Only
         * the not-found status short-circuits to success; every other
         * error propagates so the caller can decide whether to abort
         * the open or retry.
         */
        return status == SDB_E_NOT_FOUND ? SDB_OK : status;
    }
    status = sdb_file_size(&file, &file_size_u64);
    if (status != SDB_OK || file_size_u64 == 0U) {
        (void)sdb_file_close(&file);
        return status;
    }
    if (file_size_u64 > SDB_WAL_MAX_BYTES) {
        (void)sdb_file_close(&file);
        return SDB_E_CORRUPT;
    }
    if (file_size_u64 > (uint64_t)SIZE_MAX) {
        (void)sdb_file_close(&file);
        return SDB_E_OVERFLOW;
    }
    file_size = (size_t)file_size_u64;
    if (file_size < SDB_WAL_HEADER_SIZE + SDB_WAL_COMMIT_SIZE) {
        /*
         * A structurally-incomplete WAL is safely ignored, but leaving it on
         * disk lets repeated crash-during-create cycles accumulate stale
         * files (A1.3). Best-effort remove; errors are non-fatal to recovery.
         */
        (void)sdb_file_close(&file);
        (void)sdb_file_remove(wal_path, true);
        (void)sdb_file_sync_parent_directory(wal_path);
        return SDB_OK;
    }
    status = sdb_file_read_full(
        &file, 0U, header, sizeof(header)
    );
    if (status != SDB_OK) {
        (void)sdb_file_close(&file);
        return status;
    }
    if (memcmp(header, sdb_wal_magic, sizeof(sdb_wal_magic)) != 0) {
        (void)sdb_file_close(&file);
        return SDB_OK;
    }
    wal_version = sdb_read_u16_le(header + 4U);
    /*
     * Two separate WAL-header failure modes have different semantics:
     *
     * 1. Wrong file_id or wrong page_size — this WAL was written for
     *    a DIFFERENT database. The only realistic way to reach this
     *    state on a valid installation is a crash inside
     *    sdb_replace_finish, between the unlink(.replace) that
     *    completes the swap and the unlink(.wal) that clears the old
     *    file's WAL. After such a crash, reopen sees the new file's
     *    file_id in the superblock but the old file's file_id in the
     *    orphaned .wal — a mismatch that a fatal SDB_E_CORRUPT would
     *    turn into a permanent brick. Treat it the way we already
     *    treat a structurally-incomplete WAL: best-effort unlink,
     *    directory fsync, and return SDB_OK. There is no replay to
     *    do — the transactions in that WAL belonged to a file that
     *    no longer exists at this path.
     *
     * 2. Wrong version, wrong header size, or bad CRC — this WAL is
     *    structurally corrupt. That IS an integrity failure worth
     *    surfacing; keep the SDB_E_CORRUPT.
     */
    if (sdb_read_u32_le(header + 16U) != superblock->page_size
        || memcmp(
            header + 24U, superblock->file_id, SDB_FILE_ID_SIZE
        ) != 0) {
        (void)sdb_file_close(&file);
        (void)sdb_file_remove(wal_path, true);
        (void)sdb_file_sync_parent_directory(wal_path);
        return SDB_OK;
    }
    if ((wal_version != SDB_WAL_VERSION_V1
            && wal_version != SDB_WAL_VERSION)
        || sdb_read_u16_le(header + 6U)
            != (uint16_t)SDB_WAL_HEADER_SIZE
        || sdb_crc32_zeroed_range(
            header,
            SDB_WAL_HEADER_SIZE,
            SDB_WAL_HEADER_CHECKSUM_OFFSET,
            sizeof(uint32_t)
        ) != sdb_read_u32_le(
            header + SDB_WAL_HEADER_CHECKSUM_OFFSET
        )) {
        (void)sdb_file_close(&file);
        return SDB_E_CORRUPT;
    }
    page_count = (size_t)sdb_read_u32_le(header + 20U);
    if (page_count == 0U || page_count > (size_t)SDB_WAL_MAX_RECORDS
        || sdb_wal_expected_size(
            (size_t)superblock->page_size, page_count, &expected_size
        ) != SDB_OK
        || expected_size != file_size) {
        (void)sdb_file_close(&file);
        return SDB_OK;
    }
    buffer = (uint8_t *)malloc(file_size);
    if (buffer == NULL) {
        (void)sdb_file_close(&file);
        return SDB_E_INTERNAL;
    }
    status = sdb_file_read_full(&file, 0U, buffer, file_size);
    (void)sdb_file_close(&file);
    if (status != SDB_OK) {
        free(buffer);
        return status;
    }
    /*
     * A short or structurally incomplete WAL has no valid commit marker and
     * is safely ignored. A full-sized but invalid committed WAL is corruption.
     */
    if (file_size < SDB_WAL_HEADER_SIZE + SDB_WAL_COMMIT_SIZE
        || memcmp(buffer, sdb_wal_magic, sizeof(sdb_wal_magic)) != 0) {
        free(buffer);
        return SDB_OK;
    }
    wal_version = sdb_read_u16_le(buffer + 4U);
    if ((wal_version != SDB_WAL_VERSION_V1
            && wal_version != SDB_WAL_VERSION)
        || sdb_read_u16_le(buffer + 6U) != (uint16_t)SDB_WAL_HEADER_SIZE
        || sdb_read_u32_le(buffer + 16U) != superblock->page_size
        || memcmp(buffer + 24U, superblock->file_id, SDB_FILE_ID_SIZE) != 0
        || sdb_crc32_zeroed_range(
            buffer,
            SDB_WAL_HEADER_SIZE,
            SDB_WAL_HEADER_CHECKSUM_OFFSET,
            sizeof(uint32_t)
        ) != sdb_read_u32_le(buffer + SDB_WAL_HEADER_CHECKSUM_OFFSET)) {
        free(buffer);
        return SDB_E_CORRUPT;
    }
    if (wal_version == SDB_WAL_VERSION_V1) {
        size_t reserved_index;
        for (reserved_index = 40U;
             reserved_index < SDB_WAL_HEADER_CHECKSUM_OFFSET;
             ++reserved_index) {
            if (buffer[reserved_index] != 0U) {
                free(buffer);
                return SDB_E_CORRUPT;
            }
        }
        target_next_page_id = superblock->next_page_id;
        target_freelist_page = superblock->freelist_page;
    } else {
        size_t reserved_index;
        target_next_page_id = sdb_read_u64_le(buffer + 40U);
        target_freelist_page = sdb_read_u64_le(buffer + 48U);
        for (reserved_index = 56U;
             reserved_index < SDB_WAL_HEADER_CHECKSUM_OFFSET;
             ++reserved_index) {
            if (buffer[reserved_index] != 0U) {
                free(buffer);
                return SDB_E_CORRUPT;
            }
        }
        if (target_next_page_id < superblock->next_page_id
            || target_freelist_page >= target_next_page_id) {
            free(buffer);
            return SDB_E_CORRUPT;
        }
    }
    page_count = (size_t)sdb_read_u32_le(buffer + 20U);
    txn_id = sdb_read_u64_le(buffer + 8U);
    if (page_count == 0U || page_count > (size_t)SDB_WAL_MAX_RECORDS
        || txn_id == 0U
        || sdb_wal_expected_size(
            (size_t)superblock->page_size, page_count, &expected_size
        ) != SDB_OK
        || expected_size != file_size) {
        free(buffer);
        return SDB_OK;
    }
    data_size = file_size - SDB_WAL_COMMIT_SIZE;
    commit = buffer + data_size;
    checksum = sdb_crc32_zeroed_range(
        commit,
        SDB_WAL_COMMIT_SIZE,
        SDB_WAL_COMMIT_CHECKSUM_OFFSET,
        sizeof(uint32_t)
    );
    if (memcmp(commit, sdb_wal_commit_magic, sizeof(sdb_wal_commit_magic)) != 0
        || sdb_read_u16_le(commit + 4U) != wal_version
        || sdb_read_u16_le(commit + 6U) != (uint16_t)SDB_WAL_COMMIT_SIZE
        || sdb_read_u64_le(commit + 8U) != txn_id
        || sdb_read_u32_le(commit + 16U) != sdb_crc32(buffer, data_size)
        || sdb_read_u32_le(commit + 20U) != checksum) {
        free(buffer);
        return SDB_OK;
    }
    status = sdb_wal_validate_records(
        buffer,
        (size_t)superblock->page_size,
        page_count,
        txn_id,
        target_next_page_id,
        superblock,
        data_key
    );
    if (status != SDB_OK) {
        free(buffer);
        return status;
    }
    if (txn_id > superblock->checkpoint_lsn) {
        status = sdb_wal_apply(
            buffer,
            (size_t)superblock->page_size,
            page_count,
            txn_id,
            target_next_page_id,
            database
        );
        if (status != SDB_OK) {
            free(buffer);
            return status;
        }
        *replayed_out = true;
    }
    *txn_id_out = txn_id;
    *next_page_id_out = target_next_page_id;
    *freelist_page_out = target_freelist_page;
    free(buffer);
    return SDB_OK;
}

sdb_status sdb_wal_clear(const char *wal_path)
{
    sdb_file file;
    sdb_status status;
    sdb_status close_status;
    if (wal_path == NULL) {
        return SDB_E_INVALID_ARGUMENT;
    }
    status = sdb_file_open_existing(wal_path, true, &file);
    if (status != SDB_OK) {
        /*
         * Mirror sdb_wal_recover: only a missing WAL is the healthy
         * no-op; every other open failure propagates so a stale WAL
         * that we cannot clear does not silently look cleared.
         */
        return status == SDB_E_NOT_FOUND ? SDB_OK : status;
    }
    status = sdb_file_resize(&file, 0U);
    if (status == SDB_OK) {
        status = sdb_file_sync(&file);
    }
    close_status = sdb_file_close(&file);
    if (status == SDB_OK && close_status == SDB_OK) {
        /*
         * Persist the size change through the directory entry so a crash
         * cannot leave an old body behind on filesystems where truncate
         * durability depends on a parent-directory fsync (A1.7).
         */
        status = sdb_file_sync_parent_directory(wal_path);
    }
    return status != SDB_OK ? status : close_status;
}

#if SDB_TESTING
void sdb_wal_fail_after_for_testing(size_t successful_operations)
{
    sdb_wal_test_fail_after = successful_operations;
}

void sdb_wal_clear_failure_for_testing(void)
{
    sdb_wal_test_fail_after = SIZE_MAX;
}
#endif
