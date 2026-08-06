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

static const uint8_t auth_key[32] = {
    0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
    0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00,
    0x0F, 0x1E, 0x2D, 0x3C, 0x4B, 0x5A, 0x69, 0x78,
    0x87, 0x96, 0xA5, 0xB4, 0xC3, 0xD2, 0xE1, 0xF0
};

static sdb_superblock_v1 sample_auth(uint64_t generation)
{
    sdb_superblock_v1 value;
    size_t index;
    (void)memset(&value, 0, sizeof(value));
    value.page_size = 4096U;
    value.flags = SDB_FLAG_ENCRYPTED | SDB_FLAG_HEADER_AUTH;
    value.generation = generation;
    value.checkpoint_lsn = generation * UINT64_C(10);
    value.root_page = 2U;
    value.next_page_id = 3U;
    value.key_wrap_id = 1U;
    value.kdf_iterations = SDB_MIN_KDF_ITERATIONS;
    for (index = 0U; index < SDB_SALT_SIZE; ++index) {
        value.salt[index] = (uint8_t)(index + 1U);
        value.file_id[index] = (uint8_t)(0x80U + index);
    }
    for (index = 0U; index < SDB_WRAPPED_KEY_SIZE; ++index) {
        value.wrapped_key[index] = (uint8_t)(index + 3U);
    }
    for (index = 0U; index < SDB_KEY_WRAP_TAG_SIZE; ++index) {
        value.key_wrap_tag[index] = (uint8_t)(index + 7U);
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

static void test_one_corrupt_mirror_heals(void)
{
    sdb_superblock_v1 value = sample(13U);
    sdb_superblock_read_result result;
    sdb_file file;
    cleanup();
    assert(sdb_superblock_store_create(test_path, &value) == SDB_OK);
    corrupt_byte(40U);
    assert(sdb_file_open_existing(test_path, true, &file) == SDB_OK);
    assert(sdb_superblock_store_read_file(&file, &result) == SDB_OK);
    assert(result.valid_mirror_count == 1U);
    assert(result.selected_slot == 1U);
    assert(sdb_superblock_store_heal_file(&file, &result, NULL) == SDB_OK);
    assert(sdb_superblock_store_read_file(&file, &result) == SDB_OK);
    assert(result.valid_mirror_count == 2U);
    assert(result.superblock.generation == 13U);
    assert(sdb_file_close(&file) == SDB_OK);
    cleanup();
}

static void test_heal_failure_preserves_good_mirror(void)
{
    sdb_superblock_v1 value = sample(14U);
    sdb_superblock_read_result result;
    sdb_file file;
    cleanup();
    assert(sdb_superblock_store_create(test_path, &value) == SDB_OK);
    corrupt_byte((uint64_t)SDB_SUPERBLOCK_SLOT_SIZE + 40U);
    assert(sdb_file_open_existing(test_path, true, &file) == SDB_OK);
    assert(sdb_superblock_store_read_file(&file, &result) == SDB_OK);
    assert(result.valid_mirror_count == 1U);
    assert(result.selected_slot == 0U);
    /* op0 reads the damaged peer; op1 is the healing write. */
    sdb_file_fail_after_for_testing(&file, 1U);
    assert(
        sdb_superblock_store_heal_file(&file, &result, NULL) == SDB_E_IO
    );
    sdb_file_clear_failure_for_testing(&file);
    assert(sdb_superblock_store_read_file(&file, &result) == SDB_OK);
    assert(result.valid_mirror_count == 1U);
    assert(result.selected_slot == 0U);
    assert(result.superblock.generation == 14U);
    assert(sdb_file_close(&file) == SDB_OK);
    cleanup();
}

static void test_older_valid_mirror_heals(void)
{
    sdb_superblock_v1 old_value = sample(15U);
    sdb_superblock_v1 new_value = sample(16U);
    sdb_superblock_read_result result;
    sdb_file file;
    cleanup();
    assert(sdb_superblock_store_create(test_path, &old_value) == SDB_OK);
    write_slot(1U, &new_value);
    assert(sdb_file_open_existing(test_path, true, &file) == SDB_OK);
    assert(sdb_superblock_store_read_file(&file, &result) == SDB_OK);
    assert(result.valid_mirror_count == 2U);
    assert(result.selected_slot == 1U);
    assert(sdb_superblock_store_heal_file(&file, &result, NULL) == SDB_OK);
    assert(sdb_superblock_store_read_file(&file, &result) == SDB_OK);
    assert(result.valid_mirror_count == 2U);
    assert(result.superblock.generation == 16U);
    assert(result.selected_slot == 0U);
    assert(sdb_file_close(&file) == SDB_OK);
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
    right.checkpoint_lsn ^= UINT64_C(0xABCD);
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
        sdb_status update_status;
        uint64_t expected_generation;
        cleanup();
        assert(sdb_superblock_store_create(test_path, &old_value) == SDB_OK);
        assert(sdb_file_open_existing(test_path, true, &file) == SDB_OK);
        sdb_file_fail_after_for_testing(&file, fail_after);
        update_status = sdb_superblock_store_update_file(&file, &new_value);
        sdb_file_clear_failure_for_testing(&file);
        /*
         * update_file issues a fixed op sequence on this handle
         * (io_limit == SIZE_MAX, so each slot read/write and each fsync is a
         * single injectable op):
         *   op0 read slot0, op1 read slot1, op2 write first slot,
         *   op3 fsync, op4 write second slot, op5 fsync.
         * The new generation only reaches disk once op2 (the first slot write)
         * succeeds, i.e. fail_after >= 3. All 6 ops succeed at fail_after == 6.
         */
        expected_generation = fail_after >= 3U ? 11U : 10U;
        if (fail_after >= 6U) {
            assert(update_status == SDB_OK);
        } else {
            assert(update_status != SDB_OK);
        }
        assert(sdb_superblock_store_read_file(&file, &result) == SDB_OK);
        assert(result.superblock.generation == expected_generation);
        assert(sdb_file_close(&file) == SDB_OK);
    }
    cleanup();
}

static void test_newer_slot0_selected_by_generation(void)
{
    sdb_superblock_v1 newer = sample(9U);
    sdb_superblock_v1 older = sample(8U);
    sdb_superblock_read_result result;
    cleanup();
    /*
     * create writes gen 9 to BOTH slots; then slot 1 is overwritten with an
     * OLDER generation. Selection must follow the generation comparison
     * (slot 0 wins) rather than write-recency (slot 1 was written last). This
     * is the mirror image of test_newer_single_mirror_wins, pinning the
     * comparison from both directions.
     */
    assert(sdb_superblock_store_create(test_path, &newer) == SDB_OK);
    write_slot(1U, &older);
    assert(sdb_superblock_store_read(test_path, &result) == SDB_OK);
    assert(result.valid_mirror_count == 2U);
    assert(result.selected_slot == 0U);
    assert(result.superblock.generation == 9U);
    cleanup();
}

static void test_update_rejects_next_page_id_retreat(void)
{

    sdb_superblock_v1 old_value = sample(30U);
    sdb_superblock_v1 new_value = sample(31U);
    sdb_superblock_read_result result;
    cleanup();
    old_value.next_page_id = 100U;
    new_value.next_page_id = 42U;
    assert(sdb_superblock_store_create(test_path, &old_value) == SDB_OK);
    assert(
        sdb_superblock_store_update(test_path, &new_value)
        == SDB_E_INVALID_ARGUMENT
    );

    assert(sdb_superblock_store_read(test_path, &result) == SDB_OK);
    assert(result.superblock.generation == 30U);
    assert(result.superblock.next_page_id == 100U);
    cleanup();
}

static void test_update_accepts_next_page_id_equal(void)
{

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

/*
 * Build an authenticated store whose two mirrors are at DIFFERENT generations
 * and both carry a valid HMAC tag: slot1 holds the newer generation, slot0 the
 * older one. This is reached by starting both slots at the older generation and
 * failing the update after the first (peer) slot write reaches disk but before
 * the selected slot is rewritten -- exactly the sequence pinned by
 * test_update_failure_at_every_io_boundary_is_recoverable.
 */
static void build_split_generation_authenticated(void)
{
    sdb_superblock_v1 older = sample_auth(40U);
    sdb_superblock_v1 newer = sample_auth(41U);
    sdb_file file;
    cleanup();
    assert(
        sdb_superblock_store_create_authenticated(test_path, &older, auth_key)
        == SDB_OK
    );
    assert(sdb_file_open_existing(test_path, true, &file) == SDB_OK);
    /* op0 read slot0, op1 read slot1, op2 write slot1 (peer), op3 fsync. */
    sdb_file_fail_after_for_testing(&file, 3U);
    assert(
        sdb_superblock_store_update_file_authenticated(
            &file, &newer, auth_key
        ) == SDB_E_IO
    );
    sdb_file_clear_failure_for_testing(&file);
    assert(sdb_file_close(&file) == SDB_OK);
}

static void test_authenticated_high_gen_tag_flip_falls_back(void)
{
    sdb_superblock_read_result result;
    sdb_file file;
    build_split_generation_authenticated();
    /* Keyless selection still picks the newer slot1 (generation wins). */
    assert(sdb_file_open_existing(test_path, false, &file) == SDB_OK);
    assert(sdb_superblock_store_read_file(&file, &result) == SDB_OK);
    assert(result.selected_slot == 1U);
    assert(result.superblock.generation == 41U);
    assert(sdb_file_close(&file) == SDB_OK);
    /* Flip one byte inside the newer slot's HMAC tag region [160, 192). */
    corrupt_byte(
        (uint64_t)SDB_SUPERBLOCK_SLOT_SIZE + SDB_SUPERBLOCK_HEADER_SIZE + 5U
    );
    /*
     * Integrity-aware selection must reject the newer-but-corrupt mirror and
     * fall back to the intact older mirror rather than surfacing CORRUPT.
     */
    assert(sdb_file_open_existing(test_path, false, &file) == SDB_OK);
    assert(
        sdb_superblock_store_read_authenticated_file(&file, auth_key, &result)
        == SDB_OK
    );
    assert(result.selected_slot == 0U);
    assert(result.superblock.generation == 40U);
    assert(result.valid_mirror_count == 1U);
    assert(sdb_file_close(&file) == SDB_OK);
    cleanup();
}

static void test_authenticated_both_tags_flipped_is_corrupt(void)
{
    sdb_superblock_read_result result;
    sdb_file file;
    build_split_generation_authenticated();
    corrupt_byte(SDB_SUPERBLOCK_HEADER_SIZE + 5U);
    corrupt_byte(
        (uint64_t)SDB_SUPERBLOCK_SLOT_SIZE + SDB_SUPERBLOCK_HEADER_SIZE + 5U
    );
    assert(sdb_file_open_existing(test_path, false, &file) == SDB_OK);
    assert(
        sdb_superblock_store_read_authenticated_file(&file, auth_key, &result)
        == SDB_E_CORRUPT
    );
    assert(sdb_file_close(&file) == SDB_OK);
    cleanup();
}

static void test_authenticated_both_intact_selects_higher_gen(void)
{
    sdb_superblock_v1 older = sample_auth(50U);
    sdb_superblock_v1 newer = sample_auth(51U);
    sdb_superblock_read_result result;
    sdb_file file;
    cleanup();
    assert(
        sdb_superblock_store_create_authenticated(test_path, &older, auth_key)
        == SDB_OK
    );
    assert(sdb_file_open_existing(test_path, true, &file) == SDB_OK);
    assert(
        sdb_superblock_store_update_file_authenticated(
            &file, &newer, auth_key
        ) == SDB_OK
    );
    assert(
        sdb_superblock_store_read_authenticated_file(&file, auth_key, &result)
        == SDB_OK
    );
    assert(result.valid_mirror_count == 2U);
    assert(result.superblock.generation == 51U);
    assert(sdb_file_close(&file) == SDB_OK);
    cleanup();
}

static void test_authenticated_read_rejects_null_key(void)
{
    sdb_superblock_v1 value = sample_auth(60U);
    sdb_superblock_read_result result;
    sdb_file file;
    cleanup();
    assert(
        sdb_superblock_store_create_authenticated(test_path, &value, auth_key)
        == SDB_OK
    );
    assert(sdb_file_open_existing(test_path, false, &file) == SDB_OK);
    assert(
        sdb_superblock_store_read_authenticated_file(&file, NULL, &result)
        == SDB_E_INVALID_ARGUMENT
    );
    assert(sdb_file_close(&file) == SDB_OK);
    cleanup();
}

int main(void)
{
    test_create_update_and_reopen();
    test_one_corrupt_mirror_recovers();
    test_one_corrupt_mirror_heals();
    test_heal_failure_preserves_good_mirror();
    test_older_valid_mirror_heals();
    test_newer_single_mirror_wins();
    test_newer_slot0_selected_by_generation();
    test_same_generation_split_brain_is_rejected();
    test_truncated_second_mirror_recovers();
    test_update_failure_at_every_io_boundary_is_recoverable();
    test_update_rejects_next_page_id_retreat();
    test_update_accepts_next_page_id_equal();
    test_partial_first_mirror_write_is_recoverable();
    test_authenticated_high_gen_tag_flip_falls_back();
    test_authenticated_both_tags_flipped_is_corrupt();
    test_authenticated_both_intact_selects_higher_gen();
    test_authenticated_read_rejects_null_key();
    (void)puts("superblock store tests: ok");
    return 0;
}
