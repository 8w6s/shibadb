/*
 * Byte-identical gate for the ChaCha20 keystream backend (scalar vs AVX2).
 *
 * The AVX2 kernel only accelerates the ChaCha20 keystream inside
 * sdb_xchacha20poly1305_encrypt; Poly1305 and the AEAD framing are unchanged.
 * So this test drives the public encrypt and compares its CIPHERTEXT against an
 * INDEPENDENT reference: HChaCha20 subkey (via the public sdb_hchacha20, which
 * is not on the accelerated path) + a from-scratch RFC 8439 ChaCha20 block
 * function here, keystream starting at counter 1 (block 0 is the Poly1305 key).
 * Lengths 0..2048 plus 4096/8192 exercise the <1 block tail, partial final
 * block, and many 8-block AVX2 groups. Run with SDB_NO_SIMD=1 to force and gate
 * the scalar path too. The RFC 8439 tag KAT in test_crypto.c anchors Poly1305.
 */

#include "cpu_features.h"
#include "crypto.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t rd32le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8U)
        | ((uint32_t)p[2] << 16U) | ((uint32_t)p[3] << 24U);
}

static void wr32le(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8U);
    p[2] = (uint8_t)(v >> 16U);
    p[3] = (uint8_t)(v >> 24U);
}

static uint32_t rol32(uint32_t x, unsigned n)
{
    return (x << n) | (x >> (32U - n));
}

static void qr(uint32_t s[16], unsigned a, unsigned b, unsigned c, unsigned d)
{
    s[a] += s[b]; s[d] ^= s[a]; s[d] = rol32(s[d], 16U);
    s[c] += s[d]; s[b] ^= s[c]; s[b] = rol32(s[b], 12U);
    s[a] += s[b]; s[d] ^= s[a]; s[d] = rol32(s[d], 8U);
    s[c] += s[d]; s[b] ^= s[c]; s[b] = rol32(s[b], 7U);
}

static void ref_chacha20_block(
    const uint8_t key[32], uint32_t counter, const uint8_t nonce[12],
    uint8_t out[64]
)
{
    uint32_t s[16];
    uint32_t x[16];
    unsigned r;
    size_t i;
    s[0] = 0x61707865U; s[1] = 0x3320646eU;
    s[2] = 0x79622d32U; s[3] = 0x6b206574U;
    for (i = 0U; i < 8U; ++i) {
        s[4U + i] = rd32le(key + i * 4U);
    }
    s[12] = counter;
    s[13] = rd32le(nonce);
    s[14] = rd32le(nonce + 4U);
    s[15] = rd32le(nonce + 8U);
    (void)memcpy(x, s, sizeof(x));
    for (r = 0U; r < 10U; ++r) {
        qr(x, 0U, 4U, 8U, 12U);
        qr(x, 1U, 5U, 9U, 13U);
        qr(x, 2U, 6U, 10U, 14U);
        qr(x, 3U, 7U, 11U, 15U);
        qr(x, 0U, 5U, 10U, 15U);
        qr(x, 1U, 6U, 11U, 12U);
        qr(x, 2U, 7U, 8U, 13U);
        qr(x, 3U, 4U, 9U, 14U);
    }
    for (i = 0U; i < 16U; ++i) {
        wr32le(out + i * 4U, x[i] + s[i]);
    }
}

/* Independent XChaCha20 ciphertext: subkey via sdb_hchacha20, keystream from
 * block counter 1, XORed into the plaintext. */
static void ref_xchacha20_ct(
    const uint8_t key[32], const uint8_t nonce[24],
    const uint8_t *pt, size_t len, uint8_t *ct
)
{
    uint8_t subkey[32];
    uint8_t ietf_nonce[12];
    uint8_t ks[64];
    uint32_t counter = 1U;
    size_t done = 0U;
    sdb_hchacha20(key, nonce, subkey);
    (void)memset(ietf_nonce, 0, 4U);
    (void)memcpy(ietf_nonce + 4U, nonce + 16U, 8U);
    while (done < len) {
        const size_t take = (len - done) < 64U ? (len - done) : 64U;
        size_t i;
        ref_chacha20_block(subkey, counter, ietf_nonce, ks);
        for (i = 0U; i < take; ++i) {
            ct[done + i] = (uint8_t)(pt[done + i] ^ ks[i]);
        }
        done += take;
        ++counter;
    }
}

int main(void)
{
    const sdb_cpu_features *cpu = sdb_cpu_features_get();
    const size_t sizes[3] = { 4096U, 8192U, 5000U };
    uint8_t key[32];
    uint8_t nonce[24];
    uint8_t *pt = (uint8_t *)malloc(8192U);
    uint8_t *ct = (uint8_t *)malloc(8192U);
    uint8_t *ref = (uint8_t *)malloc(8192U);
    uint8_t tag[16];
    size_t len;
    size_t i;

    assert(pt != NULL && ct != NULL && ref != NULL);
    for (i = 0U; i < 32U; ++i) { key[i] = (uint8_t)(i * 7U + 1U); }
    for (i = 0U; i < 24U; ++i) { nonce[i] = (uint8_t)(i * 5U + 3U); }
    for (i = 0U; i < 8192U; ++i) { pt[i] = (uint8_t)((i * 191U + 13U) & 0xffU); }

    (void)printf(
        "test_chacha_simd: backend avx2=%d (SDB_NO_SIMD forces scalar)\n",
        (int)cpu->avx2
    );

    for (len = 0U; len <= 2048U; ++len) {
        sdb_status st = sdb_xchacha20poly1305_encrypt(
            key, nonce, NULL, 0U, pt, len, ct, tag
        );
        assert(st == SDB_OK);
        ref_xchacha20_ct(key, nonce, pt, len, ref);
        assert(memcmp(ct, ref, len) == 0);
    }
    for (i = 0U; i < 3U; ++i) {
        sdb_status st = sdb_xchacha20poly1305_encrypt(
            key, nonce, NULL, 0U, pt, sizes[i], ct, tag
        );
        assert(st == SDB_OK);
        ref_xchacha20_ct(key, nonce, pt, sizes[i], ref);
        assert(memcmp(ct, ref, sizes[i]) == 0);
    }

    /* And a decrypt round-trip must recover the plaintext at a large size. */
    {
        sdb_status st = sdb_xchacha20poly1305_encrypt(
            key, nonce, NULL, 0U, pt, 4096U, ct, tag
        );
        assert(st == SDB_OK);
        st = sdb_xchacha20poly1305_decrypt(
            key, nonce, NULL, 0U, ct, 4096U, tag, ref
        );
        assert(st == SDB_OK);
        assert(memcmp(ref, pt, 4096U) == 0);
    }

    free(pt);
    free(ct);
    free(ref);
    (void)printf("chacha simd tests: ok\n");
    return 0;
}
