#include "pager.h"
#include "wal.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static const char *test_path = "test-transaction-crash.tmp";
static const char *wal_path = "test-transaction-crash.tmp.wal";
static const uint8_t old_value[] = "old";
static const uint8_t new_value[] = "new";

static void prepare(
    sdb_pager *pager, uint64_t *first_out, uint64_t *second_out
)
{
    uint8_t salt[16] = {7U};
    uint8_t file_id[16] = {8U};
    (void)remove(test_path);
    (void)remove(wal_path);
    assert(sdb_pager_create(
        test_path, 4096U, salt, file_id, pager
    ) == SDB_OK);
    assert(sdb_pager_allocate(pager, first_out) == SDB_OK);
    assert(sdb_pager_allocate(pager, second_out) == SDB_OK);
    assert(sdb_pager_write(
        pager, *first_out, (uint16_t)SDB_PAGE_TYPE_DATA, 0U,
        old_value, sizeof(old_value)
    ) == SDB_OK);
    assert(sdb_pager_write(
        pager, *second_out, (uint16_t)SDB_PAGE_TYPE_DATA, 0U,
        old_value, sizeof(old_value)
    ) == SDB_OK);
}

static bool is_new(sdb_pager *pager, uint64_t page_id)
{
    uint8_t page[4096];
    sdb_page_view view;
    assert(sdb_pager_read(
        pager, page_id, page, sizeof(page), &view
    ) == SDB_OK);
    assert(view.payload_size == sizeof(old_value));
    if (memcmp(view.payload, new_value, sizeof(new_value)) == 0) {
        return true;
    }
    assert(memcmp(view.payload, old_value, sizeof(old_value)) == 0);
    return false;
}

static void stage_and_commit(sdb_pager *pager, uint64_t first, uint64_t second)
{
    sdb_txn txn;
    uint64_t reserved;
    assert(sdb_txn_begin(pager, &txn) == SDB_OK);
    assert(sdb_txn_allocate(&txn, &reserved) == SDB_OK);
    assert(reserved == pager->superblock.next_page_id);
    assert(sdb_txn_put(
        &txn, first, (uint16_t)SDB_PAGE_TYPE_DATA,
        new_value, sizeof(new_value)
    ) == SDB_OK);
    assert(sdb_txn_put(
        &txn, second, (uint16_t)SDB_PAGE_TYPE_DATA,
        new_value, sizeof(new_value)
    ) == SDB_OK);
    assert(sdb_txn_put(
        &txn, reserved, (uint16_t)SDB_PAGE_TYPE_DATA,
        new_value, sizeof(new_value)
    ) == SDB_OK);
    (void)sdb_txn_commit(&txn);
}

static void verify_atomic_reopen(uint64_t first, uint64_t second)
{
    sdb_pager pager;
    bool first_new;
    bool second_new;
    const uint64_t reserved = second + 1U;
    assert(sdb_pager_open(test_path, &pager) == SDB_OK);
    first_new = is_new(&pager, first);
    second_new = is_new(&pager, second);
    assert(first_new == second_new);
    if (first_new) {
        assert(pager.superblock.next_page_id == reserved + 1U);
        assert(is_new(&pager, reserved));
    } else {
        assert(pager.superblock.next_page_id == reserved);
    }
    assert(sdb_pager_close(&pager) == SDB_OK);
}

int main(void)
{
    size_t boundary;

    /* Interrupt WAL body, body sync, commit marker, and commit sync. */
    for (boundary = 0U; boundary < 6U; ++boundary) {
        sdb_pager pager;
        uint64_t first;
        uint64_t second;
        prepare(&pager, &first, &second);
        sdb_wal_fail_after_for_testing(boundary);
        stage_and_commit(&pager, first, second);
        sdb_wal_clear_failure_for_testing();
        assert(sdb_pager_close(&pager) == SDB_OK);
        verify_atomic_reopen(first, second);
    }

    /* Interrupt page application, database sync, and checkpoint mirrors. */
    for (boundary = 0U; boundary < 10U; ++boundary) {
        sdb_pager pager;
        uint64_t first;
        uint64_t second;
        prepare(&pager, &first, &second);
        sdb_file_fail_after_for_testing(&pager.file, boundary);
        stage_and_commit(&pager, first, second);
        sdb_file_clear_failure_for_testing(&pager.file);
        assert(sdb_pager_close(&pager) == SDB_OK);
        verify_atomic_reopen(first, second);
    }

    (void)remove(wal_path);
    (void)remove(test_path);
    (void)puts("transaction crash tests: ok");
    return 0;
}
