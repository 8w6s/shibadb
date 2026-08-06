#include "encrypted_page.h"

#include <stdlib.h>
#include <string.h>

static const uint8_t sdb_encrypted_magic[4] = {
    (uint8_t)'S', (uint8_t)'E', (uint8_t)'N', (uint8_t)'1'
};

_Static_assert(SDB_FILE_ID_SIZE == 16U,
    "encrypted AAD layout requires a 16-byte file ID");
_Static_assert(SDB_CRYPTO_NONCE_SIZE == 24U,
    "encrypted page format requires a 24-byte nonce");
_Static_assert(SDB_CRYPTO_TAG_SIZE == 16U,
    "encrypted page format requires a 16-byte tag");
_Static_assert(
    SDB_ENCRYPTED_NONCE_OFFSET + SDB_CRYPTO_NONCE_SIZE
        == SDB_ENCRYPTED_SIZE_OFFSET,
    "encrypted page nonce/size offset mismatch");
_Static_assert(SDB_ENCRYPTED_SIZE_OFFSET + 4U == SDB_ENCRYPTED_TAG_OFFSET,
    "encrypted page size/tag offset mismatch");
_Static_assert(
    SDB_ENCRYPTED_TAG_OFFSET + SDB_CRYPTO_TAG_SIZE
        == SDB_ENCRYPTED_PAYLOAD_HEADER_SIZE,
    "encrypted page tag/header offset mismatch");
_Static_assert(SDB_ENCRYPTED_AAD_PAGE_ID_OFFSET == SDB_FILE_ID_SIZE,
    "encrypted AAD file-id/page-id offset mismatch");
_Static_assert(SDB_ENCRYPTED_AAD_SIZE_OFFSET + 4U == SDB_ENCRYPTED_AAD_SIZE,
    "encrypted AAD size-field offset mismatch");

static void sdb_encrypted_aad(
    const sdb_superblock_v1 *superblock,
    uint64_t page_id,
    uint64_t page_lsn,
    uint16_t plaintext_type,
    uint32_t plaintext_size,
    uint8_t aad[SDB_ENCRYPTED_AAD_SIZE]
)
{
    (void)memset(aad, 0, SDB_ENCRYPTED_AAD_SIZE);
    (void)memcpy(
        aad + SDB_ENCRYPTED_AAD_FILE_ID_OFFSET,
        superblock->file_id,
        SDB_FILE_ID_SIZE
    );
    sdb_write_u64_le(aad + SDB_ENCRYPTED_AAD_PAGE_ID_OFFSET, page_id);
    sdb_write_u64_le(aad + SDB_ENCRYPTED_AAD_PAGE_LSN_OFFSET, page_lsn);
    sdb_write_u16_le(aad + SDB_ENCRYPTED_AAD_TYPE_OFFSET, plaintext_type);
    sdb_write_u32_le(aad + SDB_ENCRYPTED_AAD_SIZE_OFFSET, plaintext_size);
}

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
)
{
    uint8_t *payload;
    uint8_t nonce[SDB_CRYPTO_NONCE_SIZE];
    uint8_t aad[SDB_ENCRYPTED_AAD_SIZE];
    sdb_status status;
    if (superblock == NULL || data_key == NULL || page == NULL
        || page_id == 0U
        || (plaintext_type != (uint16_t)SDB_PAGE_TYPE_DATA
            && plaintext_type != (uint16_t)SDB_PAGE_TYPE_FREE)
        || page_size < SDB_PAGE_HEADER_SIZE
            + SDB_ENCRYPTED_PAYLOAD_HEADER_SIZE
        || (plaintext == NULL && plaintext_size != 0U)
        || plaintext_size > (size_t)UINT32_MAX
        || plaintext_size > page_size - SDB_PAGE_HEADER_SIZE
            - SDB_ENCRYPTED_PAYLOAD_HEADER_SIZE) {
        return SDB_E_INVALID_ARGUMENT;
    }
    payload = (uint8_t *)malloc(
        SDB_ENCRYPTED_PAYLOAD_HEADER_SIZE + plaintext_size
    );
    if (payload == NULL) {
        return SDB_E_OUT_OF_MEMORY;
    }
    (void)memset(payload, 0, SDB_ENCRYPTED_PAYLOAD_HEADER_SIZE);
    (void)memcpy(
        payload + SDB_ENCRYPTED_MAGIC_OFFSET,
        sdb_encrypted_magic,
        sizeof(sdb_encrypted_magic)
    );
    sdb_write_u16_le(payload + SDB_ENCRYPTED_VERSION_OFFSET, UINT16_C(1));
    sdb_write_u16_le(payload + SDB_ENCRYPTED_TYPE_OFFSET, plaintext_type);
    status = sdb_random_bytes(nonce, sizeof(nonce));
    if (status != SDB_OK) {
        sdb_secure_zero(
            payload, SDB_ENCRYPTED_PAYLOAD_HEADER_SIZE + plaintext_size
        );
        free(payload);
        return status;
    }
    (void)memcpy(payload + SDB_ENCRYPTED_NONCE_OFFSET, nonce, sizeof(nonce));
    sdb_write_u32_le(
        payload + SDB_ENCRYPTED_SIZE_OFFSET, (uint32_t)plaintext_size
    );
    sdb_encrypted_aad(
        superblock,
        page_id,
        page_lsn,
        plaintext_type,
        (uint32_t)plaintext_size,
        aad
    );
    status = sdb_xchacha20poly1305_encrypt(
        data_key,
        nonce,
        aad,
        sizeof(aad),
        plaintext,
        plaintext_size,
        payload + SDB_ENCRYPTED_PAYLOAD_HEADER_SIZE,
        payload + SDB_ENCRYPTED_TAG_OFFSET
    );
    if (status == SDB_OK) {
        status = sdb_page_encode(
            page,
            page_size,
            (uint16_t)SDB_PAGE_TYPE_ENCRYPTED,
            page_id,
            page_lsn,
            payload,
            SDB_ENCRYPTED_PAYLOAD_HEADER_SIZE + plaintext_size
        );
    }
    sdb_secure_zero(payload, SDB_ENCRYPTED_PAYLOAD_HEADER_SIZE + plaintext_size);
    free(payload);
    sdb_secure_zero(nonce, sizeof(nonce));
    sdb_secure_zero(aad, sizeof(aad));
    return status;
}

