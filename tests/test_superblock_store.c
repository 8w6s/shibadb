#include "file.h"
#include "superblock_store.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static const char *test_path = "test-superblock-store.tmp";

static sdb_superblock_v1 sample(uint64_t generation)
{
    sdb_superblock_v1 value;
    size_t index;
    (void)memset(&value, 0, sizeof(value));
    value.page_size = 4096U;
    value.generation = generation;
    value.checkpoint_lsn = generation * UINT64_C(10);
    value.root_page = 2U;
    value.next_page_id = 3U;
    for (index = 0U; index < SDB_SALT_SIZE; ++index) {
        value.salt[index] = (uint8_t)(index + 1U);
        value.file_id[index] = (uint8_t)(0x80U + index);
    }
    return value;
}

static void cleanup(void)
{
    (void)remove(test_path);
}

static void corrupt_byte(uint64_t offset)
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

static void write_slot(uint8_t slot, const sdb_superblock_v1 *value)
{
    sdb_file file;
    uint8_t bytes[SDB_SUPERBLOCK_SLOT_SIZE];
    assert(sdb_superblock_v1_encode(value, bytes, sizeof(bytes)) == SDB_OK);
    assert(sdb_file_open_existing(test_path, true, &file) == SDB_OK);
    assert(sdb_file_write_full(
        &file,
        (uint64_t)slot * (uint64_t)SDB_SUPERBLOCK_SLOT_SIZE,
        bytes,
        sizeof(bytes)
    ) == SDB_OK);
    assert(sdb_file_sync(&file) == SDB_OK);
    assert(sdb_file_close(&file) == SDB_OK);
}

static void test_create_update_and_reopen(void)
{
    sdb_superblock_v1 first = sample(1U);
    sdb_superblock_v1 second = sample(2U);
    sdb_superblock_read_result result;
    cleanup();
    assert(sdb_superblock_store_create(test_path, &first) == SDB_OK);
    assert(sdb_superblock_store_read(test_path, &result) == SDB_OK);
    assert(result.valid_mirror_count == 2U);
    assert(result.superblock.generation == 1U);
    assert(sdb_superblock_store_update(test_path, &second) == SDB_OK);
    assert(sdb_superblock_store_read(test_path, &result) == SDB_OK);
    assert(result.valid_mirror_count == 2U);
    assert(result.superblock.generation == 2U);
    assert(sdb_superblock_store_update(test_path, &second) == SDB_E_INVALID_ARGUMENT);
    cleanup();
}

static void test_one_corrupt_mirror_recovers(void)
{
    sdb_superblock_v1 value = sample(3U);
    sdb_superblock_read_result result;
    cleanup();
    assert(sdb_superblock_store_create(test_path, &value) == SDB_OK);
    corrupt_byte(40U);
    assert(sdb_superblock_store_read(test_path, &result) == SDB_OK);
    assert(result.valid_mirror_count == 1U);
    assert(result.selected_slot == 1U);
    assert(result.superblock.generation == 3U);
    corrupt_byte((uint64_t)SDB_SUPERBLOCK_SLOT_SIZE + 40U);
    assert(sdb_superblock_store_read(test_path, &result) == SDB_E_CORRUPT);
    cleanup();
}

static void test_newer_single_mirror_wins(void)
{
    sdb_superblock_v1 old_value = sample(4U);
    sdb_superblock_v1 new_value = sample(5U);
    sdb_superblock_read_result result;
    cleanup();
    assert(sdb_superblock_store_create(test_path, &old_value) == SDB_OK);
    write_slot(1U, &new_value);
    assert(sdb_superblock_store_read(test_path, &result) == SDB_OK);
    assert(result.selected_slot == 1U);
    assert(result.superblock.generation == 5U);
    cleanup();
}

static void test_same_generation_split_brain_is_rejected(void)
{
    sdb_superblock_v1 left = sample(6U);
    sdb_superblock_v1 right = sample(6U);
    sdb_superblock_read_result result;
    right.root_page = 99U;
    cleanup();
    assert(sdb_superblock_store_create(test_path, &left) == SDB_OK);
    write_slot(1U, &right);
    assert(sdb_superblock_store_read(test_path, &result) == SDB_E_CORRUPT);
    cleanup();
}

static void test_truncated_second_mirror_recovers(void)
{
    sdb_superblock_v1 value = sample(7U);
    sdb_superblock_read_result result;
    sdb_file file;
    cleanup();
    assert(sdb_superblock_store_create(test_path, &value) == SDB_OK);
    assert(sdb_file_open_existing(test_path, true, &file) == SDB_OK);
    assert(sdb_file_resize(&file, (uint64_t)SDB_SUPERBLOCK_SLOT_SIZE + 12U) == SDB_OK);
    assert(sdb_file_sync(&file) == SDB_OK);
    assert(sdb_file_close(&file) == SDB_OK);
    assert(sdb_superblock_store_read(test_path, &result) == SDB_OK);
    assert(result.valid_mirror_count == 1U);
    assert(result.selected_slot == 0U);
    cleanup();
}

