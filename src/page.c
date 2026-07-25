#include "page.h"

#include "internal.h"

#include <string.h>

#define SDB_PAGE_CHECKSUM_OFFSET ((size_t)28)

static const uint8_t sdb_page_magic[4] = {
    (uint8_t)'S', (uint8_t)'P', (uint8_t)'G', (uint8_t)'1'
};

static bool sdb_page_type_valid(uint16_t type)
{
    return type == (uint16_t)SDB_PAGE_TYPE_DATA
        || type == (uint16_t)SDB_PAGE_TYPE_FREE
        || type == (uint16_t)SDB_PAGE_TYPE_ENCRYPTED;
}

sdb_status sdb_page_encode(
    uint8_t *page,
    size_t page_size,
    uint16_t type,
    uint64_t page_id,
    uint64_t page_lsn,
    const uint8_t *payload,
    size_t payload_size
)
{
    uint32_t checksum;
    if (page == NULL || page_id == 0U || !sdb_page_type_valid(type)) {
        return SDB_E_INVALID_ARGUMENT;
    }
    if (page_size < SDB_MIN_PAGE_SIZE || page_size > SDB_MAX_PAGE_SIZE
        || (page_size & (page_size - 1U)) != 0U) {
        return SDB_E_INVALID_ARGUMENT;
    }
    if (payload_size > page_size - SDB_PAGE_HEADER_SIZE
        || (payload == NULL && payload_size != 0U)
        || payload_size > (size_t)UINT32_MAX) {
        return SDB_E_INVALID_ARGUMENT;
    }
    (void)memset(page, 0, page_size);
    (void)memcpy(page, sdb_page_magic, sizeof(sdb_page_magic));
    sdb_write_u16_le(page + 4U, type);
    sdb_write_u16_le(page + 6U, 0U);
    sdb_write_u64_le(page + 8U, page_id);
    sdb_write_u64_le(page + 16U, page_lsn);
    sdb_write_u32_le(page + 24U, (uint32_t)payload_size);
    if (payload_size != 0U) {
        (void)memcpy(page + SDB_PAGE_HEADER_SIZE, payload, payload_size);
    }
    checksum = sdb_crc32_zeroed_range(
        page, page_size, SDB_PAGE_CHECKSUM_OFFSET, sizeof(uint32_t)
    );
    sdb_write_u32_le(page + SDB_PAGE_CHECKSUM_OFFSET, checksum);
    return SDB_OK;
}

sdb_status sdb_page_decode(
    const uint8_t *page,
    size_t page_size,
    uint64_t expected_page_id,
    sdb_page_view *view_out
)
{
    uint16_t type;
    uint16_t flags;
    uint32_t payload_size;
    uint32_t stored_checksum;
    uint32_t computed_checksum;
    uint64_t page_id;
    size_t index;

    if (page == NULL || view_out == NULL || expected_page_id == 0U) {
        return SDB_E_INVALID_ARGUMENT;
    }
    if (page_size < SDB_MIN_PAGE_SIZE || page_size > SDB_MAX_PAGE_SIZE
        || (page_size & (page_size - 1U)) != 0U) {
        return SDB_E_INVALID_ARGUMENT;
    }
    if (memcmp(page, sdb_page_magic, sizeof(sdb_page_magic)) != 0) {
        return SDB_E_BAD_MAGIC;
    }
    type = sdb_read_u16_le(page + 4U);
    flags = sdb_read_u16_le(page + 6U);
    page_id = sdb_read_u64_le(page + 8U);
    payload_size = sdb_read_u32_le(page + 24U);
    if (!sdb_page_type_valid(type) || flags != 0U
        || page_id != expected_page_id
        || (size_t)payload_size > page_size - SDB_PAGE_HEADER_SIZE) {
        return SDB_E_CORRUPT;
    }
    stored_checksum = sdb_read_u32_le(page + SDB_PAGE_CHECKSUM_OFFSET);
    computed_checksum = sdb_crc32_zeroed_range(
        page, page_size, SDB_PAGE_CHECKSUM_OFFSET, sizeof(uint32_t)
    );
    if (stored_checksum != computed_checksum) {
        return SDB_E_CORRUPT;
    }
    for (index = SDB_PAGE_HEADER_SIZE + (size_t)payload_size;
         index < page_size;
         ++index) {
        if (page[index] != 0U) {
            return SDB_E_CORRUPT;
        }
    }
    view_out->type = type;
    view_out->flags = flags;
    view_out->page_id = page_id;
    view_out->page_lsn = sdb_read_u64_le(page + 16U);
    view_out->payload_size = payload_size;
    view_out->payload = page + SDB_PAGE_HEADER_SIZE;
    return SDB_OK;
}
