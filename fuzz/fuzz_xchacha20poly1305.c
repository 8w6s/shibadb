#include "crypto.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    static const uint8_t key[32] = {
        0x00U, 0x01U, 0x02U, 0x03U
    };
    static const uint8_t nonce[24] = {
        0x10U, 0x11U, 0x12U, 0x13U
    };
    uint8_t plaintext[4096];
    uint8_t before[4096];
    uint8_t tag[16] = {0U};
    size_t ciphertext_size;
    if (size > sizeof(plaintext) + sizeof(tag)) {
        return 0;
    }
    ciphertext_size = size > sizeof(tag) ? size - sizeof(tag) : 0U;
    if (size >= sizeof(tag)) {
        (void)memcpy(tag, data + ciphertext_size, sizeof(tag));
    }
    (void)memset(plaintext, 0xa5, sizeof(plaintext));
    (void)memcpy(before, plaintext, sizeof(before));
    if (sdb_xchacha20poly1305_decrypt(
            key,
            nonce,
            NULL,
            0U,
            data,
            ciphertext_size,
            tag,
            plaintext
        ) != SDB_OK) {
        if (memcmp(plaintext, before, sizeof(plaintext)) != 0) {
            __builtin_trap();
        }
    }
    return 0;
}
