#include "superblock_store.h"

#include "crypto.h"

#include <stdio.h>
#include <string.h>

#define SDB_SUPERBLOCK_AUTH_TAG_OFFSET SDB_SUPERBLOCK_HEADER_SIZE
#define SDB_SUPERBLOCK_AUTH_TAG_SIZE ((size_t)32)
#define SDB_SUPERBLOCK_AUTH_PADDING_OFFSET \
    (SDB_SUPERBLOCK_AUTH_TAG_OFFSET + SDB_SUPERBLOCK_AUTH_TAG_SIZE)

static uint64_t sdb_slot_offset(uint8_t slot)
{
    return (uint64_t)slot * (uint64_t)SDB_SUPERBLOCK_SLOT_SIZE;
}

static sdb_status sdb_encode_slot(
    const sdb_superblock_v1 *superblock,
    const uint8_t *data_key,
    uint8_t slot[SDB_SUPERBLOCK_SLOT_SIZE]
)
{
    sdb_status status = sdb_superblock_v1_encode(
        superblock, slot, SDB_SUPERBLOCK_SLOT_SIZE
    );
    if (status != SDB_OK) {
        return status;
    }
    if ((superblock->flags & SDB_FLAG_HEADER_AUTH) != 0U) {
        if (data_key == NULL) {
            return SDB_E_INVALID_ARGUMENT;
        }
        sdb_hmac_sha256(
            data_key,
            32U,
            slot,
            SDB_SUPERBLOCK_HEADER_SIZE,
            slot + SDB_SUPERBLOCK_AUTH_TAG_OFFSET
        );
    }
    return SDB_OK;
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

static bool sdb_slot_padding_is_zero(
    const uint8_t slot[SDB_SUPERBLOCK_SLOT_SIZE],
    const sdb_superblock_v1 *superblock
)
{
    const size_t start = (superblock->flags & SDB_FLAG_HEADER_AUTH) != 0U
        ? SDB_SUPERBLOCK_AUTH_PADDING_OFFSET
        : SDB_SUPERBLOCK_HEADER_SIZE;
    size_t index;
    for (index = start; index < SDB_SUPERBLOCK_SLOT_SIZE; ++index) {
        if (slot[index] != 0U) {
            return false;
        }
    }
    return true;
}

static sdb_status sdb_superblock_load_slots(
    sdb_file *file,
    uint8_t slot_bytes[2][SDB_SUPERBLOCK_SLOT_SIZE],
    sdb_superblock_v1 decoded[2],
    bool valid[2],
    uint8_t *valid_count_out
)
{
    bool io_failure = false;
    uint8_t slot;
    uint8_t valid_count = 0U;

    valid[0] = false;
    valid[1] = false;
    for (slot = 0U; slot < 2U; ++slot) {
        const sdb_status read_status = sdb_file_read_full(
            file,
            sdb_slot_offset(slot),
            slot_bytes[slot],
            SDB_SUPERBLOCK_SLOT_SIZE
        );
        if (read_status == SDB_E_IO) {
            io_failure = true;
        }
        if (read_status == SDB_OK
            && sdb_superblock_v1_decode(
                slot_bytes[slot], SDB_SUPERBLOCK_SLOT_SIZE, &decoded[slot]
            ) == SDB_OK
            && sdb_slot_padding_is_zero(slot_bytes[slot], &decoded[slot])) {
            valid[slot] = true;
            ++valid_count;
        }
    }
    *valid_count_out = valid_count;
    /*
     * A read I/O error on ONE slot (e.g. a bad sector) must not defeat the
     * two-slot redundancy: if the other slot decoded cleanly, report success
     * and let the caller select it. The I/O error is only fatal when NO slot
     * is usable. This mirrors the SDB_E_TRUNCATED path, which already leaves
     * io_failure unset and falls back to the intact mirror.
     */
    if (valid_count > 0U) {
        return SDB_OK;
    }
    return io_failure ? SDB_E_IO : SDB_OK;
}

static bool sdb_slot_auth_tag_matches(
    const uint8_t slot[SDB_SUPERBLOCK_SLOT_SIZE], const uint8_t data_key[32]
)
{
    uint8_t expected[SDB_SUPERBLOCK_AUTH_TAG_SIZE];
    bool matches;
    sdb_hmac_sha256(
        data_key, 32U, slot, SDB_SUPERBLOCK_HEADER_SIZE, expected
    );
    matches = sdb_constant_time_equal(
        expected,
        slot + SDB_SUPERBLOCK_AUTH_TAG_OFFSET,
        SDB_SUPERBLOCK_AUTH_TAG_SIZE
    );
    sdb_secure_zero(expected, sizeof(expected));
    return matches;
}

sdb_status sdb_superblock_store_read_file(
    sdb_file *file, sdb_superblock_read_result *result_out
)
{
    uint8_t slot_bytes[2][SDB_SUPERBLOCK_SLOT_SIZE];
    sdb_superblock_v1 decoded[2];
    bool valid[2];
    uint8_t valid_count = 0U;
    sdb_status status;

    if (file == NULL || result_out == NULL) {
        return SDB_E_INVALID_ARGUMENT;
    }
    status = sdb_superblock_load_slots(
        file, slot_bytes, decoded, valid, &valid_count
    );
    if (status != SDB_OK) {
        return status;
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
    result_out->format_version = sdb_read_u16_le(
        slot_bytes[result_out->selected_slot] + 8U
    );
    return SDB_OK;
}

sdb_status sdb_superblock_store_heal_file(
    sdb_file *file,
    const sdb_superblock_read_result *current,
    const uint8_t *data_key
)
{
    uint8_t encoded[SDB_SUPERBLOCK_SLOT_SIZE];
    uint8_t peer_bytes[SDB_SUPERBLOCK_SLOT_SIZE];
    uint8_t peer_slot;
    sdb_status status;
    if (file == NULL || current == NULL
        || current->valid_mirror_count == 0U
        || current->valid_mirror_count > SDB_SUPERBLOCK_SLOT_COUNT
        || current->selected_slot >= SDB_SUPERBLOCK_SLOT_COUNT) {
        return SDB_E_INVALID_ARGUMENT;
    }
    status = sdb_encode_slot(&current->superblock, data_key, encoded);
    if (status != SDB_OK) {
        return status;
    }
    peer_slot = current->selected_slot == 0U ? 1U : 0U;
    status = sdb_file_read_full(
        file, sdb_slot_offset(peer_slot), peer_bytes, sizeof(peer_bytes)
    );
    if (status == SDB_OK && memcmp(peer_bytes, encoded, sizeof(encoded)) == 0) {
        return SDB_OK;
    }
    if (status != SDB_OK && status != SDB_E_TRUNCATED) {
        return status;
    }
    status = sdb_file_write_full(
        file, sdb_slot_offset(peer_slot), encoded, sizeof(encoded)
    );
    if (status == SDB_OK) {
        status = sdb_file_sync(file);
    }
    return status;
}

sdb_status sdb_superblock_store_read_authenticated_file(
    sdb_file *file,
    const uint8_t data_key[32],
    sdb_superblock_read_result *result_out
)
{
    uint8_t slot_bytes[2][SDB_SUPERBLOCK_SLOT_SIZE];
    sdb_superblock_v1 decoded[2];
    bool valid[2];
    bool authed[2] = {false, false};
    uint8_t valid_count = 0U;
    uint8_t authed_count = 0U;
    uint8_t slot;
    uint8_t selected;
    sdb_status status;

    if (file == NULL || data_key == NULL || result_out == NULL) {
        return SDB_E_INVALID_ARGUMENT;
    }
    status = sdb_superblock_load_slots(
        file, slot_bytes, decoded, valid, &valid_count
    );
    if (status != SDB_OK) {
        sdb_secure_zero(slot_bytes, sizeof(slot_bytes));
        return status;
    }
    /*
     * Integrity-aware selection: a mirror only counts if it both decodes and
     * authenticates against data_key, verified on the very bytes read above so
     * no second read can race the selection. The newest generation among the
     * authenticated mirrors wins; a corrupt newer mirror therefore falls back
     * to an intact older one instead of failing the open.
     */
    for (slot = 0U; slot < 2U; ++slot) {
        if (valid[slot]
            && (decoded[slot].flags & SDB_FLAG_HEADER_AUTH) != 0U
            && sdb_slot_auth_tag_matches(slot_bytes[slot], data_key)) {
            authed[slot] = true;
            ++authed_count;
        }
    }
    if (authed_count == 0U) {
        sdb_secure_zero(slot_bytes, sizeof(slot_bytes));
        return SDB_E_CORRUPT;
    }
    if (authed_count == 2U
        && decoded[0].generation == decoded[1].generation
        && !sdb_superblocks_equal(&decoded[0], &decoded[1])) {
        sdb_secure_zero(slot_bytes, sizeof(slot_bytes));
        return SDB_E_CORRUPT;
    }

    selected = 0U;
    if (!authed[0]
        || (authed[1] && decoded[1].generation > decoded[0].generation)) {
        selected = 1U;
    }
    result_out->selected_slot = selected;
    result_out->valid_mirror_count = authed_count;
    result_out->superblock = decoded[selected];
    result_out->format_version = sdb_read_u16_le(slot_bytes[selected] + 8U);
    sdb_secure_zero(slot_bytes, sizeof(slot_bytes));
    return SDB_OK;
}

static sdb_status sdb_superblock_store_create_internal(
    const char *path,
    const sdb_superblock_v1 *superblock,
    const uint8_t *data_key
)
{
    sdb_file file;
    uint8_t slot[SDB_SUPERBLOCK_SLOT_SIZE];
    sdb_status status;
    sdb_status close_status;

    if (path == NULL || superblock == NULL) {
        return SDB_E_INVALID_ARGUMENT;
    }
    status = sdb_encode_slot(superblock, data_key, slot);
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
        status = sdb_file_sync(&file);
    }
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

        (void)remove(path);
        (void)sdb_file_sync_parent_directory(path);
    }
    return status != SDB_OK ? status : close_status;
}

