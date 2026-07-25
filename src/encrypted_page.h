#ifndef SHIBADB_ENCRYPTED_PAGE_H
#define SHIBADB_ENCRYPTED_PAGE_H

#include "crypto.h"
#include "page.h"

#define SDB_ENCRYPTED_PAYLOAD_HEADER_SIZE ((size_t)52)

sdb_status sdb_encrypted_page_encode(
    const sdb_superblock_v1 *superblock,
    const uint8_t data_key[32],
    uint8_t *page,
    size_t page_size,
    uint16_t plaintext_type,
    uint64_t page_id,
    uint64_t page_lsn,
    const uint8_t *plaintext,
    size_t plaintext_size
);
sdb_status sdb_encrypted_page_decrypt(
    const sdb_superblock_v1 *superblock,
    const uint8_t data_key[32],
    const sdb_page_view *encrypted_view,
    uint8_t *plaintext,
    size_t plaintext_capacity,
    uint16_t *plaintext_type_out,
    size_t *plaintext_size_out
);

#endif
