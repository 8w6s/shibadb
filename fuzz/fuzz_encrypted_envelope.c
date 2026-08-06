
#include "encrypted_page.h"
#include "page.h"
#include "shibadb.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define SDB_FUZZ_PAGE_SIZE ((size_t)4096)

static const uint8_t k_data_key[32] = {
    0x01U, 0x02U, 0x03U, 0x04U, 0x05U, 0x06U, 0x07U, 0x08U,
    0x09U, 0x0aU, 0x0bU, 0x0cU, 0x0dU, 0x0eU, 0x0fU, 0x10U,
    0x11U, 0x12U, 0x13U, 0x14U, 0x15U, 0x16U, 0x17U, 0x18U,
    0x19U, 0x1aU, 0x1bU, 0x1cU, 0x1dU, 0x1eU, 0x1fU, 0x20U
};

static void build_superblock(sdb_superblock_v1 *sb)
{
    size_t i;
    (void)memset(sb, 0, sizeof(*sb));
    sb->page_size = (uint32_t)SDB_FUZZ_PAGE_SIZE;
    sb->flags = SDB_FLAG_ENCRYPTED;
    sb->generation = 1U;
    sb->root_page = 2U;
    sb->next_page_id = 3U;
    sb->key_wrap_id = 1U;
    sb->kdf_iterations = 600000U;
    for (i = 0U; i < SDB_SALT_SIZE; ++i) {
        sb->salt[i] = (uint8_t)(0x40U + i);
    }
    for (i = 0U; i < SDB_FILE_ID_SIZE; ++i) {
        sb->file_id[i] = (uint8_t)(0x80U + i);
    }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    sdb_superblock_v1 sb;
    uint8_t page[SDB_FUZZ_PAGE_SIZE];
    uint8_t plaintext_out[SDB_FUZZ_PAGE_SIZE];
    uint8_t plaintext_before[SDB_FUZZ_PAGE_SIZE];
    size_t plaintext_size_out;
    uint16_t plaintext_type_out;
    sdb_page_view view;
    const size_t max_plaintext = SDB_FUZZ_PAGE_SIZE
        - SDB_PAGE_HEADER_SIZE - SDB_ENCRYPTED_PAYLOAD_HEADER_SIZE;

    build_superblock(&sb);

    if (size <= max_plaintext) {
        (void)memset(page, 0, sizeof(page));
        if (sdb_encrypted_page_encode(
                &sb, k_data_key, page, sizeof(page),
                (uint16_t)SDB_PAGE_TYPE_DATA,
                 42U,  7U,
                data, size
            ) == SDB_OK) {
            if (sdb_page_decode(page, sizeof(page), 42U, &view) == SDB_OK) {
                (void)memset(plaintext_out, 0xa5, sizeof(plaintext_out));
                if (sdb_encrypted_page_decrypt(
                        &sb, k_data_key, &view,
                        plaintext_out, sizeof(plaintext_out),
                        &plaintext_type_out, &plaintext_size_out
                    ) == SDB_OK) {

                    if (plaintext_size_out != size
                        || plaintext_type_out != (uint16_t)SDB_PAGE_TYPE_DATA
                        || (size != 0U
                            && memcmp(plaintext_out, data, size) != 0)) {
                        __builtin_trap();
                    }
                }
            }
        }
    }

    if (size <= sizeof(page)) {
        (void)memset(page, 0, sizeof(page));
        (void)memcpy(page, data, size);
        (void)memset(plaintext_out, 0x5a, sizeof(plaintext_out));
        (void)memcpy(plaintext_before, plaintext_out, sizeof(plaintext_out));
        plaintext_size_out = 0U;
        plaintext_type_out = 0U;
        if (sdb_page_decode(page, sizeof(page), 42U, &view) == SDB_OK) {
            const sdb_status status = sdb_encrypted_page_decrypt(
                &sb, k_data_key, &view,
                plaintext_out, sizeof(plaintext_out),
                &plaintext_type_out, &plaintext_size_out
            );
            if (status != SDB_OK) {

                if (memcmp(
                        plaintext_out, plaintext_before,
                        sizeof(plaintext_out)
                    ) != 0) {
                    __builtin_trap();
                }
            }
        }
    }
    return 0;
}