sdb_status sdb_superblock_store_create(
    const char *path, const sdb_superblock_v1 *superblock
)
{
    return sdb_superblock_store_create_internal(path, superblock, NULL);
}

sdb_status sdb_superblock_store_create_authenticated(
    const char *path,
    const sdb_superblock_v1 *superblock,
    const uint8_t data_key[32]
)
{
    if (data_key == NULL
        || superblock == NULL
        || (superblock->flags & SDB_FLAG_HEADER_AUTH) == 0U) {
        return SDB_E_INVALID_ARGUMENT;
    }
    return sdb_superblock_store_create_internal(path, superblock, data_key);
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

static sdb_status sdb_superblock_store_update_file_internal(
    sdb_file *file,
    const sdb_superblock_v1 *superblock,
    const uint8_t *data_key
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
    status = sdb_encode_slot(superblock, data_key, encoded);
    if (status != SDB_OK) {
        return status;
    }
    status = sdb_superblock_store_read_file(file, &current);
    if (status == SDB_OK
        && superblock->generation <= current.superblock.generation) {
        status = SDB_E_INVALID_ARGUMENT;
    }

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

sdb_status sdb_superblock_store_update_file(
    sdb_file *file, const sdb_superblock_v1 *superblock
)
{
    return sdb_superblock_store_update_file_internal(file, superblock, NULL);
}

sdb_status sdb_superblock_store_update_file_authenticated(
    sdb_file *file,
    const sdb_superblock_v1 *superblock,
    const uint8_t data_key[32]
)
{
    if (data_key == NULL
        || superblock == NULL
        || (superblock->flags & SDB_FLAG_HEADER_AUTH) == 0U) {
        return SDB_E_INVALID_ARGUMENT;
    }
    return sdb_superblock_store_update_file_internal(
        file, superblock, data_key
    );
}