sdb_status sdb_encrypted_page_decrypt(
    const sdb_superblock_v1 *superblock,
    const uint8_t data_key[SDB_CRYPTO_KEY_SIZE],
    const sdb_page_view *encrypted_view,
    uint8_t *plaintext,
    size_t plaintext_capacity,
    uint16_t *plaintext_type_out,
    size_t *plaintext_size_out
)
{
    const uint8_t *payload;
    uint8_t expected_nonce[SDB_CRYPTO_NONCE_SIZE];
    uint8_t aad[SDB_ENCRYPTED_AAD_SIZE];
    uint16_t version;
    uint16_t plaintext_type;
    uint32_t plaintext_size;
    sdb_status status;
    if (superblock == NULL || data_key == NULL || encrypted_view == NULL
        || encrypted_view->payload == NULL
        || plaintext_type_out == NULL || plaintext_size_out == NULL
        || encrypted_view->type != (uint16_t)SDB_PAGE_TYPE_ENCRYPTED
        || encrypted_view->payload_size < SDB_ENCRYPTED_PAYLOAD_HEADER_SIZE) {
        return SDB_E_INVALID_ARGUMENT;
    }
    *plaintext_type_out = 0U;
    *plaintext_size_out = 0U;
    payload = encrypted_view->payload;
    version = sdb_read_u16_le(payload + SDB_ENCRYPTED_VERSION_OFFSET);
    plaintext_type = sdb_read_u16_le(payload + SDB_ENCRYPTED_TYPE_OFFSET);
    plaintext_size = sdb_read_u32_le(payload + SDB_ENCRYPTED_SIZE_OFFSET);
    if (memcmp(
            payload + SDB_ENCRYPTED_MAGIC_OFFSET,
            sdb_encrypted_magic,
            sizeof(sdb_encrypted_magic)
        ) != 0
        || version != UINT16_C(1)
        || (plaintext_type != (uint16_t)SDB_PAGE_TYPE_DATA
            && plaintext_type != (uint16_t)SDB_PAGE_TYPE_FREE)
        || (size_t)plaintext_size
            != (size_t)encrypted_view->payload_size
                - SDB_ENCRYPTED_PAYLOAD_HEADER_SIZE) {
        return SDB_E_CORRUPT;
    }
    if (plaintext == NULL && plaintext_capacity != 0U) {
        return SDB_E_INVALID_ARGUMENT;
    }
    if (plaintext_capacity < (size_t)plaintext_size) {
        *plaintext_size_out = (size_t)plaintext_size;
        return SDB_E_BUFFER_TOO_SMALL;
    }
    (void)memcpy(
        expected_nonce,
        payload + SDB_ENCRYPTED_NONCE_OFFSET,
        sizeof(expected_nonce)
    );
    sdb_encrypted_aad(
        superblock,
        encrypted_view->page_id,
        encrypted_view->page_lsn,
        plaintext_type,
        plaintext_size,
        aad
    );
    status = sdb_xchacha20poly1305_decrypt(
        data_key,
        expected_nonce,
        aad,
        sizeof(aad),
        payload + SDB_ENCRYPTED_PAYLOAD_HEADER_SIZE,
        (size_t)plaintext_size,
        payload + SDB_ENCRYPTED_TAG_OFFSET,
        plaintext
    );
    if (status == SDB_OK) {
        *plaintext_type_out = plaintext_type;
        *plaintext_size_out = (size_t)plaintext_size;
    }
    sdb_secure_zero(expected_nonce, sizeof(expected_nonce));
    sdb_secure_zero(aad, sizeof(aad));
    return status;
}
