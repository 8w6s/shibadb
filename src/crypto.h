#ifndef SHIBADB_CRYPTO_H
#define SHIBADB_CRYPTO_H

#include "internal.h"

#define SDB_CRYPTO_KEY_SIZE ((size_t)32)
#define SDB_CRYPTO_NONCE_SIZE ((size_t)24)
#define SDB_CRYPTO_TAG_SIZE ((size_t)16)
#define SDB_SHA256_SIZE ((size_t)32)

typedef struct sdb_sha256_context {
    uint32_t state[8];
    uint64_t bit_count;
    uint8_t buffer[64];
    size_t buffered;
} sdb_sha256_context;

void sdb_secure_zero(void *memory, size_t size);
bool sdb_constant_time_equal(
    const uint8_t *left, const uint8_t *right, size_t size
);
sdb_status sdb_random_bytes(uint8_t *output, size_t size);
void sdb_sha256(const uint8_t *data, size_t size, uint8_t output[32]);
void sdb_sha256_init(sdb_sha256_context *context);
void sdb_sha256_update(
    sdb_sha256_context *context, const uint8_t *data, size_t size
);
void sdb_sha256_final(
    sdb_sha256_context *context, uint8_t output[32]
);
void sdb_hmac_sha256(
    const uint8_t *key,
    size_t key_size,
    const uint8_t *data,
    size_t data_size,
    uint8_t output[32]
);
sdb_status sdb_pbkdf2_hmac_sha256(
    const uint8_t *password,
    size_t password_size,
    const uint8_t *salt,
    size_t salt_size,
    uint32_t iterations,
    uint8_t *output,
    size_t output_size
);
void sdb_hchacha20(
    const uint8_t key[32], const uint8_t nonce[16], uint8_t output[32]
);
sdb_status sdb_xchacha20poly1305_encrypt(
    const uint8_t key[32],
    const uint8_t nonce[24],
    const uint8_t *aad,
    size_t aad_size,
    const uint8_t *plaintext,
    size_t plaintext_size,
    uint8_t *ciphertext,
    uint8_t tag[16]
);
sdb_status sdb_xchacha20poly1305_decrypt(
    const uint8_t key[32],
    const uint8_t nonce[24],
    const uint8_t *aad,
    size_t aad_size,
    const uint8_t *ciphertext,
    size_t ciphertext_size,
    const uint8_t tag[16],
    uint8_t *plaintext
);

#endif
