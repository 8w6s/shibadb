#include "superblock_store.h"

#include <stdio.h>
#include <string.h>

static uint64_t sdb_slot_offset(uint8_t slot)
{
    return (uint64_t)slot * (uint64_t)SDB_SUPERBLOCK_SLOT_SIZE;
}

static sdb_status sdb_encode_slot(
    const sdb_superblock_v1 *superblock,
    uint8_t slot[SDB_SUPERBLOCK_SLOT_SIZE]
)
{
    return sdb_superblock_v1_encode(superblock, slot, SDB_SUPERBLOCK_SLOT_SIZE);
}

static bool sdb_superblocks_equal(
    const sdb_superblock_v1 *left, const sdb_superblock_v1 *right
)
{
    return left->page_size == right->page_size
        && left->flags == right->flags
        && left->generation == right->generation
        && left->checkpoint_lsn == right->checkpoint_lsn
        && left->root_page == right->root_page
        && left->freelist_page == right->freelist_page
        && left->next_page_id == right->next_page_id
        && left->key_wrap_id == right->key_wrap_id
        && left->kdf_iterations == right->kdf_iterations
        && memcmp(left->salt, right->salt, SDB_SALT_SIZE) == 0
        && memcmp(left->file_id, right->file_id, SDB_FILE_ID_SIZE) == 0
        && memcmp(
            left->wrapped_key, right->wrapped_key, SDB_WRAPPED_KEY_SIZE
        ) == 0
        && memcmp(
            left->key_wrap_tag, right->key_wrap_tag, SDB_KEY_WRAP_TAG_SIZE
        ) == 0;
}

static bool sdb_slot_padding_is_zero(const uint8_t slot[SDB_SUPERBLOCK_SLOT_SIZE])
{
    size_t index;
    for (index = SDB_SUPERBLOCK_HEADER_SIZE;
         index < SDB_SUPERBLOCK_SLOT_SIZE;
         ++index) {
        if (slot[index] != 0U) {
            return false;
        }
    }
    return true;
}

sdb_status sdb_superblock_store_read_file(
    sdb_file *file, sdb_superblock_read_result *result_out
)
{
    uint8_t slot_bytes[2][SDB_SUPERBLOCK_SLOT_SIZE];
    sdb_superblock_v1 decoded[2];
    bool valid[2] = {false, false};
    uint8_t slot;
    uint8_t valid_count = 0U;

    if (file == NULL || result_out == NULL) {
        return SDB_E_INVALID_ARGUMENT;
    }
    for (slot = 0U; slot < 2U; ++slot) {
        const sdb_status read_status = sdb_file_read_full(
            file,
            sdb_slot_offset(slot),
            slot_bytes[slot],
            SDB_SUPERBLOCK_SLOT_SIZE
        );
        if (read_status == SDB_OK
            && sdb_slot_padding_is_zero(slot_bytes[slot])
            && sdb_superblock_v1_decode(
                slot_bytes[slot], SDB_SUPERBLOCK_SLOT_SIZE, &decoded[slot]
            ) == SDB_OK) {
            valid[slot] = true;
            ++valid_count;
        }
    }
    if (valid_count == 0U) {
        return SDB_E_CORRUPT;
    }
    if (valid_count == 2U
        && decoded[0].generation == decoded[1].generation
        && !sdb_superblocks_equal(&decoded[0], &decoded[1])) {
        return SDB_E_CORRUPT;
    }

    result_out->selected_slot = 0U;
    if (!valid[0]
        || (valid[1] && decoded[1].generation > decoded[0].generation)) {
        result_out->selected_slot = 1U;
    }
    result_out->valid_mirror_count = valid_count;
    result_out->superblock = decoded[result_out->selected_slot];
    return SDB_OK;
}

sdb_status sdb_superblock_store_create(
    const char *path,
    const sdb_superblock_v1 *superblock
)
{
    sdb_file file;
    uint8_t slot[SDB_SUPERBLOCK_SLOT_SIZE];
    sdb_status status;
    sdb_status close_status;

    if (path == NULL || superblock == NULL) {
        return SDB_E_INVALID_ARGUMENT;
    }
    status = sdb_encode_slot(superblock, slot);
    if (status != SDB_OK) {
        return status;
    }
    status = sdb_file_create_new(path, &file);
    if (status != SDB_OK) {
        return status;
    }
    status = sdb_file_resize(
        &file, (uint64_t)(SDB_SUPERBLOCK_SLOT_SIZE * SDB_SUPERBLOCK_SLOT_COUNT)
    );
    if (status == SDB_OK) {
        status = sdb_file_write_full(&file, sdb_slot_offset(0U), slot, sizeof(slot));
    }
    if (status == SDB_OK) {
        status = sdb_file_sync(&file);
    }
    if (status == SDB_OK) {
        status = sdb_file_write_full(&file, sdb_slot_offset(1U), slot, sizeof(slot));
    }
    if (status == SDB_OK) {
        status = sdb_file_sync(&file);
    }
    close_status = sdb_file_close(&file);
    if (status == SDB_OK && close_status == SDB_OK) {
        status = sdb_file_sync_parent_directory(path);
    } else {
        /*
         * O_EXCL proved this invocation created the path, so cleanup cannot
         * delete a pre-existing user file. A crash may still leave a partial
         * artifact; readers reject it because neither mirror validates.
         */
        (void)remove(path);
        (void)sdb_file_sync_parent_directory(path);
    }
    return status != SDB_OK ? status : close_status;
}

