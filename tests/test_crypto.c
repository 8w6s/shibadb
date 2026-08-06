#include "crypto.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint8_t hex_nibble(char character)
{
    if (character >= '0' && character <= '9') {
        return (uint8_t)(character - '0');
    }
    if (character >= 'a' && character <= 'f') {
        return (uint8_t)(character - 'a' + 10);
    }
    assert(false);
    return 0U;
}

static size_t decode_hex(const char *hex, uint8_t *output, size_t capacity)
{
    const size_t length = strlen(hex);
    size_t index;
    assert((length & 1U) == 0U);
    assert(length / 2U <= capacity);
    for (index = 0U; index < length / 2U; ++index) {
        output[index] = (uint8_t)(
            (hex_nibble(hex[index * 2U]) << 4U)
            | hex_nibble(hex[index * 2U + 1U])
        );
    }
    return length / 2U;
}

int main(void)
{
    uint8_t actual[256];
    uint8_t expected[256];
    size_t expected_size;

    expected_size = decode_hex(
        "ba7816bf8f01cfea414140de5dae2223"
        "b00361a396177a9cb410ff61f20015ad",
        expected,
        sizeof(expected)
    );
    sdb_sha256((const uint8_t *)"abc", 3U, actual);
    assert(expected_size == 32U);
    assert(memcmp(actual, expected, 32U) == 0);

    expected_size = decode_hex(
        "f7bc83f430538424b13298e6aa6fb143"
        "ef4d59a14946175997479dbc2d1a3cd8",
        expected,
        sizeof(expected)
    );
    sdb_hmac_sha256(
        (const uint8_t *)"key",
        3U,
        (const uint8_t *)"The quick brown fox jumps over the lazy dog",
        43U,
        actual
    );
    assert(expected_size == 32U);
    assert(memcmp(actual, expected, 32U) == 0);

    expected_size = decode_hex(
        "ae4d0c95af6b46d32d0adff928f06dd0"
        "2a303f8ef3c251dfd6e2d85a95474c43",
        expected,
        sizeof(expected)
    );
    assert(sdb_pbkdf2_hmac_sha256(
        (const uint8_t *)"password",
        8U,
        (const uint8_t *)"salt",
        4U,
        2U,
        actual,
        32U
    ) == SDB_OK);
    assert(expected_size == 32U);
    assert(memcmp(actual, expected, 32U) == 0);

    {
        uint8_t key[32];
        uint8_t nonce[16];
        size_t index;
        for (index = 0U; index < sizeof(key); ++index) {
            key[index] = (uint8_t)index;
        }
        expected_size = decode_hex(
            "000000090000004a0000000031415927",
            nonce,
            sizeof(nonce)
        );
        assert(expected_size == sizeof(nonce));
        expected_size = decode_hex(
            "82413b4227b27bfed30e42508a877d73"
            "a0f9e4d58a74a853c12ec41326d3ecdc",
            expected,
            sizeof(expected)
        );
        sdb_hchacha20(key, nonce, actual);
        assert(expected_size == 32U);
        assert(memcmp(actual, expected, 32U) == 0);
    }

    {
        static const uint8_t plaintext[] =
            "Ladies and Gentlemen of the class of '99: If I could offer you "
            "only one tip for the future, sunscreen would be it.";
        uint8_t key[32];
        uint8_t nonce[24];
        uint8_t aad[12];
        uint8_t ciphertext[sizeof(plaintext) - 1U];
        uint8_t decrypted[sizeof(plaintext) - 1U];
        uint8_t tag[16];
        size_t index;
        for (index = 0U; index < sizeof(key); ++index) {
            key[index] = (uint8_t)(0x80U + index);
        }
        for (index = 0U; index < sizeof(nonce); ++index) {
            nonce[index] = (uint8_t)(0x40U + index);
        }
        expected_size = decode_hex(
            "50515253c0c1c2c3c4c5c6c7", aad, sizeof(aad)
        );
        assert(expected_size == sizeof(aad));
        expected_size = decode_hex(
            "bd6d179d3e83d43b9576579493c0e939"
            "572a1700252bfaccbed2902c21396cbb"
            "731c7f1b0b4aa6440bf3a82f4eda7e39"
            "ae64c6708c54c216cb96b72e1213b452"
            "2f8c9ba40db5d945b11b69b982c1bb9e"
            "3f3fac2bc369488f76b2383565d3fff9"
            "21f9664c97637da9768812f615c68b13"
            "b52e",
            expected,
            sizeof(expected)
        );
        assert(expected_size == sizeof(ciphertext));
        assert(sdb_xchacha20poly1305_encrypt(
            key,
            nonce,
            aad,
            sizeof(aad),
            plaintext,
            sizeof(plaintext) - 1U,
            ciphertext,
            tag
        ) == SDB_OK);
        assert(memcmp(ciphertext, expected, expected_size) == 0);
        expected_size = decode_hex(
            "c0875924c1c7987947deafd8780acf49",
            expected,
            sizeof(expected)
        );
        assert(memcmp(tag, expected, expected_size) == 0);
        assert(sdb_xchacha20poly1305_decrypt(
            key,
            nonce,
            aad,
            sizeof(aad),
            ciphertext,
            sizeof(ciphertext),
            tag,
            decrypted
        ) == SDB_OK);
        assert(memcmp(
            decrypted, plaintext, sizeof(decrypted)
        ) == 0);
        ciphertext[17] ^= 1U;
        (void)memset(decrypted, 0xa5, sizeof(decrypted));
        assert(sdb_xchacha20poly1305_decrypt(
            key,
            nonce,
            aad,
            sizeof(aad),
            ciphertext,
            sizeof(ciphertext),
            tag,
            decrypted
        ) == SDB_E_CORRUPT);
        for (index = 0U; index < sizeof(decrypted); ++index) {
            assert(decrypted[index] == 0xa5U);
        }
        ciphertext[17] ^= 1U;
        {
            uint8_t bad_key[32];
            uint8_t bad_nonce[24];
            uint8_t bad_tag[16];
            uint8_t bad_aad[sizeof(aad)];
            (void)memcpy(bad_key, key, sizeof(key));
            (void)memcpy(bad_nonce, nonce, sizeof(nonce));
            (void)memcpy(bad_tag, tag, sizeof(tag));
            (void)memcpy(bad_aad, aad, sizeof(aad));
            bad_key[0] ^= 1U;
            bad_nonce[0] ^= 1U;
            bad_tag[0] ^= 1U;
            bad_aad[0] ^= 1U;
            /* Wrong key: authentication must fail, buffer untouched. */
            (void)memset(decrypted, 0xa5, sizeof(decrypted));
            assert(sdb_xchacha20poly1305_decrypt(
                bad_key,
                nonce,
                aad,
                sizeof(aad),
                ciphertext,
                sizeof(ciphertext),
                tag,
                decrypted
            ) == SDB_E_CORRUPT);
            for (index = 0U; index < sizeof(decrypted); ++index) {
                assert(decrypted[index] == 0xa5U);
            }
            /* Flipped nonce. */
            (void)memset(decrypted, 0xa5, sizeof(decrypted));
            assert(sdb_xchacha20poly1305_decrypt(
                key,
                bad_nonce,
                aad,
                sizeof(aad),
                ciphertext,
                sizeof(ciphertext),
                tag,
                decrypted
            ) == SDB_E_CORRUPT);
            for (index = 0U; index < sizeof(decrypted); ++index) {
                assert(decrypted[index] == 0xa5U);
            }
            /* Flipped tag. */
            (void)memset(decrypted, 0xa5, sizeof(decrypted));
            assert(sdb_xchacha20poly1305_decrypt(
                key,
                nonce,
                aad,
                sizeof(aad),
                ciphertext,
                sizeof(ciphertext),
                bad_tag,
                decrypted
            ) == SDB_E_CORRUPT);
            for (index = 0U; index < sizeof(decrypted); ++index) {
                assert(decrypted[index] == 0xa5U);
            }
            /* Altered AAD: same ciphertext+tag, tampered associated data. */
            (void)memset(decrypted, 0xa5, sizeof(decrypted));
            assert(sdb_xchacha20poly1305_decrypt(
                key,
                nonce,
                bad_aad,
                sizeof(bad_aad),
                ciphertext,
                sizeof(ciphertext),
                tag,
                decrypted
            ) == SDB_E_CORRUPT);
            for (index = 0U; index < sizeof(decrypted); ++index) {
                assert(decrypted[index] == 0xa5U);
            }
            /*
             * Sanity: with the ciphertext restored and every input pristine
             * the same call decrypts cleanly, so the four rejections above
             * are attributable to the tampering and not to a corrupted setup.
             */
            (void)memset(decrypted, 0xa5, sizeof(decrypted));
            assert(sdb_xchacha20poly1305_decrypt(
                key,
                nonce,
                aad,
                sizeof(aad),
                ciphertext,
                sizeof(ciphertext),
                tag,
                decrypted
            ) == SDB_OK);
            assert(memcmp(decrypted, plaintext, sizeof(decrypted)) == 0);
        }
    }
    (void)puts("crypto tests: ok");
    return 0;
}
