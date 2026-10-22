#include "encrypted_page.h"
#include "pager.h"
#include "wal.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static const char *test_path = "test-checkpoint-encryption.tmp";
static const char *wal_path = "test-checkpoint-encryption.tmp.wal";
static const uint8_t password[] = "checkpoint-encryption-password";
static const uint8_t original_value[] = "original";
static const uint8_t updated_value[] = "updated";

static void cleanup(void)
{
    (void)remove(test_path);
    (void)remove(wal_path);
    (void)remove("test-checkpoint-encryption.tmp.lock");
}

int main(void)
{
    sdb_pager pager;
    sdb_txn transaction;
    sdb_page_view view;
    uint8_t before[4096];
    uint8_t after[4096];
    uint8_t frame[4096];
    uint8_t payload[4096];
    uint8_t salt[SDB_SALT_SIZE];
    uint8_t file_id[SDB_FILE_ID_SIZE];
    uint64_t page_id;
    uint64_t frame_offset;
    const uint64_t data_offset =
        (uint64_t)(SDB_SUPERBLOCK_SLOT_SIZE * SDB_SUPERBLOCK_SLOT_COUNT);

    memset(salt, 0x21, sizeof(salt));
    memset(file_id, 0x32, sizeof(file_id));
    cleanup();
    assert(sdb_pager_create_encrypted(
        test_path,
        4096U,
        salt,
        file_id,
        password,
        sizeof(password) - 1U,
        SDB_MIN_KDF_ITERATIONS,
        &pager
    ) == SDB_OK);
    assert(sdb_pager_allocate(&pager, &page_id) == SDB_OK);
    assert(sdb_pager_write(
        &pager,
        page_id,
        (uint16_t)SDB_PAGE_TYPE_DATA,
        1U,
        original_value,
        sizeof(original_value)
    ) == SDB_OK);
    assert(sdb_txn_begin(&pager, &transaction) == SDB_OK);
    assert(sdb_txn_put(
        &transaction,
        page_id,
        (uint16_t)SDB_PAGE_TYPE_DATA,
        updated_value,
        sizeof(updated_value)
    ) == SDB_OK);
    assert(sdb_txn_commit(&transaction) == SDB_OK);
    assert(sdb_wal_index_get(
        &pager.wal_index, page_id, &frame_offset
    ));
    assert(sdb_file_read_full(
        &pager.file,
        data_offset + (page_id - 1U) * (uint64_t)sizeof(before),
        before,
        sizeof(before)
    ) == SDB_OK);

    assert(sdb_file_read_full(
        &pager.wal_file,
        frame_offset + (uint64_t)SDB_WAL_RECORD_HEADER_SIZE_V5,
        frame,
        sizeof(frame)
    ) == SDB_OK);
    assert(sdb_page_decode(
        frame, sizeof(frame), page_id, &view
    ) == SDB_OK);
    assert(view.type == (uint16_t)SDB_PAGE_TYPE_ENCRYPTED);
    (void)memcpy(payload, view.payload, view.payload_size);
    payload[SDB_ENCRYPTED_PAYLOAD_HEADER_SIZE] ^= UINT8_C(1);
    assert(sdb_page_encode(
        frame,
        sizeof(frame),
        (uint16_t)SDB_PAGE_TYPE_ENCRYPTED,
        page_id,
        view.page_lsn,
        payload,
        view.payload_size
    ) == SDB_OK);
    assert(sdb_file_write_full(
        &pager.wal_file,
        frame_offset + (uint64_t)SDB_WAL_RECORD_HEADER_SIZE_V5,
        frame,
        sizeof(frame)
    ) == SDB_OK);
    assert(sdb_file_sync(&pager.wal_file) == SDB_OK);

    assert(sdb_pager_checkpoint(&pager) == SDB_E_CORRUPT);
    assert(sdb_file_read_full(
        &pager.file,
        data_offset + (page_id - 1U) * (uint64_t)sizeof(after),
        after,
        sizeof(after)
    ) == SDB_OK);
    assert(memcmp(before, after, sizeof(before)) == 0);

    (void)sdb_pager_close(&pager);
    cleanup();
    (void)puts("checkpoint encryption: forged ciphertext rejected ok");
    return 0;
}
