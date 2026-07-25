#ifndef SHIBADB_PAGER_H
#define SHIBADB_PAGER_H

#include "file.h"
#include "crypto.h"
#include "page.h"
#include "page_cache.h"
#include "wal.h"

#define SDB_PAGER_CACHE_CAPACITY 64U

typedef struct sdb_pager {
    sdb_file file;
    sdb_superblock_v1 superblock;
    sdb_page_cache cache;
    char *wal_path;
    uint8_t data_key[SDB_CRYPTO_KEY_SIZE];
    bool open;
    bool needs_recovery;
    bool transaction_active;
    bool encryption_enabled;
} sdb_pager;

typedef struct sdb_txn_page {
    uint64_t page_id;
    uint16_t type;
    uint8_t *payload;
    size_t payload_size;
} sdb_txn_page;

typedef struct sdb_txn {
    sdb_pager *pager;
    sdb_superblock_v1 target_superblock;
    sdb_txn_page *pages;
    size_t page_count;
    size_t page_capacity;
    uint64_t *allocated_pages;
    size_t allocated_page_count;
    size_t allocated_page_capacity;
    bool active;
} sdb_txn;

sdb_status sdb_pager_create(
    const char *path,
    uint32_t page_size,
    const uint8_t salt[SDB_SALT_SIZE],
    const uint8_t file_id[SDB_FILE_ID_SIZE],
    sdb_pager *pager_out
);
sdb_status sdb_pager_open(const char *path, sdb_pager *pager_out);
sdb_status sdb_pager_create_encrypted(
    const char *path,
    uint32_t page_size,
    const uint8_t salt[SDB_SALT_SIZE],
    const uint8_t file_id[SDB_FILE_ID_SIZE],
    const uint8_t *password,
    size_t password_size,
    uint32_t kdf_iterations,
    sdb_pager *pager_out
);
sdb_status sdb_pager_open_encrypted(
    const char *path,
    const uint8_t *password,
    size_t password_size,
    sdb_pager *pager_out
);
sdb_status sdb_pager_rotate_password(
    sdb_pager *pager,
    const uint8_t *new_password,
    size_t new_password_size,
    uint32_t new_kdf_iterations
);
size_t sdb_pager_payload_capacity(const sdb_pager *pager);
sdb_status sdb_pager_close(sdb_pager *pager);
sdb_status sdb_pager_allocate(sdb_pager *pager, uint64_t *page_id_out);
sdb_status sdb_pager_free(sdb_pager *pager, uint64_t page_id);
sdb_status sdb_pager_write(
    sdb_pager *pager,
    uint64_t page_id,
    uint16_t type,
    uint64_t page_lsn,
    const uint8_t *payload,
    size_t payload_size
);
sdb_status sdb_pager_read(
    sdb_pager *pager,
    uint64_t page_id,
    uint8_t *page_buffer,
    size_t page_buffer_size,
    sdb_page_view *view_out
);
sdb_status sdb_txn_begin(sdb_pager *pager, sdb_txn *txn_out);
sdb_status sdb_txn_put(
    sdb_txn *txn,
    uint64_t page_id,
    uint16_t type,
    const uint8_t *payload,
    size_t payload_size
);
sdb_status sdb_txn_allocate(sdb_txn *txn, uint64_t *page_id_out);
sdb_status sdb_txn_free(sdb_txn *txn, uint64_t page_id);
sdb_status sdb_txn_commit(sdb_txn *txn);
void sdb_txn_abort(sdb_txn *txn);

#endif
