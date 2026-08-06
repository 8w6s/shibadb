#include "wal.h"

#include "crypto.h"
#include "encrypted_page.h"
#include "internal.h"

#include <stdlib.h>
#include <string.h>

#define SDB_WAL_VERSION_V1 UINT16_C(1)
#define SDB_WAL_VERSION UINT16_C(2)
/*
 * Legacy (v1/v2, pre-WAL-mode) commit-record size. The v3 multi-txn WAL
 * grew SDB_WAL_COMMIT_SIZE from 24 to 44 to carry frame_count/next_page_id/
 * freelist/running-crc. The single-record legacy reader (sdb_wal_recover)
 * and its writer (sdb_wal_write_committed) must keep using the historical
 * 24-byte layout, otherwise a WAL actually written by the old binary
 * (24-byte commit) fails the expected-size gate and is silently discarded,
 * losing a crash-committed pre-upgrade txn. v3 code paths keep using
 * SDB_WAL_COMMIT_SIZE.
 */
#define SDB_WAL_LEGACY_COMMIT_SIZE ((size_t)24)
#define SDB_WAL_HEADER_CHECKSUM_OFFSET ((size_t)60)
#define SDB_WAL_COMMIT_CHECKSUM_OFFSET ((size_t)20)
#define SDB_WAL_V3_COMMIT_FRAME_COUNT_OFFSET ((size_t)16)
#define SDB_WAL_V3_COMMIT_NEXT_PAGE_ID_OFFSET ((size_t)20)
#define SDB_WAL_V3_COMMIT_FREELIST_PAGE_OFFSET ((size_t)28)
#define SDB_WAL_V3_COMMIT_RUNNING_CRC_OFFSET ((size_t)36)
#define SDB_WAL_V3_COMMIT_CHECKSUM_OFFSET ((size_t)40)

_Static_assert(
    SDB_WAL_COMMIT_SIZE
        >= SDB_WAL_V3_COMMIT_CHECKSUM_OFFSET + sizeof(uint32_t),
    "SDB_WAL_COMMIT_SIZE must hold the v3 commit record"
);

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
        return SDB_E_OUT_OF_MEMORY;
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
            without_commit, SDB_WAL_LEGACY_COMMIT_SIZE, size_out
        )) {
        return SDB_E_OVERFLOW;
    }
    return SDB_OK;
}

