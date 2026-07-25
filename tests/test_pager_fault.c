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

    /*
     * Cut every pager allocation at successive file-operation boundaries.
     * Regardless of whether allocation committed, the database must reopen
     * and remain capable of allocating, writing, and reading a valid page.
     */
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
