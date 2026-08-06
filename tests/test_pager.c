#include "pager.h"
#include "superblock_store.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static const char *test_path = "test-pager.tmp";

static uint32_t next_random(uint32_t *state)
{
    *state = (*state * UINT32_C(1664525)) + UINT32_C(1013904223);
    return *state;
}

static void fill_identity(uint8_t salt[16], uint8_t file_id[16])
{
    size_t index;
    for (index = 0U; index < 16U; ++index) {
        salt[index] = (uint8_t)(index + 1U);
        file_id[index] = (uint8_t)(0xc0U + index);
    }
}

static void corrupt_superblock_byte(uint64_t offset)
{
    sdb_file file;
    uint8_t byte;
    assert(sdb_file_open_existing(test_path, true, &file) == SDB_OK);
    assert(sdb_file_read_full(&file, offset, &byte, 1U) == SDB_OK);
    byte ^= UINT8_C(0x40);
    assert(sdb_file_write_full(&file, offset, &byte, 1U) == SDB_OK);
    assert(sdb_file_sync(&file) == SDB_OK);
    assert(sdb_file_close(&file) == SDB_OK);
}

int main(void)
{
    sdb_pager pager;
    uint8_t salt[16];
    uint8_t file_id[16];
    uint8_t page[4096];
    static const uint8_t payload[] = "persistent page";
    sdb_page_view view;
    uint64_t first;
    uint64_t second;
    uint64_t reused;
    uint64_t third;
    uint64_t fourth;
    bool allocated[512] = {false};
    bool has_value[512] = {false};
    uint32_t random_state = UINT32_C(0x51ba1234);
    size_t operation;

    (void)remove(test_path);
    fill_identity(salt, file_id);
    assert(sdb_pager_create(test_path, 4096U, salt, file_id, &pager) == SDB_OK);
    assert(sdb_pager_allocate(&pager, &first) == SDB_OK);
    assert(sdb_pager_allocate(&pager, &second) == SDB_OK);
    assert(first == 1U);
    assert(second == 2U);
    assert(sdb_pager_write(
        &pager, first, (uint16_t)SDB_PAGE_TYPE_DATA,
        1U, payload, sizeof(payload)
    ) == SDB_OK);
    assert(sdb_pager_write(
        &pager, second, (uint16_t)SDB_PAGE_TYPE_DATA,
        2U, payload, sizeof(payload)
    ) == SDB_OK);
    /* Read both pages back in-session: the bytes just written must round-trip. */
    assert(sdb_pager_read(
        &pager, first, page, sizeof(page), &view
    ) == SDB_OK);
    assert(view.payload_size == sizeof(payload));
    assert(memcmp(view.payload, payload, sizeof(payload)) == 0);
    assert(sdb_pager_read(
        &pager, second, page, sizeof(page), &view
    ) == SDB_OK);
    assert(view.payload_size == sizeof(payload));
    assert(memcmp(view.payload, payload, sizeof(payload)) == 0);
    assert(sdb_pager_close(&pager) == SDB_OK);

    /* Open must repair a damaged peer before returning the handle. */
    corrupt_superblock_byte(40U);
    assert(sdb_pager_open(test_path, &pager) == SDB_OK);
    assert(sdb_pager_close(&pager) == SDB_OK);
    {
        sdb_superblock_read_result mirrors;
        assert(sdb_superblock_store_read(test_path, &mirrors) == SDB_OK);
        assert(mirrors.valid_mirror_count == 2U);
    }

    assert(sdb_pager_open(test_path, &pager) == SDB_OK);
    /* After close/reopen the payload must still match exactly, for BOTH pages. */
    assert(sdb_pager_read(
        &pager, first, page, sizeof(page), &view
    ) == SDB_OK);
    assert(view.payload_size == sizeof(payload));
    assert(memcmp(view.payload, payload, sizeof(payload)) == 0);
    assert(sdb_pager_read(
        &pager, second, page, sizeof(page), &view
    ) == SDB_OK);
    assert(view.payload_size == sizeof(payload));
    assert(memcmp(view.payload, payload, sizeof(payload)) == 0);
    assert(sdb_pager_free(&pager, first) == SDB_OK);
    assert(sdb_pager_free(&pager, first) == SDB_E_INVALID_ARGUMENT);
    assert(sdb_pager_free(&pager, second) == SDB_OK);
    assert(sdb_pager_close(&pager) == SDB_OK);

    assert(sdb_pager_open(test_path, &pager) == SDB_OK);
    assert(sdb_pager_allocate(&pager, &reused) == SDB_OK);
    assert(reused == second);
    assert(sdb_pager_allocate(&pager, &third) == SDB_OK);
    assert(third == first);
    assert(sdb_pager_allocate(&pager, &fourth) == SDB_OK);
    assert(fourth == 3U);
    assert(sdb_pager_close(&pager) == SDB_OK);

    assert(sdb_pager_open(test_path, &pager) == SDB_OK);
    assert(pager.superblock.next_page_id == 4U);
    assert(pager.superblock.freelist_page == 0U);

    allocated[reused] = true;
    allocated[third] = true;
    allocated[fourth] = true;
    for (operation = 0U; operation < 400U; ++operation) {
        uint32_t choice = next_random(&random_state);
        uint64_t candidate = (uint64_t)((choice % 500U) + 1U);
        if ((choice & 1U) != 0U && allocated[candidate]) {
            assert(sdb_pager_free(&pager, candidate) == SDB_OK);
            allocated[candidate] = false;
            has_value[candidate] = false;
        } else {
            uint64_t allocated_id;
            uint8_t value;
            assert(sdb_pager_allocate(&pager, &allocated_id) == SDB_OK);
            assert(allocated_id < 512U);
            assert(!allocated[allocated_id]);
            allocated[allocated_id] = true;
            value = (uint8_t)allocated_id;
            assert(sdb_pager_write(
                &pager,
                allocated_id,
                (uint16_t)SDB_PAGE_TYPE_DATA,
                (uint64_t)operation + 10U,
                &value,
                sizeof(value)
            ) == SDB_OK);
            /* Read the page straight back: the written byte must round-trip. */
            assert(sdb_pager_read(
                &pager, allocated_id, page, sizeof(page), &view
            ) == SDB_OK);
            assert(view.payload_size == sizeof(value));
            assert(view.payload[0] == value);
            has_value[allocated_id] = true;
        }
        if ((operation % 37U) == 36U) {
            assert(sdb_pager_close(&pager) == SDB_OK);
            assert(sdb_pager_open(test_path, &pager) == SDB_OK);
            /*
             * After close/reopen every page that still holds a written value
             * must read back its exact byte (the low byte of its page id). A
             * durability or freelist-reuse bug that lost or aliased a payload
             * across the reopen trips here.
             */
            {
                uint64_t verify_id;
                for (verify_id = 1U; verify_id < 512U; ++verify_id) {
                    if (has_value[verify_id]) {
                        assert(sdb_pager_read(
                            &pager, verify_id, page, sizeof(page), &view
                        ) == SDB_OK);
                        assert(view.payload_size == 1U);
                        assert(view.payload[0] == (uint8_t)verify_id);
                    }
                }
            }
        }
    }
    assert(sdb_pager_close(&pager) == SDB_OK);
    (void)remove(test_path);
    (void)puts("pager tests: ok");
    return 0;
}