static sdb_status sdb_wal_open_reset(const char *path, sdb_file *file_out)
{
    sdb_status status = sdb_file_open_existing(path, true, file_out);
    bool created = false;
    bool opened = status == SDB_OK;
    if (status != SDB_OK) {
        status = sdb_file_create_new(path, file_out);
        created = status == SDB_OK;
        opened = created;
    }
    if (status == SDB_OK) {
        status = sdb_file_resize(file_out, 0U);
    }
    if (status == SDB_OK && created) {
        status = sdb_file_sync_parent_directory(path);
    }
    if (status != SDB_OK && opened) {
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
        return SDB_E_OUT_OF_MEMORY;
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
    sdb_write_u16_le(cursor + 6U, (uint16_t)SDB_WAL_LEGACY_COMMIT_SIZE);
    sdb_write_u64_le(cursor + 8U, txn_id);
    sdb_write_u32_le(cursor + 16U, sdb_crc32(buffer, data_size));
    checksum = sdb_crc32_zeroed_range(
        cursor,
        SDB_WAL_LEGACY_COMMIT_SIZE,
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

        status = sdb_file_write_full(&file, 0U, buffer, data_size);
    }
    if (status == SDB_OK) {
        status = sdb_file_sync(&file);
    }
    if (status == SDB_OK) {
        status = sdb_file_write_full(
            &file, (uint64_t)data_size, cursor, SDB_WAL_LEGACY_COMMIT_SIZE
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

/*
 * R4 group commit: build the [header?][frames][commit-record] staging buffer
 * for one txn WITHOUT touching the disk. Identical byte layout to the write
 * that sdb_wal_append_txn used to do inline, but the pwrite + fsync are
 * deferred so a leader can coalesce many txns' buffers into ONE fsync. On
 * success *buffer_out is a heap buffer the caller must free, *total_size_out
 * its length, *write_offset_out where it must land in the WAL (0 for the
 * header-carrying first append, else append_offset), and *new_offset_out the
 * post-append wal_tail. Pure function: no I/O, no testing hooks — the leader
 * owns durability. append_offset is validated exactly as the inline path did.
 */
sdb_status sdb_wal_encode_txn(
    const sdb_superblock_v1 *superblock,
    uint64_t append_offset,
    uint64_t txn_id,
    const sdb_wal_page *pages,
    size_t page_count,
    uint8_t **buffer_out,
    size_t *total_size_out,
    uint64_t *write_offset_out,
    uint64_t *new_offset_out
)
{
    uint8_t *buffer;
    sdb_wal_commit_rec rec;
    size_t frame_size;
    size_t frames_size;
    size_t header_prefix;
    size_t total_size;
    uint8_t *frames_ptr;
    uint64_t write_offset;
    size_t index;
    uint32_t running_crc;
    sdb_status status;

    if (superblock == NULL || pages == NULL || buffer_out == NULL
        || total_size_out == NULL || write_offset_out == NULL
        || new_offset_out == NULL || txn_id == 0U || page_count == 0U
        || page_count > (size_t)SDB_WAL_MAX_RECORDS
        || page_count > (size_t)UINT32_MAX) {
        return SDB_E_INVALID_ARGUMENT;
    }
    *buffer_out = NULL;
    frame_size = sdb_wal_frame_size_v5((size_t)superblock->page_size);
    if (!sdb_checked_mul_size(frame_size, page_count, &frames_size)) {
        return SDB_E_OVERFLOW;
    }
    if (append_offset > SDB_WAL_MAX_BYTES
        || (uint64_t)frames_size > SDB_WAL_MAX_BYTES - append_offset
        || (uint64_t)SDB_WAL_COMMIT_SIZE
            > SDB_WAL_MAX_BYTES - append_offset - (uint64_t)frames_size) {
        return SDB_E_OVERFLOW;
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
    /*
     * The very first txn into a fresh (or freshly cleared) WAL stamps the
     * 64-byte identity header into the [0, SDB_WAL_HEADER_SIZE) hole so that
     * sdb_wal_recover_all can refuse a foreign WAL (different file_id /
     * page_size) before replaying a single frame. Under R2a the header, the
     * frames, and the commit-record all share the SAME buffer + single fsync,
     * so a crash mid-write leaves a torn tail that recovery discards whole
     * (the running-CRC over the frames must match the commit-record), never a
     * lost committed txn. Subsequent appends (offset past the header) leave
     * the header untouched.
     */
    header_prefix = (append_offset == (uint64_t)SDB_WAL_HEADER_SIZE)
        ? SDB_WAL_HEADER_SIZE
        : (size_t)0U;
    /*
     * R2a single-fsync commit: the header (first append only), every frame,
     * AND the commit-record now share ONE staging buffer, written by one
     * contiguous pwrite and sealed by one fsync. Reserve room for all three
     * here so the commit-record can be laid down at buffer[header_prefix +
     * frames_size] instead of a separate write.
     */
    if (!sdb_checked_add_size(header_prefix, frames_size, &total_size)
        || !sdb_checked_add_size(
            total_size, SDB_WAL_COMMIT_SIZE, &total_size
        )) {
        return SDB_E_OVERFLOW;
    }
    buffer = (uint8_t *)calloc(total_size, 1U);
    if (buffer == NULL) {
        return SDB_E_OUT_OF_MEMORY;
    }
    frames_ptr = buffer + header_prefix;
    {
        uint8_t *cursor = frames_ptr;
        for (index = 0U; index < page_count; ++index) {
            /* v5 self-describing, database-bound frame header (40 bytes). */
            sdb_write_u64_le(cursor, pages[index].page_id);
            sdb_write_u64_le(
                cursor + SDB_WAL_FRAME_V4_TXN_ID_OFFSET, txn_id
            );
            sdb_write_u32_le(
                cursor + SDB_WAL_FRAME_V4_FRAME_INDEX_OFFSET,
                (uint32_t)index
            );
            sdb_write_u32_le(
                cursor + SDB_WAL_FRAME_V4_FRAME_COUNT_OFFSET,
                (uint32_t)page_count
            );
            (void)memcpy(
                cursor + SDB_WAL_FRAME_V5_FILE_ID_OFFSET,
                superblock->file_id,
                SDB_FILE_ID_SIZE
            );
            (void)memcpy(
                cursor + SDB_WAL_RECORD_HEADER_SIZE_V5,
                pages[index].bytes,
                (size_t)superblock->page_size
            );
            cursor += frame_size;
        }
    }
    if (header_prefix != 0U) {
        (void)memcpy(buffer, sdb_wal_magic, sizeof(sdb_wal_magic));
        sdb_write_u16_le(buffer + 4U, SDB_WAL_VERSION_V5);
        sdb_write_u16_le(buffer + 6U, (uint16_t)SDB_WAL_HEADER_SIZE);
        sdb_write_u64_le(buffer + 8U, txn_id);
        sdb_write_u32_le(buffer + 16U, superblock->page_size);
        sdb_write_u32_le(buffer + 20U, (uint32_t)page_count);
        (void)memcpy(buffer + 24U, superblock->file_id, SDB_FILE_ID_SIZE);
        sdb_write_u64_le(buffer + 40U, superblock->next_page_id);
        sdb_write_u64_le(buffer + 48U, superblock->freelist_page);
        sdb_write_u32_le(
            buffer + SDB_WAL_HEADER_CHECKSUM_OFFSET,
            sdb_crc32_zeroed_range(
                buffer,
                SDB_WAL_HEADER_SIZE,
                SDB_WAL_HEADER_CHECKSUM_OFFSET,
                sizeof(uint32_t)
            )
        );
    }
    running_crc = sdb_crc32(frames_ptr, frames_size);
    rec.txn_id = txn_id;
    rec.frame_count = (uint32_t)page_count;
    rec.next_page_id = superblock->next_page_id;
    rec.freelist_page = superblock->freelist_page;
    sdb_wal_encode_commit_rec(
        buffer + header_prefix + frames_size, &rec, running_crc
    );

    write_offset = (header_prefix != 0U) ? 0U : append_offset;
    *buffer_out = buffer;
    *total_size_out = total_size;
    *write_offset_out = write_offset;
    *new_offset_out = append_offset + (uint64_t)frames_size
        + (uint64_t)SDB_WAL_COMMIT_SIZE;
    return SDB_OK;
}

sdb_status sdb_wal_append_txn(
    sdb_file *wal,
    uint64_t append_offset,
    const sdb_superblock_v1 *superblock,
    uint64_t txn_id,
    const sdb_wal_page *pages,
    size_t page_count,
    uint64_t *new_offset_out
)
{
    uint8_t *buffer = NULL;
    size_t total_size = 0U;
    uint64_t write_offset = 0U;
    uint64_t local_new_offset = 0U;
    sdb_status status;

    if (wal == NULL || new_offset_out == NULL) {
        return SDB_E_INVALID_ARGUMENT;
    }
    /*
     * Contract (guards the class of bug where a caller trusts an advanced WAL
     * tail after a failed append): encode into a LOCAL offset and only publish
     * *new_offset_out once the write AND fsync have both succeeded. A failed
     * pwrite/fsync leaves *new_offset_out untouched, so the caller never
     * advances its wal_tail past bytes that are not durably on disk.
     */
    status = sdb_wal_encode_txn(
        superblock, append_offset, txn_id, pages, page_count,
        &buffer, &total_size, &write_offset, &local_new_offset
    );
    if (status != SDB_OK) {
        return status;
    }
#if SDB_TESTING
    /*
     * Re-prime the fd's injected-failure counter to the current global
     * boundary on EVERY append. With the persistent WAL fd (R2a) the same
     * sdb_file is reused across commits, so — unlike the old open-per-commit
     * path, where each commit got a fresh fd with a zeroed counter — the
     * boundary must be reset here or a failure armed for one commit would
     * bleed into the next. SIZE_MAX (the disarmed value) simply clears it.
     */
    sdb_file_fail_after_for_testing(wal, sdb_wal_test_fail_after);
#endif
    /*
     * ONE durability barrier (R2a). The [header?][frames][commit-record]
     * staging buffer is flushed by a single pwrite + single fsync, replacing
     * the former frames-fsync-then-commit-fsync pair. This is safe — not a
     * weakening — because of the R1 running-CRC: recovery recomputes the CRC
     * over the frames before it will honour the commit-record, so a crash at
     * any point in this write (torn frames, an absent commit-record, or a
     * commit-record whose frames are short/torn) fails the CRC-or-decode gate
     * and the whole txn is dropped. The commit-record is NEVER validated in
     * isolation from its frames; fsync remains the ordering barrier (nothing
     * before it is durable, everything after it is), so crash-before-fsync
     * drops an uncommitted txn and crash-after-fsync keeps a committed one.
     */
    status = sdb_file_write_full(
        wal, write_offset, buffer, total_size
    );
    if (status == SDB_OK) {
        status = sdb_file_sync(wal);
    }
#if SDB_TESTING
    /*
     * Scope the injected failure to THIS append. With the persistent WAL fd
     * (R2a) the same handle is later reused by the checkpoint truncate and by
     * subsequent commits, so leaving the boundary armed on the fd would fault
     * an unrelated later op (e.g. sdb_pager_wal_reset's resize). The pre-R2a
     * open-per-commit fd was discarded right here, giving the same one-append
     * scope implicitly; re-arm is done fresh at the top of the next append.
     */
    sdb_file_clear_failure_for_testing(wal);
#endif
    free(buffer);
    if (status == SDB_OK) {
        *new_offset_out = local_new_offset;
    }
    return status;
}

static sdb_status sdb_wal_apply_frames(
    const uint8_t *frames,
    size_t page_size,
    size_t page_count,
    uint64_t txn_id,
    uint64_t next_page_id,
    size_t rec_hdr_size,
    sdb_file *database,
    bool sync_after
)
{
    const uint64_t data_offset =
        (uint64_t)(SDB_SUPERBLOCK_SLOT_SIZE * SDB_SUPERBLOCK_SLOT_COUNT);
    const uint8_t *cursor = frames;
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
            cursor + rec_hdr_size,
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
            cursor + rec_hdr_size,
            page_size
        );
        if (status != SDB_OK) {
            return status;
        }
        cursor += rec_hdr_size + page_size;
    }
    return sync_after ? sdb_file_sync(database) : SDB_OK;
}

static sdb_status sdb_wal_validate_frames(
    const uint8_t *frames,
    size_t page_size,
    size_t page_count,
    uint64_t txn_id,
    uint64_t next_page_id,
    size_t rec_hdr_size,
    bool v4_meta,
    const sdb_superblock_v1 *superblock,
    const uint8_t *data_key
)
{
    const uint8_t *cursor = frames;
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

    if (data_key != NULL) {
        decrypt_scratch = (uint8_t *)malloc(page_size);
        if (decrypt_scratch == NULL) {
            free(page_ids);
            return SDB_E_OUT_OF_MEMORY;
        }
    }
    for (index = 0U; index < page_count; ++index) {
        const uint64_t page_id = sdb_read_u64_le(cursor);
        sdb_page_view view;
        /*
         * v4 frames are self-describing: every frame must name the same txn,
         * carry its own contiguous 0-based index and the shared frame_count.
         * This makes the txn boundary deterministic — a page-data byte that
         * happens to spell 'SCMT' can never be read as a commit-record.
         */
        if (v4_meta
            && (sdb_read_u64_le(cursor + SDB_WAL_FRAME_V4_TXN_ID_OFFSET)
                    != txn_id
                || sdb_read_u32_le(
                       cursor + SDB_WAL_FRAME_V4_FRAME_INDEX_OFFSET
                   ) != (uint32_t)index
                || sdb_read_u32_le(
                       cursor + SDB_WAL_FRAME_V4_FRAME_COUNT_OFFSET
                   ) != (uint32_t)page_count)) {
            free(decrypt_scratch);
            free(page_ids);
            return SDB_E_CORRUPT;
        }
        if (rec_hdr_size == SDB_WAL_RECORD_HEADER_SIZE_V5
            && memcmp(
                cursor + SDB_WAL_FRAME_V5_FILE_ID_OFFSET,
                superblock->file_id,
                SDB_FILE_ID_SIZE
            ) != 0) {
            free(decrypt_scratch);
            free(page_ids);
            return SDB_E_CORRUPT;
        }
        if (page_id == 0U || page_id >= next_page_id
            || !sdb_wal_id_set_insert(page_ids, id_capacity, page_id)
            || sdb_page_decode(
                cursor + rec_hdr_size,
                page_size,
                page_id,
                &view
            ) != SDB_OK
            || view.page_lsn != txn_id) {
            free(decrypt_scratch);
            free(page_ids);
            return SDB_E_CORRUPT;
        }
        if (data_key != NULL) {
            uint16_t plaintext_type_out;
            size_t plaintext_size_out;
            sdb_status decrypt_status;
            /*
             * In an encrypted database every legitimate WAL frame is an
             * ENCRYPTED envelope: the commit path always routes pages through
             * sdb_encrypted_page_encode, which stamps SDB_PAGE_TYPE_ENCRYPTED
             * on the outer page regardless of the plaintext page type. A frame
             * that declares any other outer type is therefore a keyless forgery
             * — every gate it passed (range, uniqueness, CRC32, page_lsn) is
             * public — so it MUST be refused at the recovery/parse boundary,
             * before sdb_wal_apply_frames writes it verbatim into the data file.
             * This mirrors the read-path guard (an encrypted DB decodes only
             * ENCRYPTED pages) and closes the WAL-forgery arm in
             * docs/NONCE_MODEL.md: without it an attacker who can write only the
             * .wal sidecar overwrites a committed page with attacker bytes,
             * which the read path then rejects as corrupt — permanent data loss
             * / DoS with no key.
             */
            if (view.type != (uint16_t)SDB_PAGE_TYPE_ENCRYPTED) {
                sdb_secure_zero(decrypt_scratch, page_size);
                free(decrypt_scratch);
                free(page_ids);
                return SDB_E_CORRUPT;
            }
            decrypt_status = sdb_encrypted_page_decrypt(
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
        cursor += rec_hdr_size + page_size;
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
    if (file_size < SDB_WAL_HEADER_SIZE + SDB_WAL_LEGACY_COMMIT_SIZE) {

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
    if (wal_version == SDB_WAL_VERSION_V3
        || wal_version == SDB_WAL_VERSION_V4
        || wal_version == SDB_WAL_VERSION_V5) {
        /*
         * A v3/v4 multi-txn WAL (identity header stamped by sdb_wal_append_txn)
         * is owned by sdb_wal_recover_all, which has already run before this
         * legacy fallback. The legacy single-record reader must treat such a
         * header as "not a legacy WAL" and no-op — never as a corrupt legacy
         * WAL. Otherwise a multi-txn WAL that recover_all legitimately left
         * unreplayed (e.g. a torn / uncommitted first txn whose commit-record
         * never made it to disk) would wrongly fail the open. A foreign
         * multi-txn WAL is already rejected by recover_all's identity gate, so
         * it never reaches here.
         */
        (void)sdb_file_close(&file);
        return SDB_OK;
    }

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
        return SDB_E_OUT_OF_MEMORY;
    }
    status = sdb_file_read_full(&file, 0U, buffer, file_size);
    (void)sdb_file_close(&file);
    if (status != SDB_OK) {
        free(buffer);
        return status;
    }

    if (file_size < SDB_WAL_HEADER_SIZE + SDB_WAL_LEGACY_COMMIT_SIZE
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
    data_size = file_size - SDB_WAL_LEGACY_COMMIT_SIZE;
    commit = buffer + data_size;
    checksum = sdb_crc32_zeroed_range(
        commit,
        SDB_WAL_LEGACY_COMMIT_SIZE,
        SDB_WAL_COMMIT_CHECKSUM_OFFSET,
        sizeof(uint32_t)
    );
    if (memcmp(commit, sdb_wal_commit_magic, sizeof(sdb_wal_commit_magic)) != 0
        || sdb_read_u16_le(commit + 4U) != wal_version
        || sdb_read_u16_le(commit + 6U) != (uint16_t)SDB_WAL_LEGACY_COMMIT_SIZE
        || sdb_read_u64_le(commit + 8U) != txn_id
        || sdb_read_u32_le(commit + 16U) != sdb_crc32(buffer, data_size)
        || sdb_read_u32_le(commit + 20U) != checksum) {
        free(buffer);
        return SDB_OK;
    }
    status = sdb_wal_validate_frames(
        buffer + SDB_WAL_HEADER_SIZE,
        (size_t)superblock->page_size,
        page_count,
        txn_id,
        target_next_page_id,
        SDB_WAL_RECORD_HEADER_SIZE,
        false,
        superblock,
        data_key
    );
    if (status != SDB_OK) {
        free(buffer);
        return status;
    }
    if (txn_id > superblock->checkpoint_lsn) {
        status = sdb_wal_apply_frames(
            buffer + SDB_WAL_HEADER_SIZE,
            (size_t)superblock->page_size,
            page_count,
            txn_id,
            target_next_page_id,
            SDB_WAL_RECORD_HEADER_SIZE,
            database,
            true
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

/*
 * Probe whether the first txn in the buffer is a self-describing v4 txn.
 *
 * Used only when a 64-byte identity header IS present but its CRC failed to
 * validate (bug a fix). A torn/damaged header must NOT default the recovery
 * to the legacy v3 stride: if the WAL is really v4 (frames are 24+page_size,
 * not 8+page_size) with only the header sector damaged, a v3 scan reads the
 * wrong frame size, never finds the commit magic, treats the WAL as empty,
 * and lets the caller clear it -> every committed txn is lost silently
 * (violates invariant 1). The v4 frames are themselves self-describing, so
 * we trust them directly: the first frame must be frame_index 0 and carry a
 * frame_count that places a decodable, running-CRC-verified commit-record at
 * a KNOWN offset. Any failure means this is not an intact v4 txn (a genuine
 * v3 WAL, or a torn first-append before the commit-record was fsynced), and
 * the caller keeps the v3 path, which correctly drops that uncommitted tail.
 */
static bool sdb_wal_first_txn_is_self_describing(
    const uint8_t *buffer,
    size_t file_size,
    size_t page_size,
    size_t record_header_size,
    const uint8_t expected_file_id[SDB_FILE_ID_SIZE]
)
{
    const size_t frame_size = record_header_size + page_size;
    const size_t cursor = SDB_WAL_HEADER_SIZE;
    size_t remaining;
    size_t declared_count;
    size_t commit_offset;
    const uint8_t *commit_ptr;
    sdb_wal_commit_rec rec;
    uint32_t stored_running_crc = 0U;

    if (file_size <= cursor) {
        return false;
    }
    if (record_header_size != SDB_WAL_RECORD_HEADER_SIZE_V4
        && record_header_size != SDB_WAL_RECORD_HEADER_SIZE_V5) {
        return false;
    }
    remaining = file_size - cursor;
    if (remaining < frame_size + SDB_WAL_COMMIT_SIZE) {
        return false;
    }
    if (sdb_read_u32_le(buffer + cursor + SDB_WAL_FRAME_V4_FRAME_INDEX_OFFSET)
        != 0U) {
        return false;
    }
    if (record_header_size == SDB_WAL_RECORD_HEADER_SIZE_V5
        && memcmp(
            buffer + cursor + SDB_WAL_FRAME_V5_FILE_ID_OFFSET,
            expected_file_id,
            SDB_FILE_ID_SIZE
        ) != 0) {
        return false;
    }
    declared_count = (size_t)sdb_read_u32_le(
        buffer + cursor + SDB_WAL_FRAME_V4_FRAME_COUNT_OFFSET
    );
    if (declared_count == 0U
        || declared_count > (size_t)SDB_WAL_MAX_RECORDS
        || declared_count > (remaining - SDB_WAL_COMMIT_SIZE) / frame_size) {
        return false;
    }
    commit_offset = cursor + declared_count * frame_size;
    commit_ptr = buffer + commit_offset;
    if (memcmp(
            commit_ptr, sdb_wal_commit_magic, sizeof(sdb_wal_commit_magic)
        ) != 0
        || sdb_wal_decode_commit_rec(
               commit_ptr, SDB_WAL_COMMIT_SIZE, &rec, &stored_running_crc
           ) != SDB_OK
        || rec.frame_count != (uint32_t)declared_count
        || sdb_crc32(buffer + cursor, declared_count * frame_size)
            != stored_running_crc) {
        return false;
    }
    return true;
}

sdb_status sdb_wal_recover_all(
    const char *wal_path,
    sdb_file *database,
    const sdb_superblock_v1 *superblock,
    const uint8_t *data_key,
    uint64_t *last_lsn_out,
    uint64_t *next_page_id_out,
    uint64_t *freelist_out,
    bool *replayed_out
)
{
    sdb_file file;
    uint8_t *buffer = NULL;
    uint64_t file_size_u64;
    size_t file_size;
    size_t frame_size;
    size_t page_size;
    uint64_t last_lsn;
    uint64_t target_next_page_id;
    uint64_t target_freelist_page;
    uint64_t expected_next;
    size_t cursor;
    bool anything_applied = false;
    bool use_self_describing = false;
    bool identity_bound = false;
    size_t record_header_size = SDB_WAL_RECORD_HEADER_SIZE;
    sdb_status status;

    if (wal_path == NULL || database == NULL || superblock == NULL
        || last_lsn_out == NULL || next_page_id_out == NULL
        || freelist_out == NULL || replayed_out == NULL) {
        return SDB_E_INVALID_ARGUMENT;
    }
    last_lsn = superblock->checkpoint_lsn;
    target_next_page_id = superblock->next_page_id;
    target_freelist_page = superblock->freelist_page;
    *last_lsn_out = last_lsn;
    *next_page_id_out = target_next_page_id;
    *freelist_out = target_freelist_page;
    *replayed_out = false;

    page_size = (size_t)superblock->page_size;
    if (page_size < (size_t)SDB_MIN_PAGE_SIZE
        || page_size > (size_t)SDB_MAX_PAGE_SIZE
        || (page_size & (page_size - 1U)) != 0U) {
        return SDB_E_INVALID_ARGUMENT;
    }
    frame_size = sdb_wal_frame_size(page_size);

    status = sdb_file_open_existing(wal_path, false, &file);
    if (status != SDB_OK) {
        return status == SDB_E_NOT_FOUND ? SDB_OK : status;
    }
    status = sdb_file_size(&file, &file_size_u64);
    if (status != SDB_OK) {
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
    /* Nothing past the reserved header slot — no txns to recover. */
    if (file_size <= SDB_WAL_HEADER_SIZE) {
        (void)sdb_file_close(&file);
        return SDB_OK;
    }
    buffer = (uint8_t *)malloc(file_size);
    if (buffer == NULL) {
        (void)sdb_file_close(&file);
        return SDB_E_OUT_OF_MEMORY;
    }
    status = sdb_file_read_full(&file, 0U, buffer, file_size);
    (void)sdb_file_close(&file);
    if (status != SDB_OK) {
        free(buffer);
        return status;
    }

    /*
     * Identity gate (bug a). The v3 append path stamps a 64-byte identity
     * header into [0, SDB_WAL_HEADER_SIZE) on the first append. If that
     * header is present we MUST confirm it belongs to this database before
     * replaying a single frame, otherwise a foreign / stray .wal sidecar with
     * a matching page_size could overwrite committed pages. A pre-R1 WAL left
     * the header as an all-zero hole; treat that as legacy and fall through to
     * the existing scan (backward-compat — no committed data is lost).
     */
    {
        bool header_present = false;
        size_t header_index;
        for (header_index = 0U;
             header_index < SDB_WAL_HEADER_SIZE;
             ++header_index) {
            if (buffer[header_index] != 0U) {
                header_present = true;
                break;
            }
        }
        if (header_present) {
            const uint16_t header_version = sdb_read_u16_le(buffer + 4U);
            /*
             * The header CRC seals a COMPLETELY-written header. If it
             * validates, the header is trustworthy and MUST identify this
             * database — any magic / version / header_size / page_size /
             * file_id disagreement is a foreign or deliberately-corrupted
             * sidecar and we refuse before touching a single frame.
             *
             * If the CRC does NOT validate, the header is a torn first-append
             * (a crash mid-write of the header+frames buffer, before the
             * commit-record was fsynced) or media damage — NOT a trustworthy
             * foreign header. Fall through to the frame scan, which discards
             * the uncommitted torn first txn exactly as a pre-R1 zero-hole WAL
             * would, so a crash between the header write and the commit-record
             * never turns a legitimately-uncommitted txn into E_CORRUPT.
             */
            const bool crc_ok =
                sdb_crc32_zeroed_range(
                    buffer,
                    SDB_WAL_HEADER_SIZE,
                    SDB_WAL_HEADER_CHECKSUM_OFFSET,
                    sizeof(uint32_t)
                ) == sdb_read_u32_le(
                    buffer + SDB_WAL_HEADER_CHECKSUM_OFFSET
                );
            if (crc_ok
                && (memcmp(buffer, sdb_wal_magic, sizeof(sdb_wal_magic)) != 0
                    || (header_version != SDB_WAL_VERSION_V1
                        && header_version != SDB_WAL_VERSION
                        && header_version != SDB_WAL_VERSION_V3
                        && header_version != SDB_WAL_VERSION_V4
                        && header_version != SDB_WAL_VERSION_V5)
                    || sdb_read_u16_le(buffer + 6U)
                        != (uint16_t)SDB_WAL_HEADER_SIZE
                    || sdb_read_u32_le(buffer + 16U) != superblock->page_size
                    || memcmp(
                        buffer + 24U, superblock->file_id, SDB_FILE_ID_SIZE
                    ) != 0)) {
                free(buffer);
                return SDB_E_CORRUPT;
            }
            if (crc_ok) {
                identity_bound = true;
            }
            /*
             * A trustworthy (CRC-sealed) v4 identity header selects the
             * deterministic self-describing-frame recovery path. Any other
             * case — v3/v1/v2 header, or a torn header whose CRC failed —
             * falls through to the legacy magic-scan over 8-byte frames,
             * preserving backward-compat for pre-Part-B WALs.
             */
            if (crc_ok && header_version == SDB_WAL_VERSION_V4) {
                use_self_describing = true;
                record_header_size = SDB_WAL_RECORD_HEADER_SIZE_V4;
            } else if (crc_ok && header_version == SDB_WAL_VERSION_V5) {
                use_self_describing = true;
                record_header_size = SDB_WAL_RECORD_HEADER_SIZE_V5;
            } else if (!crc_ok
                       && sdb_wal_first_txn_is_self_describing(
                              buffer, file_size, page_size,
                              SDB_WAL_RECORD_HEADER_SIZE_V5,
                              superblock->file_id
                          )) {
                use_self_describing = true;
                identity_bound = true;
                record_header_size = SDB_WAL_RECORD_HEADER_SIZE_V5;
            } else if (!crc_ok
                       && sdb_wal_first_txn_is_self_describing(
                              buffer, file_size, page_size,
                              SDB_WAL_RECORD_HEADER_SIZE_V4,
                              superblock->file_id
                          )) {
                /*
                 * Bug a fix: the identity header sector is torn/damaged
                 * (crc_ok == false) but the self-describing v4 frames and
                 * their commit-record are intact. Trust the frames rather
                 * than falling back to the v3 stride, which would scan a
                 * 24+page_size layout with an 8+page_size step, miss the
                 * commit magic, and silently drop every committed txn.
                 */
                if (data_key == NULL) {
                    free(buffer);
                    return SDB_E_CORRUPT;
                }
                use_self_describing = true;
                record_header_size = SDB_WAL_RECORD_HEADER_SIZE_V4;
            }
        }
    }

    if (!identity_bound && !use_self_describing && data_key == NULL) {
        free(buffer);
        return SDB_E_CORRUPT;
    }
    if (use_self_describing) {
        frame_size = record_header_size + page_size;
    }
    expected_next = superblock->checkpoint_lsn + 1U;
    cursor = SDB_WAL_HEADER_SIZE;

    for (;;) {
        size_t remaining;

        if (cursor >= file_size) {
            break;
        }
        remaining = file_size - cursor;
        /* Need at least one frame plus commit-record to hold a txn. */
        if (remaining < frame_size + SDB_WAL_COMMIT_SIZE) {
            break;
        }

        if (use_self_describing) {
            /*
             * Deterministic v4 boundary (bug b). The first frame of the txn
             * carries frame_count, so the commit-record sits at a KNOWN
             * offset — no probing k*frame_size for a 'SCMT' magic that
             * page data could coincidentally spell. running-CRC over the
             * whole txn stays the primary torn-tail detector; any failed
             * check drops this txn and everything after it (all-or-nothing).
             */
            size_t declared_count = (size_t)sdb_read_u32_le(
                buffer + cursor + SDB_WAL_FRAME_V4_FRAME_COUNT_OFFSET
            );
            size_t commit_offset;
            const uint8_t *commit_ptr;
            sdb_wal_commit_rec rec;
            uint32_t stored_running_crc = 0U;

            if (declared_count == 0U
                || declared_count > (size_t)SDB_WAL_MAX_RECORDS
                || declared_count
                    > (remaining - SDB_WAL_COMMIT_SIZE) / frame_size) {
                break;
            }
            commit_offset = cursor + declared_count * frame_size;
            commit_ptr = buffer + commit_offset;
            if (memcmp(
                    commit_ptr,
                    sdb_wal_commit_magic,
                    sizeof(sdb_wal_commit_magic)
                ) != 0
                || sdb_wal_decode_commit_rec(
                       commit_ptr,
                       SDB_WAL_COMMIT_SIZE,
                       &rec,
                       &stored_running_crc
                   ) != SDB_OK
                || rec.frame_count != (uint32_t)declared_count
                || sdb_crc32(buffer + cursor, declared_count * frame_size)
                    != stored_running_crc
                || rec.txn_id != expected_next
                || rec.next_page_id < target_next_page_id
                || (rec.freelist_page != 0U
                    && rec.freelist_page >= rec.next_page_id)) {
                break;
            }
            status = sdb_wal_validate_frames(
                buffer + cursor,
                page_size,
                declared_count,
                rec.txn_id,
                rec.next_page_id,
                record_header_size,
                true,
                superblock,
                data_key
            );
            if (status != SDB_OK) {
                free(buffer);
                return status;
            }
            if (rec.txn_id > superblock->checkpoint_lsn) {
                status = sdb_wal_apply_frames(
                    buffer + cursor,
                    page_size,
                    declared_count,
                    rec.txn_id,
                    rec.next_page_id,
                    record_header_size,
                    database,
                    false
                );
                if (status != SDB_OK) {
                    free(buffer);
                    return status;
                }
                anything_applied = true;
            }
            last_lsn = rec.txn_id;
            target_next_page_id = rec.next_page_id;
            target_freelist_page = rec.freelist_page;
            cursor = commit_offset + SDB_WAL_COMMIT_SIZE;
            expected_next = rec.txn_id + 1U;
        } else {
            /*
             * Legacy v3 path (pre-Part-B WAL, or a torn/zero-hole header):
             * frames are [page_id:8][data] and the txn length is unknown, so
             * probe k = 1..max_frames for the commit magic + valid CRCs.
             */
            size_t max_frames = (remaining - SDB_WAL_COMMIT_SIZE) / frame_size;
            size_t k;
            bool accepted = false;
            bool torn = false;

            if (max_frames > (size_t)SDB_WAL_MAX_RECORDS) {
                max_frames = (size_t)SDB_WAL_MAX_RECORDS;
            }
            for (k = 1U; k <= max_frames; ++k) {
                size_t commit_offset = cursor + k * frame_size;
                const uint8_t *commit_ptr = buffer + commit_offset;
                sdb_wal_commit_rec rec;
                uint32_t stored_running_crc = 0U;
                uint32_t actual_running_crc;
                sdb_status decode_status;

                if (memcmp(
                        commit_ptr,
                        sdb_wal_commit_magic,
                        sizeof(sdb_wal_commit_magic)
                    ) != 0) {
                    continue;
                }
                decode_status = sdb_wal_decode_commit_rec(
                    commit_ptr,
                    SDB_WAL_COMMIT_SIZE,
                    &rec,
                    &stored_running_crc
                );
                if (decode_status != SDB_OK) {
                    /*
                     * Magic matched but commit-record is invalid: torn /
                     * corrupted commit-record. Stop the whole recovery.
                     */
                    torn = true;
                    break;
                }
                if (rec.frame_count != (uint32_t)k) {
                    /*
                     * Magic + valid inner-CRC on a decodable commit-rec that
                     * doesn't sit at k*frame_size from the txn start means
                     * either extremely improbable page-data coincidence or a
                     * torn frame. Treat as torn to preserve all-or-nothing.
                     */
                    torn = true;
                    break;
                }
                actual_running_crc = sdb_crc32(
                    buffer + cursor, k * frame_size
                );
                if (actual_running_crc != stored_running_crc) {
                    torn = true;
                    break;
                }
                /* Monotone txn ids, contiguous with checkpoint. */
                if (rec.txn_id != expected_next) {
                    torn = true;
                    break;
                }
                /* Freelist / next_page_id must be sane. */
                if (rec.next_page_id < target_next_page_id
                    || (rec.freelist_page != 0U
                        && rec.freelist_page >= rec.next_page_id)) {
                    torn = true;
                    break;
                }
                /*
                 * Validate each frame (page decode, unique page_ids, optional
                 * decrypt sanity check).
                 */
                status = sdb_wal_validate_frames(
                    buffer + cursor,
                    page_size,
                    (size_t)rec.frame_count,
                    rec.txn_id,
                    rec.next_page_id,
                    SDB_WAL_RECORD_HEADER_SIZE,
                    false,
                    superblock,
                    data_key
                );
                if (status != SDB_OK) {
                    free(buffer);
                    return status;
                }
                /*
                 * Apply frames to the data file. Skip the final fsync — we do
                 * one fsync at the end after all txns are applied.
                 */
                if (rec.txn_id > superblock->checkpoint_lsn) {
                    status = sdb_wal_apply_frames(
                        buffer + cursor,
                        page_size,
                        (size_t)rec.frame_count,
                        rec.txn_id,
                        rec.next_page_id,
                        SDB_WAL_RECORD_HEADER_SIZE,
                        database,
                        false
                    );
                    if (status != SDB_OK) {
                        free(buffer);
                        return status;
                    }
                    anything_applied = true;
                }
                last_lsn = rec.txn_id;
                target_next_page_id = rec.next_page_id;
                target_freelist_page = rec.freelist_page;
                cursor = commit_offset + SDB_WAL_COMMIT_SIZE;
                expected_next = rec.txn_id + 1U;
                accepted = true;
                break;
            }
            if (torn || !accepted) {
                break;
            }
        }
    }

    free(buffer);
    if (anything_applied) {
        status = sdb_file_sync(database);
        if (status != SDB_OK) {
            return status;
        }
        *replayed_out = true;
    }
    *last_lsn_out = last_lsn;
    *next_page_id_out = target_next_page_id;
    *freelist_out = target_freelist_page;
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

        return status == SDB_E_NOT_FOUND ? SDB_OK : status;
    }
    status = sdb_file_resize(&file, 0U);
    if (status == SDB_OK) {
        status = sdb_file_sync(&file);
    }
    close_status = sdb_file_close(&file);
    if (status == SDB_OK && close_status == SDB_OK) {

        status = sdb_file_sync_parent_directory(wal_path);
    }
    return status != SDB_OK ? status : close_status;
}

size_t sdb_wal_frame_size(size_t page_size)
{
    return SDB_WAL_RECORD_HEADER_SIZE + page_size;
}

size_t sdb_wal_frame_size_v4(size_t page_size)
{
    return SDB_WAL_RECORD_HEADER_SIZE_V4 + page_size;
}

size_t sdb_wal_frame_size_v5(size_t page_size)
{
    return SDB_WAL_RECORD_HEADER_SIZE_V5 + page_size;
}

void sdb_wal_encode_commit_rec(
    uint8_t *out, const sdb_wal_commit_rec *rec, uint32_t running_crc
)
{
    uint32_t commit_crc;

    (void)memset(out, 0, SDB_WAL_COMMIT_SIZE);
    (void)memcpy(out, sdb_wal_commit_magic, sizeof(sdb_wal_commit_magic));
    sdb_write_u16_le(out + 4U, SDB_WAL_VERSION_V5);
    sdb_write_u16_le(out + 6U, (uint16_t)SDB_WAL_COMMIT_SIZE);
    sdb_write_u64_le(out + 8U, rec->txn_id);
    sdb_write_u32_le(
        out + SDB_WAL_V3_COMMIT_FRAME_COUNT_OFFSET, rec->frame_count
    );
    sdb_write_u64_le(
        out + SDB_WAL_V3_COMMIT_NEXT_PAGE_ID_OFFSET, rec->next_page_id
    );
    sdb_write_u64_le(
        out + SDB_WAL_V3_COMMIT_FREELIST_PAGE_OFFSET, rec->freelist_page
    );
    sdb_write_u32_le(
        out + SDB_WAL_V3_COMMIT_RUNNING_CRC_OFFSET, running_crc
    );
    commit_crc = sdb_crc32_zeroed_range(
        out,
        SDB_WAL_COMMIT_SIZE,
        SDB_WAL_V3_COMMIT_CHECKSUM_OFFSET,
        sizeof(uint32_t)
    );
    sdb_write_u32_le(
        out + SDB_WAL_V3_COMMIT_CHECKSUM_OFFSET, commit_crc
    );
}

sdb_status sdb_wal_decode_commit_rec(
    const uint8_t *in,
    size_t avail,
    sdb_wal_commit_rec *out,
    uint32_t *running_crc_out
)
{
    uint32_t stored_commit_crc;
    uint32_t expected_commit_crc;

    if (in == NULL || out == NULL || running_crc_out == NULL
        || avail < SDB_WAL_COMMIT_SIZE) {
        return SDB_E_INVALID_ARGUMENT;
    }
    {
        /*
         * The 44-byte commit-record layout is identical across v3 and v4 —
         * only the version stamp changed when Part B bumped the frame format.
         * Accept BOTH so a v4 binary can still recover a legacy v3 WAL whose
         * commit-records carry version 3.
         */
        const uint16_t rec_version = sdb_read_u16_le(in + 4U);
        if (memcmp(in, sdb_wal_commit_magic, sizeof(sdb_wal_commit_magic)) != 0
            || (rec_version != SDB_WAL_VERSION_V3
                && rec_version != SDB_WAL_VERSION_V4
                && rec_version != SDB_WAL_VERSION_V5)
            || sdb_read_u16_le(in + 6U) != (uint16_t)SDB_WAL_COMMIT_SIZE) {
            return SDB_E_CORRUPT;
        }
    }
    stored_commit_crc = sdb_read_u32_le(
        in + SDB_WAL_V3_COMMIT_CHECKSUM_OFFSET
    );
    expected_commit_crc = sdb_crc32_zeroed_range(
        in,
        SDB_WAL_COMMIT_SIZE,
        SDB_WAL_V3_COMMIT_CHECKSUM_OFFSET,
        sizeof(uint32_t)
    );
    if (stored_commit_crc != expected_commit_crc) {
        return SDB_E_CORRUPT;
    }
    out->txn_id = sdb_read_u64_le(in + 8U);
    out->frame_count = sdb_read_u32_le(
        in + SDB_WAL_V3_COMMIT_FRAME_COUNT_OFFSET
    );
    out->next_page_id = sdb_read_u64_le(
        in + SDB_WAL_V3_COMMIT_NEXT_PAGE_ID_OFFSET
    );
    out->freelist_page = sdb_read_u64_le(
        in + SDB_WAL_V3_COMMIT_FREELIST_PAGE_OFFSET
    );
    *running_crc_out = sdb_read_u32_le(
        in + SDB_WAL_V3_COMMIT_RUNNING_CRC_OFFSET
    );
    return SDB_OK;
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
