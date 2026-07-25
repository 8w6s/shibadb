#ifndef SHIBADB_WAL_H
#define SHIBADB_WAL_H

#include "file.h"
#include "page.h"

#define SDB_WAL_HEADER_SIZE ((size_t)64)
#define SDB_WAL_RECORD_HEADER_SIZE ((size_t)8)
#define SDB_WAL_COMMIT_SIZE ((size_t)24)
#define SDB_WAL_MAX_RECORDS UINT32_C(1048576)
#define SDB_WAL_MAX_BYTES UINT64_C(268435456)

typedef struct sdb_wal_page {
    uint64_t page_id;
    const uint8_t *bytes;
} sdb_wal_page;

sdb_status sdb_wal_write_committed(
    const char *wal_path,
    const sdb_superblock_v1 *superblock,
    uint64_t txn_id,
    const sdb_wal_page *pages,
    size_t page_count
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
sdb_status sdb_wal_clear(const char *wal_path);
#if SDB_TESTING
void sdb_wal_fail_after_for_testing(size_t successful_operations);
void sdb_wal_clear_failure_for_testing(void);
#endif

#endif
