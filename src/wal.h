#ifndef SHIBADB_WAL_H
#define SHIBADB_WAL_H

#include "file.h"
#include "page.h"

#define SDB_WAL_HEADER_SIZE ((size_t)64)
#define SDB_WAL_RECORD_HEADER_SIZE ((size_t)8)
/*
 * WAL v4 self-describing frame header. v3 frames were [page_id:8][data]; v4
 * grows the record header to 24 bytes so recovery can read the txn's frame
 * count straight from the first frame and land on the commit-record at a
 * DETERMINISTIC offset instead of probing k*frame_size for a 'SCMT' magic:
 *   0  u64  page_id
 *   8  u64  txn_id       (which txn this frame belongs to)
 *   16 u32  frame_index  (0-based position within the txn)
 *   20 u32  frame_count  (total frames in the txn; == commit_rec.frame_count)
 *   24 ...  page data (page_size bytes)
 * SDB_WAL_RECORD_HEADER_SIZE stays 8 for the v3 read path (backward-compat).
 */
#define SDB_WAL_RECORD_HEADER_SIZE_V4 ((size_t)24)
#define SDB_WAL_FRAME_V4_TXN_ID_OFFSET ((size_t)8)
#define SDB_WAL_FRAME_V4_FRAME_INDEX_OFFSET ((size_t)16)
#define SDB_WAL_FRAME_V4_FRAME_COUNT_OFFSET ((size_t)20)
/*
 * WAL v5 binds every self-describing frame to its owning database even when
 * the separate identity-header sector is torn:
 *   0..23  v4 fields
 *   24..39 file_id
 *   40..   page bytes
 */
#define SDB_WAL_RECORD_HEADER_SIZE_V5 ((size_t)40)
#define SDB_WAL_FRAME_V5_FILE_ID_OFFSET ((size_t)24)
#define SDB_WAL_COMMIT_SIZE ((size_t)44)
#define SDB_WAL_MAX_RECORDS UINT32_C(1048576)
#define SDB_WAL_MAX_BYTES UINT64_C(268435456)
#define SDB_WAL_VERSION_V3 UINT16_C(3)
#define SDB_WAL_VERSION_V4 UINT16_C(4)
#define SDB_WAL_VERSION_V5 UINT16_C(5)

typedef struct sdb_wal_page {
    uint64_t page_id;
    const uint8_t *bytes;
} sdb_wal_page;

typedef struct sdb_wal_commit_rec {
    uint64_t txn_id;
    uint32_t frame_count;
    uint64_t next_page_id;
    uint64_t freelist_page;
} sdb_wal_commit_rec;

size_t sdb_wal_frame_size(size_t page_size);
size_t sdb_wal_frame_size_v4(size_t page_size);
size_t sdb_wal_frame_size_v5(size_t page_size);
void sdb_wal_encode_commit_rec(
    uint8_t *out, const sdb_wal_commit_rec *rec, uint32_t running_crc
);
sdb_status sdb_wal_decode_commit_rec(
    const uint8_t *in,
    size_t avail,
    sdb_wal_commit_rec *out,
    uint32_t *running_crc_out
);

sdb_status sdb_wal_write_committed(
    const char *wal_path,
    const sdb_superblock_v1 *superblock,
    uint64_t txn_id,
    const sdb_wal_page *pages,
    size_t page_count
);
sdb_status sdb_wal_append_txn(
    sdb_file *wal,
    uint64_t append_offset,
    const sdb_superblock_v1 *superblock,
    uint64_t txn_id,
    const sdb_wal_page *pages,
    size_t page_count,
    uint64_t *new_offset_out
);
/*
 * R4 group commit: encode one txn into a heap staging buffer without any I/O
 * (see the definition for the full contract). The caller frees *buffer_out.
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
);
sdb_status sdb_wal_recover(
    const char *wal_path,
    sdb_file *database,
    const sdb_superblock_v1 *superblock,
    const uint8_t *data_key,
    uint64_t *txn_id_out,
    uint64_t *next_page_id_out,
    uint64_t *freelist_page_out,
    bool *replayed_out
);
sdb_status sdb_wal_recover_all(
    const char *wal_path,
    sdb_file *database,
    const sdb_superblock_v1 *superblock,
    const uint8_t *data_key,
    uint64_t *last_lsn_out,
    uint64_t *next_page_id_out,
    uint64_t *freelist_out,
    bool *replayed_out
);
sdb_status sdb_wal_clear(const char *wal_path);
#if SDB_TESTING
void sdb_wal_fail_after_for_testing(size_t successful_operations);
void sdb_wal_clear_failure_for_testing(void);
#endif

#endif
