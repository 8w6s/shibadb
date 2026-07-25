#include "key_manager.h"

#include <string.h>

static void sdb_key_wrap_nonce(
    const sdb_superblock_v1 *superblock, uint8_t nonce[24]
)
{
    (void)memcpy(nonce, superblock->file_id, SDB_FILE_ID_SIZE);
    sdb_write_u64_le(nonce + 16U, (uint64_t)superblock->key_wrap_id);
}

static void sdb_key_wrap_aad(
    const sdb_superblock_v1 *superblock, uint8_t aad[48]
)
{
    (void)memset(aad, 0, 48U);
    (void)memcpy(aad, superblock->file_id, SDB_FILE_ID_SIZE);
    (void)memcpy(aad + 16U, superblock->salt, SDB_SALT_SIZE);
    sdb_write_u32_le(aad + 32U, superblock->page_size);
    sdb_write_u32_le(aad + 36U, superblock->flags);
    sdb_write_u32_le(aad + 40U, superblock->key_wrap_id);
    sdb_write_u32_le(aad + 44U, superblock->kdf_iterations);
}

sdb_status sdb_key_wrap(
    sdb_superblock_v1 *superblock,
    const uint8_t *password,
    size_t password_size,
    const uint8_t data_key[32]
)
{
    uint8_t wrapping_key[32];
    uint8_t nonce[24];
    uint8_t aad[48];
    sdb_status status;
    if (superblock == NULL || data_key == NULL
        || (password == NULL && password_size != 0U)
        || password_size == 0U
        || (superblock->flags & SDB_FLAG_ENCRYPTED) == 0U
        || superblock->key_wrap_id == 0U
        || superblock->kdf_iterations < SDB_MIN_KDF_ITERATIONS
        || superblock->kdf_iterations > SDB_MAX_KDF_ITERATIONS) {
        return SDB_E_INVALID_ARGUMENT;
    }
    status = sdb_pbkdf2_hmac_sha256(
        password,
        password_size,
        superblock->salt,
        SDB_SALT_SIZE,
        superblock->kdf_iterations,
        wrapping_key,
        sizeof(wrapping_key)
    );
    if (status == SDB_OK) {
        sdb_key_wrap_nonce(superblock, nonce);
        sdb_key_wrap_aad(superblock, aad);
        status = sdb_xchacha20poly1305_encrypt(
            wrapping_key,
            nonce,
            aad,
            sizeof(aad),
            data_key,
            32U,
            superblock->wrapped_key,
            superblock->key_wrap_tag
        );
    }
    sdb_secure_zero(wrapping_key, sizeof(wrapping_key));
    sdb_secure_zero(nonce, sizeof(nonce));
    sdb_secure_zero(aad, sizeof(aad));
    return status;
}

sdb_status sdb_key_unwrap(
    const sdb_superblock_v1 *superblock,
    const uint8_t *password,
    size_t password_size,
    uint8_t data_key_out[32]
)
{
    uint8_t wrapping_key[32];
    uint8_t nonce[24];
    uint8_t aad[48];
    sdb_status status;
    /*
     * Guard parity with sdb_key_wrap (above). Without matching the
     * key_wrap_id != 0 and kdf_iterations range checks, the on-disk
     * decode validator is the only line of defence — fragile if a
     * future refactor changes the parse-then-validate order.
     */
    if (superblock == NULL || data_key_out == NULL
        || (password == NULL && password_size != 0U)
        || password_size == 0U
        || (superblock->flags & SDB_FLAG_ENCRYPTED) == 0U
        || superblock->key_wrap_id == 0U
        || superblock->kdf_iterations < SDB_MIN_KDF_ITERATIONS
        || superblock->kdf_iterations > SDB_MAX_KDF_ITERATIONS) {
        return SDB_E_INVALID_ARGUMENT;
    }
    status = sdb_pbkdf2_hmac_sha256(
        password,
        password_size,
        superblock->salt,
        SDB_SALT_SIZE,
        superblock->kdf_iterations,
        wrapping_key,
        sizeof(wrapping_key)
    );
    if (status == SDB_OK) {
        sdb_key_wrap_nonce(superblock, nonce);
        sdb_key_wrap_aad(superblock, aad);
        status = sdb_xchacha20poly1305_decrypt(
            wrapping_key,
            nonce,
            aad,
            sizeof(aad),
            superblock->wrapped_key,
            SDB_WRAPPED_KEY_SIZE,
            superblock->key_wrap_tag,
            data_key_out
        );
        if (status == SDB_E_CORRUPT) {
            status = SDB_E_AUTHENTICATION;
        }
    }
    if (status != SDB_OK) {
        sdb_secure_zero(data_key_out, 32U);
    }
    sdb_secure_zero(wrapping_key, sizeof(wrapping_key));
    sdb_secure_zero(nonce, sizeof(nonce));
    sdb_secure_zero(aad, sizeof(aad));
    return status;
}
