#ifndef SHIBADB_ENCRYPTED_PAGE_H
#define SHIBADB_ENCRYPTED_PAGE_H

#include "crypto.h"
#include "page.h"

#define SDB_ENCRYPTED_MAGIC_OFFSET ((size_t)0)
#define SDB_ENCRYPTED_VERSION_OFFSET ((size_t)4)
#define SDB_ENCRYPTED_TYPE_OFFSET ((size_t)6)
#define SDB_ENCRYPTED_NONCE_OFFSET ((size_t)8)
#define SDB_ENCRYPTED_SIZE_OFFSET ((size_t)32)
#define SDB_ENCRYPTED_TAG_OFFSET ((size_t)36)
#define SDB_ENCRYPTED_PAYLOAD_HEADER_SIZE ((size_t)52)

#define SDB_ENCRYPTED_AAD_FILE_ID_OFFSET ((size_t)0)
#define SDB_ENCRYPTED_AAD_PAGE_ID_OFFSET ((size_t)16)
#define SDB_ENCRYPTED_AAD_PAGE_LSN_OFFSET ((size_t)24)
#define SDB_ENCRYPTED_AAD_TYPE_OFFSET ((size_t)32)
#define SDB_ENCRYPTED_AAD_SIZE_OFFSET ((size_t)36)
#define SDB_ENCRYPTED_AAD_SIZE ((size_t)40)

sdb_status sdb_encrypted_page_encode(
    const sdb_superblock_v1 *superblock,
    const uint8_t data_key[SDB_CRYPTO_KEY_SIZE],
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
    const uint8_t data_key[SDB_CRYPTO_KEY_SIZE],
    const sdb_page_view *encrypted_view,
    uint8_t *plaintext,
    size_t plaintext_capacity,
    uint16_t *plaintext_type_out,
    size_t *plaintext_size_out
);

#endif
