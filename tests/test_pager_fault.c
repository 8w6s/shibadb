#include "pager.h"

#include <assert.h>
#include <stdio.h>

static const char *test_path = "test-pager-fault.tmp";

static uint64_t expected_file_size(const sdb_pager *pager)
{
    return (uint64_t)(
            SDB_SUPERBLOCK_SLOT_SIZE * SDB_SUPERBLOCK_SLOT_COUNT
        )
        + ((pager->superblock.next_page_id - 1U)
            * (uint64_t)pager->superblock.page_size);
}

static void assert_canonical_file_size(sdb_pager *pager)
{
    uint64_t actual_size;
    assert(sdb_file_size(&pager->file, &actual_size) == SDB_OK);
    assert(actual_size == expected_file_size(pager));
}

int main(void)
{
    uint8_t salt[16] = {1U};
    uint8_t file_id[16] = {2U};
    size_t boundary;

    for (boundary = 0U; boundary < 20U; ++boundary) {
        sdb_pager pager;
        uint8_t page[4096];
        uint8_t value = 0x5aU;
        uint64_t interrupted_id = 0U;
        uint64_t recovery_id;
        sdb_page_view view;

        (void)remove(test_path);
        assert(sdb_pager_create(
            test_path, 4096U, salt, file_id, &pager
        ) == SDB_OK);
        sdb_file_fail_after_for_testing(&pager.file, boundary);
        (void)sdb_pager_allocate(&pager, &interrupted_id);
        sdb_file_clear_failure_for_testing(&pager.file);
        assert(sdb_pager_close(&pager) == SDB_OK);

        assert(sdb_pager_open(test_path, &pager) == SDB_OK);
        assert_canonical_file_size(&pager);
        assert(sdb_pager_allocate(&pager, &recovery_id) == SDB_OK);
        assert(sdb_pager_write(
            &pager,
            recovery_id,
            (uint16_t)SDB_PAGE_TYPE_DATA,
            1U,
            &value,
            sizeof(value)
        ) == SDB_OK);
        assert(sdb_pager_read(
            &pager, recovery_id, page, sizeof(page), &view
        ) == SDB_OK);
        assert(view.payload_size == sizeof(value));
        assert(view.payload[0] == value);
        assert_canonical_file_size(&pager);
        assert(sdb_pager_close(&pager) == SDB_OK);
    }
    /*
     * Write-then-publish (invariant A.2): a crash while GROWING the file for a
     * new page must never leave an uninitialized page inside the committed
     * [1, next_page_id) range. Seed three good pages, interrupt the next
     * allocate at each I/O boundary, reopen, and assert every page still inside
     * the committed range reads back cleanly — the interrupted page is
     * reclaimed as a trailing region rather than becoming an in-range orphan.
     */
    for (boundary = 0U; boundary < 20U; ++boundary) {
        sdb_pager pager;
        uint8_t scan_buf[4096];
        sdb_page_view view;
        uint8_t value = 0x27U;
        uint64_t seed;
        uint64_t scan;
        uint64_t interrupted_id = 0U;

        (void)remove(test_path);
        assert(sdb_pager_create(
            test_path, 4096U, salt, file_id, &pager
        ) == SDB_OK);
        for (seed = 0U; seed < 3U; ++seed) {
            uint64_t seed_id;
            assert(sdb_pager_allocate(&pager, &seed_id) == SDB_OK);
            assert(sdb_pager_write(
                &pager, seed_id, (uint16_t)SDB_PAGE_TYPE_DATA,
                1U, &value, sizeof(value)
            ) == SDB_OK);
        }
        sdb_file_fail_after_for_testing(&pager.file, boundary);
        (void)sdb_pager_allocate(&pager, &interrupted_id);
        sdb_file_clear_failure_for_testing(&pager.file);
        assert(sdb_pager_close(&pager) == SDB_OK);

        assert(sdb_pager_open(test_path, &pager) == SDB_OK);
        assert_canonical_file_size(&pager);
        for (scan = 1U; scan < pager.superblock.next_page_id; ++scan) {
            assert(sdb_pager_read(
                &pager, scan, scan_buf, sizeof(scan_buf), &view
            ) == SDB_OK);
        }
        assert(sdb_pager_close(&pager) == SDB_OK);
    }
    {
        sdb_pager pager;
        sdb_file file;
        uint64_t canonical_size;
        uint64_t page_id;
        (void)remove(test_path);
        assert(sdb_pager_create(
            test_path, 4096U, salt, file_id, &pager
        ) == SDB_OK);
        assert(sdb_pager_allocate(&pager, &page_id) == SDB_OK);
        canonical_size = expected_file_size(&pager);
        assert(sdb_pager_close(&pager) == SDB_OK);
        assert(sdb_file_open_existing(test_path, true, &file) == SDB_OK);
        assert(sdb_file_resize(&file, canonical_size + 137U) == SDB_OK);
        assert(sdb_file_sync(&file) == SDB_OK);
        assert(sdb_file_close(&file) == SDB_OK);
        assert(sdb_pager_open(test_path, &pager) == SDB_OK);
        assert_canonical_file_size(&pager);
        assert(sdb_pager_close(&pager) == SDB_OK);

        assert(sdb_file_open_existing(test_path, true, &file) == SDB_OK);
        assert(sdb_file_resize(&file, canonical_size - 1U) == SDB_OK);
        assert(sdb_file_sync(&file) == SDB_OK);
        assert(sdb_file_close(&file) == SDB_OK);
        assert(sdb_pager_open(test_path, &pager) == SDB_E_TRUNCATED);
        assert(!pager.open);
    }
    (void)remove(test_path);
    (void)puts("pager fault tests: ok");
    return 0;
}