sdb_status sdb_superblock_store_read(
    const char *path,
    sdb_superblock_read_result *result_out
)
{
    sdb_file file;
    sdb_status status;
    sdb_status close_status;
    if (path == NULL || result_out == NULL) {
        return SDB_E_INVALID_ARGUMENT;
    }
    status = sdb_file_open_existing(path, false, &file);
    if (status != SDB_OK) {
        return status;
    }
    status = sdb_superblock_store_read_file(&file, result_out);
    close_status = sdb_file_close(&file);
    return status != SDB_OK ? status : close_status;
}

sdb_status sdb_superblock_store_update(
    const char *path,
    const sdb_superblock_v1 *superblock
)
{
    sdb_file file;
    sdb_status status;
    sdb_status close_status;

    if (path == NULL || superblock == NULL) {
        return SDB_E_INVALID_ARGUMENT;
    }
    status = sdb_file_open_existing(path, true, &file);
    if (status != SDB_OK) {
        return status;
    }
    status = sdb_superblock_store_update_file(&file, superblock);
    close_status = sdb_file_close(&file);
    return status != SDB_OK ? status : close_status;
}

sdb_status sdb_superblock_store_update_file(
    sdb_file *file,
    const sdb_superblock_v1 *superblock
)
{
    sdb_superblock_read_result current;
    uint8_t encoded[SDB_SUPERBLOCK_SLOT_SIZE];
    uint8_t first_slot = 0U;
    uint8_t second_slot = 1U;
    sdb_status status;

    if (file == NULL || superblock == NULL) {
        return SDB_E_INVALID_ARGUMENT;
    }
    status = sdb_encode_slot(superblock, encoded);
    if (status != SDB_OK) {
        return status;
    }
    status = sdb_superblock_store_read_file(file, &current);
    if (status == SDB_OK
        && superblock->generation <= current.superblock.generation) {
        status = SDB_E_INVALID_ARGUMENT;
    }
    /*
     * Enforce S.4 at the update boundary: file_id, salt, and page_size are
     * fixed at create-time. A buggy or malicious caller that mutates them
     * here would silently break WAL binding, encrypted-page AAD, and
     * page-size arithmetic; the resulting failures would surface far
     * downstream as opaque AEAD/CRC errors. Reject here so attribution is
     * clear.
     *
     * next_page_id is not an identity field, but it MUST be monotonic
     * non-decreasing: it is the low-water-mark for pages this database
     * has ever allocated. If a caller commits a superblock whose
     * next_page_id is smaller than the current on-disk value, the next
     * open will canonicalize the file size against the smaller number
     * and ftruncate the tail off — a durable data-loss primitive
     * reachable via the public update path. Reject retreats here for
     * the same attribution reason.
     */
    if (status == SDB_OK
        && (memcmp(
                superblock->file_id, current.superblock.file_id,
                SDB_FILE_ID_SIZE
            ) != 0
            || memcmp(
                superblock->salt, current.superblock.salt, SDB_SALT_SIZE
            ) != 0
            || superblock->page_size != current.superblock.page_size
            || superblock->next_page_id < current.superblock.next_page_id
            || (superblock->flags & SDB_FLAG_ENCRYPTED)
                != (current.superblock.flags & SDB_FLAG_ENCRYPTED))) {
        status = SDB_E_INVALID_ARGUMENT;
    }
    if (status == SDB_OK) {
        first_slot = current.selected_slot == 0U ? 1U : 0U;
        second_slot = current.selected_slot;
        status = sdb_file_write_full(
            file, sdb_slot_offset(first_slot), encoded, sizeof(encoded)
        );
    }
    if (status == SDB_OK) {
        status = sdb_file_sync(file);
    }
    if (status == SDB_OK) {
        status = sdb_file_write_full(
            file, sdb_slot_offset(second_slot), encoded, sizeof(encoded)
        );
    }
    if (status == SDB_OK) {
        status = sdb_file_sync(file);
    }
    return status;
}
