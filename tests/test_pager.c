#include "pager.h"

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
    assert(sdb_pager_close(&pager) == SDB_OK);

    assert(sdb_pager_open(test_path, &pager) == SDB_OK);
    assert(sdb_pager_read(
        &pager, first, page, sizeof(page), &view
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

    /* The allocation watermark and freelist state survive another reopen. */
    assert(sdb_pager_open(test_path, &pager) == SDB_OK);
    assert(pager.superblock.next_page_id == 4U);
    assert(pager.superblock.freelist_page == 0U);

    /* Deterministic model test across repeated close/open boundaries. */
    allocated[reused] = true;
    allocated[third] = true;
    allocated[fourth] = true;
    for (operation = 0U; operation < 400U; ++operation) {
        uint32_t choice = next_random(&random_state);
        uint64_t candidate = (uint64_t)((choice % 500U) + 1U);
        if ((choice & 1U) != 0U && allocated[candidate]) {
            assert(sdb_pager_free(&pager, candidate) == SDB_OK);
            allocated[candidate] = false;
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
        }
        if ((operation % 37U) == 36U) {
            assert(sdb_pager_close(&pager) == SDB_OK);
            assert(sdb_pager_open(test_path, &pager) == SDB_OK);
        }
    }
    assert(sdb_pager_close(&pager) == SDB_OK);
    (void)remove(test_path);
    (void)puts("pager tests: ok");
    return 0;
}
