#include "internal.h"

#include <string.h>

const uint8_t sdb_superblock_magic[SDB_SUPERBLOCK_MAGIC_SIZE] = {
    (uint8_t)'S', (uint8_t)'H', (uint8_t)'I', (uint8_t)'B',
    (uint8_t)'A', (uint8_t)'C', (uint8_t)'0', (uint8_t)'1'
};

static bool sdb_is_power_of_two_u32(uint32_t value)
{
    return value != 0U && (value & (value - 1U)) == 0U;
}

static bool sdb_bytes_all_zero(const uint8_t *bytes, size_t size)
{
    uint8_t aggregate = 0U;
    size_t index;
    for (index = 0U; index < size; ++index) {
        aggregate = (uint8_t)(aggregate | bytes[index]);
    }
    return aggregate == 0U;
}

typedef enum {
    SDB_SB_FIELD_OK = 0,
    SDB_SB_FIELD_NULL_ARG,
    SDB_SB_FIELD_PAGE_SIZE,
    SDB_SB_FIELD_FLAGS,
    SDB_SB_FIELD_NEXT_PAGE_ID,
    SDB_SB_FIELD_FILE_ID,
    SDB_SB_FIELD_SALT,
    SDB_SB_FIELD_KEY_WRAP_ID,
    SDB_SB_FIELD_KDF_ITERATIONS,
    SDB_SB_FIELD_WRAPPED_KEY,
    SDB_SB_FIELD_KEY_WRAP_TAG,
    SDB_SB_FIELD_ENC_MISMATCH,
    SDB_SB_FIELD_ROOT_PAGE,
    SDB_SB_FIELD_FREELIST_PAGE
} sdb_sb_field;

static sdb_sb_field sdb_validate_superblock_detailed(
    const sdb_superblock_v1 *superblock
)
{
    if (superblock == NULL) {
        return SDB_SB_FIELD_NULL_ARG;
    }
    if (superblock->page_size < SDB_MIN_PAGE_SIZE
        || superblock->page_size > SDB_MAX_PAGE_SIZE
        || !sdb_is_power_of_two_u32(superblock->page_size)) {
        return SDB_SB_FIELD_PAGE_SIZE;
    }
    if ((superblock->flags
            & ~(SDB_FLAG_ENCRYPTED | SDB_FLAG_HEADER_AUTH)) != 0U
        || ((superblock->flags & SDB_FLAG_HEADER_AUTH) != 0U
            && (superblock->flags & SDB_FLAG_ENCRYPTED) == 0U)) {
        return SDB_SB_FIELD_FLAGS;
    }
    if (superblock->next_page_id == 0U) {
        return SDB_SB_FIELD_NEXT_PAGE_ID;
    }
    if (superblock->root_page >= superblock->next_page_id) {
        return SDB_SB_FIELD_ROOT_PAGE;
    }
    if (superblock->freelist_page >= superblock->next_page_id) {
        return SDB_SB_FIELD_FREELIST_PAGE;
    }
    if (sdb_bytes_all_zero(superblock->file_id, SDB_FILE_ID_SIZE)) {
        return SDB_SB_FIELD_FILE_ID;
    }

    if ((superblock->flags & SDB_FLAG_ENCRYPTED) != 0U
        && sdb_bytes_all_zero(superblock->salt, SDB_SALT_SIZE)) {
        return SDB_SB_FIELD_SALT;
    }
    if ((superblock->flags & SDB_FLAG_ENCRYPTED) != 0U) {
        if (superblock->key_wrap_id == 0U) {
            return SDB_SB_FIELD_KEY_WRAP_ID;
        }
        if (superblock->kdf_iterations < SDB_MIN_KDF_ITERATIONS
            || superblock->kdf_iterations > SDB_MAX_KDF_ITERATIONS) {
            return SDB_SB_FIELD_KDF_ITERATIONS;
        }
        if (sdb_bytes_all_zero(
                superblock->wrapped_key, SDB_WRAPPED_KEY_SIZE
            )) {
            return SDB_SB_FIELD_WRAPPED_KEY;
        }
        if (sdb_bytes_all_zero(
                superblock->key_wrap_tag, SDB_KEY_WRAP_TAG_SIZE
            )) {
            return SDB_SB_FIELD_KEY_WRAP_TAG;
        }
    } else if (superblock->key_wrap_id != 0U
        || superblock->kdf_iterations != 0U
        || !sdb_bytes_all_zero(
            superblock->wrapped_key, SDB_WRAPPED_KEY_SIZE
        )
        || !sdb_bytes_all_zero(
            superblock->key_wrap_tag, SDB_KEY_WRAP_TAG_SIZE
        )) {
        return SDB_SB_FIELD_ENC_MISMATCH;
    }
    return SDB_SB_FIELD_OK;
}

static sdb_status sdb_validate_superblock(const sdb_superblock_v1 *superblock)
{
    const sdb_sb_field field = sdb_validate_superblock_detailed(superblock);
    return field == SDB_SB_FIELD_OK ? SDB_OK : SDB_E_INVALID_ARGUMENT;
}

#if SDB_TESTING

sdb_sb_field sdb_validate_superblock_field(const sdb_superblock_v1 *sb);
sdb_sb_field sdb_validate_superblock_field(const sdb_superblock_v1 *sb)
{
    return sdb_validate_superblock_detailed(sb);
}
#endif