static void test_update_failure_at_every_io_boundary_is_recoverable(void)
{
    size_t fail_after;
    for (fail_after = 0U; fail_after <= 6U; ++fail_after) {
        sdb_superblock_v1 old_value = sample(10U);
        sdb_superblock_v1 new_value = sample(11U);
        sdb_superblock_read_result result;
        sdb_file file;
        cleanup();
        assert(sdb_superblock_store_create(test_path, &old_value) == SDB_OK);
        assert(sdb_file_open_existing(test_path, true, &file) == SDB_OK);
        sdb_file_fail_after_for_testing(&file, fail_after);
        (void)sdb_superblock_store_update_file(&file, &new_value);
        sdb_file_clear_failure_for_testing(&file);
        assert(sdb_superblock_store_read_file(&file, &result) == SDB_OK);
        assert(
            result.superblock.generation == 10U
            || result.superblock.generation == 11U
        );
        assert(sdb_file_close(&file) == SDB_OK);
    }
    cleanup();
}

static void test_update_rejects_next_page_id_retreat(void)
{
    /*
     * S.4 pins immutable identity fields (file_id, salt, page_size) at
     * the update boundary. next_page_id is not identity, but it MUST be
     * monotonic-non-decreasing: it is the low-water-mark for pages the
     * DB has ever allocated. A caller that lowers it commits a
     * superblock whose file-size expectation is smaller than reality;
     * on the next open, sdb_pager_canonicalize_file_size will
     * ftruncate the tail off the DB — a durable data-loss primitive
     * reachable via the public update path.
     *
     * Reject at the update boundary so the failure has clear
     * attribution (mirrors the file_id/salt/page_size guard).
     */
    sdb_superblock_v1 old_value = sample(30U);
    sdb_superblock_v1 new_value = sample(31U);
    sdb_superblock_read_result result;
    cleanup();
    old_value.next_page_id = 100U;
    new_value.next_page_id = 42U; /* retreat — must be rejected */
    assert(sdb_superblock_store_create(test_path, &old_value) == SDB_OK);
    assert(
        sdb_superblock_store_update(test_path, &new_value)
        == SDB_E_INVALID_ARGUMENT
    );
    /* On-disk state must be untouched. */
    assert(sdb_superblock_store_read(test_path, &result) == SDB_OK);
    assert(result.superblock.generation == 30U);
    assert(result.superblock.next_page_id == 100U);
    cleanup();
}

static void test_update_accepts_next_page_id_equal(void)
{
    /* Equal (no allocations this txn) is legal; only retreat is not. */
    sdb_superblock_v1 old_value = sample(32U);
    sdb_superblock_v1 new_value = sample(33U);
    sdb_superblock_read_result result;
    cleanup();
    old_value.next_page_id = 50U;
    new_value.next_page_id = 50U;
    assert(sdb_superblock_store_create(test_path, &old_value) == SDB_OK);
    assert(sdb_superblock_store_update(test_path, &new_value) == SDB_OK);
    assert(sdb_superblock_store_read(test_path, &result) == SDB_OK);
    assert(result.superblock.generation == 33U);
    assert(result.superblock.next_page_id == 50U);
    cleanup();
}

static void test_partial_first_mirror_write_is_recoverable(void)
{
    sdb_superblock_v1 old_value = sample(20U);
    sdb_superblock_v1 new_value = sample(21U);
    sdb_superblock_read_result result;
    sdb_file file;
    cleanup();
    assert(sdb_superblock_store_create(test_path, &old_value) == SDB_OK);
    assert(sdb_file_open_existing(test_path, true, &file) == SDB_OK);
    /*
     * ceil(4096 / 17) = 241 syscalls per mirror read. Fail four chunks
     * into the first mirror write, before its 128-byte header and CRC are
     * complete, leaving a deterministic torn slot.
     */
    sdb_file_set_io_limit_for_testing(&file, 17U);
    sdb_file_fail_after_for_testing(&file, 486U);
    assert(
        sdb_superblock_store_update_file(&file, &new_value) == SDB_E_IO
    );
    sdb_file_clear_failure_for_testing(&file);
    assert(sdb_superblock_store_read_file(&file, &result) == SDB_OK);
    assert(result.superblock.generation == 20U);
    assert(result.valid_mirror_count == 1U);
    assert(sdb_file_close(&file) == SDB_OK);
    cleanup();
}

int main(void)
{
    test_create_update_and_reopen();
    test_one_corrupt_mirror_recovers();
    test_newer_single_mirror_wins();
    test_same_generation_split_brain_is_rejected();
    test_truncated_second_mirror_recovers();
    test_update_failure_at_every_io_boundary_is_recoverable();
    test_update_rejects_next_page_id_retreat();
    test_update_accepts_next_page_id_equal();
    test_partial_first_mirror_write_is_recoverable();
    (void)puts("superblock store tests: ok");
    return 0;
}
