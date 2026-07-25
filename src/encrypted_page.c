#include "encrypted_page.h"

#include <stdlib.h>
#include <string.h>

static const uint8_t sdb_encrypted_magic[4] = {
    (uint8_t)'S', (uint8_t)'E', (uint8_t)'N', (uint8_t)'1'
};

static void sdb_encrypted_aad(
    const sdb_superblock_v1 *superblock,
    uint64_t page_id,
    uint64_t page_lsn,
    uint16_t plaintext_type,
    uint32_t plaintext_size,
    uint8_t aad[40]
)
{
    (void)memset(aad, 0, 40U);
    (void)memcpy(aad, superblock->file_id, SDB_FILE_ID_SIZE);
    sdb_write_u64_le(aad + 16U, page_id);
    sdb_write_u64_le(aad + 24U, page_lsn);
    sdb_write_u16_le(aad + 32U, plaintext_type);
    sdb_write_u32_le(aad + 36U, plaintext_size);
}

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
)
{
    uint8_t *payload;
    uint8_t nonce[24];
    uint8_t aad[40];
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
        return SDB_E_INTERNAL;
    }
    (void)memset(payload, 0, SDB_ENCRYPTED_PAYLOAD_HEADER_SIZE);
    (void)memcpy(payload, sdb_encrypted_magic, sizeof(sdb_encrypted_magic));
    sdb_write_u16_le(payload + 4U, UINT16_C(1));
    sdb_write_u16_le(payload + 6U, plaintext_type);
    status = sdb_random_bytes(nonce, sizeof(nonce));
    if (status != SDB_OK) {
        sdb_secure_zero(
            payload, SDB_ENCRYPTED_PAYLOAD_HEADER_SIZE + plaintext_size
        );
        free(payload);
        return status;
    }
    (void)memcpy(payload + 8U, nonce, sizeof(nonce));
    sdb_write_u32_le(payload + 32U, (uint32_t)plaintext_size);
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
        payload + 36U
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
    const uint8_t data_key[32],
    const sdb_page_view *encrypted_view,
    uint8_t *plaintext,
    size_t plaintext_capacity,
    uint16_t *plaintext_type_out,
    size_t *plaintext_size_out
)
{
    const uint8_t *payload;
    uint8_t expected_nonce[24];
    uint8_t aad[40];
    uint32_t plaintext_size;
    sdb_status status;
    if (superblock == NULL || data_key == NULL || encrypted_view == NULL
        || plaintext_type_out == NULL || plaintext_size_out == NULL
        || encrypted_view->type != (uint16_t)SDB_PAGE_TYPE_ENCRYPTED
        || encrypted_view->payload_size < SDB_ENCRYPTED_PAYLOAD_HEADER_SIZE) {
        return SDB_E_INVALID_ARGUMENT;
    }
    payload = encrypted_view->payload;
    plaintext_size = sdb_read_u32_le(payload + 32U);
    if (memcmp(payload, sdb_encrypted_magic, sizeof(sdb_encrypted_magic)) != 0
        || sdb_read_u16_le(payload + 4U) != UINT16_C(1)
        || (sdb_read_u16_le(payload + 6U)
                != (uint16_t)SDB_PAGE_TYPE_DATA
            && sdb_read_u16_le(payload + 6U)
                != (uint16_t)SDB_PAGE_TYPE_FREE)
        || (size_t)plaintext_size
            != (size_t)encrypted_view->payload_size
                - SDB_ENCRYPTED_PAYLOAD_HEADER_SIZE
        || plaintext_capacity < (size_t)plaintext_size
        || (plaintext == NULL && plaintext_size != 0U)) {
        return SDB_E_CORRUPT;
    }
    (void)memcpy(expected_nonce, payload + 8U, sizeof(expected_nonce));
    sdb_encrypted_aad(
        superblock,
        encrypted_view->page_id,
        encrypted_view->page_lsn,
        sdb_read_u16_le(payload + 6U),
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
        payload + 36U,
        plaintext
    );
    if (status == SDB_OK) {
        *plaintext_type_out = sdb_read_u16_le(payload + 6U);
        *plaintext_size_out = (size_t)plaintext_size;
    }
    sdb_secure_zero(expected_nonce, sizeof(expected_nonce));
    sdb_secure_zero(aad, sizeof(aad));
    return status;
}