sdb_status sdb_superblock_v1_encode(
    const sdb_superblock_v1 *superblock,
    uint8_t *output,
    size_t output_size
)
{
    uint32_t checksum;
    sdb_status status = sdb_validate_superblock(superblock);
    if (status != SDB_OK) {
        return status;
    }
    if (output == NULL) {
        return SDB_E_INVALID_ARGUMENT;
    }
    if (output_size < SDB_SUPERBLOCK_HEADER_SIZE) {
        return SDB_E_BUFFER_TOO_SMALL;
    }

    (void)memset(output, 0, output_size);
    (void)memcpy(output, sdb_superblock_magic, SDB_SUPERBLOCK_MAGIC_SIZE);
    sdb_write_u16_le(output + 8U, SDB_FORMAT_VERSION_CURRENT);
    sdb_write_u16_le(output + 10U, (uint16_t)SDB_SUPERBLOCK_HEADER_SIZE);
    sdb_write_u32_le(output + 12U, superblock->page_size);
    sdb_write_u32_le(output + 16U, superblock->flags);
    sdb_write_u64_le(output + 24U, superblock->generation);
    sdb_write_u64_le(output + 32U, superblock->checkpoint_lsn);
    sdb_write_u64_le(output + 40U, superblock->root_page);
    sdb_write_u64_le(output + 48U, superblock->freelist_page);
    (void)memcpy(output + 56U, superblock->salt, SDB_SALT_SIZE);
    (void)memcpy(output + 72U, superblock->file_id, SDB_FILE_ID_SIZE);
    sdb_write_u64_le(output + 92U, superblock->next_page_id);
    sdb_write_u32_le(output + 100U, superblock->key_wrap_id);
    sdb_write_u32_le(output + 104U, superblock->kdf_iterations);
    (void)memcpy(
        output + 108U, superblock->wrapped_key, SDB_WRAPPED_KEY_SIZE
    );
    (void)memcpy(
        output + 140U, superblock->key_wrap_tag, SDB_KEY_WRAP_TAG_SIZE
    );

    checksum = sdb_crc32(output, SDB_SUPERBLOCK_HEADER_SIZE);
    sdb_write_u32_le(output + SDB_SUPERBLOCK_CHECKSUM_OFFSET, checksum);
    return SDB_OK;
}

sdb_status sdb_superblock_v1_decode(
    const uint8_t *input,
    size_t input_size,
    sdb_superblock_v1 *superblock_out
)
{
    uint8_t header[SDB_SUPERBLOCK_HEADER_SIZE];
    uint32_t stored_checksum;
    uint32_t computed_checksum;
    uint16_t format_version;
    sdb_superblock_v1 decoded;
    size_t index;

    if (input == NULL || superblock_out == NULL) {
        return SDB_E_INVALID_ARGUMENT;
    }
    if (input_size < SDB_SUPERBLOCK_HEADER_SIZE) {
        return SDB_E_BUFFER_TOO_SMALL;
    }
    if (memcmp(input, sdb_superblock_magic, SDB_SUPERBLOCK_MAGIC_SIZE) != 0) {
        return SDB_E_BAD_MAGIC;
    }
    format_version = sdb_read_u16_le(input + 8U);
    if (format_version != SDB_FORMAT_VERSION_V1
        && format_version != SDB_FORMAT_VERSION_V2) {
        return SDB_E_UNSUPPORTED_VERSION;
    }
    if (sdb_read_u16_le(input + 10U) != (uint16_t)SDB_SUPERBLOCK_HEADER_SIZE) {
        return SDB_E_CORRUPT;
    }

    (void)memcpy(header, input, SDB_SUPERBLOCK_HEADER_SIZE);
    stored_checksum = sdb_read_u32_le(header + SDB_SUPERBLOCK_CHECKSUM_OFFSET);
    (void)memset(
        header + SDB_SUPERBLOCK_CHECKSUM_OFFSET, 0,
        SDB_SUPERBLOCK_CHECKSUM_SIZE
    );
    computed_checksum = sdb_crc32(header, SDB_SUPERBLOCK_HEADER_SIZE);
    if (stored_checksum != computed_checksum) {
        return SDB_E_CORRUPT;
    }

    for (index = 20U; index < 24U; ++index) {
        if (input[index] != 0U) {
            return SDB_E_CORRUPT;
        }
    }
    for (index = 156U; index < SDB_SUPERBLOCK_HEADER_SIZE; ++index) {
        if (input[index] != 0U) {
            return SDB_E_CORRUPT;
        }
    }

    (void)memset(&decoded, 0, sizeof(decoded));
    decoded.page_size = sdb_read_u32_le(input + 12U);
    decoded.flags = sdb_read_u32_le(input + 16U);
    decoded.generation = sdb_read_u64_le(input + 24U);
    decoded.checkpoint_lsn = sdb_read_u64_le(input + 32U);
    decoded.root_page = sdb_read_u64_le(input + 40U);
    decoded.freelist_page = sdb_read_u64_le(input + 48U);
    decoded.next_page_id = sdb_read_u64_le(input + 92U);
    decoded.key_wrap_id = sdb_read_u32_le(input + 100U);
    decoded.kdf_iterations = sdb_read_u32_le(input + 104U);
    (void)memcpy(decoded.salt, input + 56U, SDB_SALT_SIZE);
    (void)memcpy(decoded.file_id, input + 72U, SDB_FILE_ID_SIZE);
    (void)memcpy(
        decoded.wrapped_key, input + 108U, SDB_WRAPPED_KEY_SIZE
    );
    (void)memcpy(
        decoded.key_wrap_tag, input + 140U, SDB_KEY_WRAP_TAG_SIZE
    );

    if (sdb_validate_superblock(&decoded) != SDB_OK) {
        return SDB_E_CORRUPT;
    }
    *superblock_out = decoded;
    return SDB_OK;
}
