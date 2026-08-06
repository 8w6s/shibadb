#ifndef SHIBADB_PAGE_H
#define SHIBADB_PAGE_H

#include "shibadb.h"

#include <stddef.h>
#include <stdint.h>

#define SDB_PAGE_HEADER_SIZE ((size_t)32)

typedef enum sdb_page_type {
    SDB_PAGE_TYPE_DATA = 1,
    SDB_PAGE_TYPE_FREE = 2,
    SDB_PAGE_TYPE_ENCRYPTED = 3
} sdb_page_type;

typedef struct sdb_page_view {
    uint16_t type;
    uint16_t flags;
    uint64_t page_id;
    uint64_t page_lsn;
    const uint8_t *payload;
    uint32_t payload_size;
} sdb_page_view;

sdb_status sdb_page_encode(
    uint8_t *page,
    size_t page_size,
    uint16_t type,
    uint64_t page_id,
    uint64_t page_lsn,
    const uint8_t *payload,
    size_t payload_size
);

sdb_status sdb_page_decode(
    const uint8_t *page,
    size_t page_size,
    uint64_t expected_page_id,
    sdb_page_view *view_out
);

#endif
