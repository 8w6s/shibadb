/*
 * Byte-identical gate for the SHA-256 backend (scalar vs SHA-NI).
 *
 * Mirrors the philosophy of test_codec.c's CRC differential: an INDEPENDENT
 * reference implementation (ref_sha256, written from the FIPS 180-4 spec here,
 * not shared with production) is compared against sdb_sha256 across every
 * length 0..2048 — which exercises single-block, multi-block, and every
 * final-block padding boundary (the 55/56/64 byte edges). Whatever backend the
 * runtime selected (SHA-NI on capable CPUs, scalar otherwise) must match the
 * reference bit-for-bit. Run again with SDB_NO_SIMD=1 to force the scalar path
 * on a SHA-NI machine and gate both.
 *
 * The NIST "abc" and empty-string known-answer vectors anchor the reference
 * itself, so a shared blind spot cannot pass silently.
 */

#include "cpu_features.h"
#include "crypto.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static uint32_t ref_rotr(uint32_t value, unsigned amount)
{
    return (value >> amount) | (value << (32U - amount));
}

static void ref_sha256(const uint8_t *data, size_t len, uint8_t out[32])
{
    static const uint32_t k[64] = {
        0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU,
        0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U, 0xd807aa98U, 0x12835b01U,
        0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U,
        0xc19bf174U, 0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
        0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU, 0x983e5152U,
        0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U,
        0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU,
        0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
        0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U, 0xd192e819U,
        0xd6990624U, 0xf40e3585U, 0x106aa070U, 0x19a4c116U, 0x1e376c08U,
        0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU,
        0x682e6ff3U, 0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
        0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U
    };
    uint32_t h[8] = {
        0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
        0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U
    };
    uint8_t buffer[4224];
    uint64_t bits = (uint64_t)len * UINT64_C(8);
    size_t total;
    size_t block;
    size_t i;

    assert(len <= 4096U);
    if (len != 0U) {
        (void)memcpy(buffer, data, len);
    }
    buffer[len] = 0x80U;
    total = len + 1U;
    while ((total % 64U) != 56U) {
        buffer[total] = 0U;
        ++total;
    }
    for (i = 0U; i < 8U; ++i) {
        buffer[total] = (uint8_t)(bits >> (56U - i * 8U));
        ++total;
    }

    for (block = 0U; block < total; block += 64U) {
        uint32_t w[64];
        uint32_t a;
        uint32_t b;
        uint32_t c;
        uint32_t d;
        uint32_t e;
        uint32_t f;
        uint32_t g;
        uint32_t hh;
        size_t t;
        for (t = 0U; t < 16U; ++t) {
            const uint8_t *p = buffer + block + t * 4U;
            w[t] = ((uint32_t)p[0] << 24U) | ((uint32_t)p[1] << 16U)
                | ((uint32_t)p[2] << 8U) | (uint32_t)p[3];
        }
        for (t = 16U; t < 64U; ++t) {
            const uint32_t s0 = ref_rotr(w[t - 15U], 7U)
                ^ ref_rotr(w[t - 15U], 18U) ^ (w[t - 15U] >> 3U);
            const uint32_t s1 = ref_rotr(w[t - 2U], 17U)
                ^ ref_rotr(w[t - 2U], 19U) ^ (w[t - 2U] >> 10U);
            w[t] = w[t - 16U] + s0 + w[t - 7U] + s1;
        }
        a = h[0]; b = h[1]; c = h[2]; d = h[3];
        e = h[4]; f = h[5]; g = h[6]; hh = h[7];
        for (t = 0U; t < 64U; ++t) {
            const uint32_t big1 =
                ref_rotr(e, 6U) ^ ref_rotr(e, 11U) ^ ref_rotr(e, 25U);
            const uint32_t ch = (e & f) ^ ((~e) & g);
            const uint32_t t1 = hh + big1 + ch + k[t] + w[t];
            const uint32_t big0 =
                ref_rotr(a, 2U) ^ ref_rotr(a, 13U) ^ ref_rotr(a, 22U);
            const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            const uint32_t t2 = big0 + maj;
            hh = g; g = f; f = e; e = d + t1;
            d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d;
        h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }

    for (i = 0U; i < 8U; ++i) {
        out[i * 4U] = (uint8_t)(h[i] >> 24U);
        out[i * 4U + 1U] = (uint8_t)(h[i] >> 16U);
        out[i * 4U + 2U] = (uint8_t)(h[i] >> 8U);
        out[i * 4U + 3U] = (uint8_t)h[i];
    }
}

static void test_reference_kat(void)
{
    /* NIST FIPS 180-4 anchors so the reference itself is trustworthy. */
    static const uint8_t abc_expected[32] = {
        0xbaU, 0x78U, 0x16U, 0xbfU, 0x8fU, 0x01U, 0xcfU, 0xeaU,
        0x41U, 0x41U, 0x40U, 0xdeU, 0x5dU, 0xaeU, 0x22U, 0x23U,
        0xb0U, 0x03U, 0x61U, 0xa3U, 0x96U, 0x17U, 0x7aU, 0x9cU,
        0xb4U, 0x10U, 0xffU, 0x61U, 0xf2U, 0x00U, 0x15U, 0xadU
    };
    static const uint8_t empty_expected[32] = {
        0xe3U, 0xb0U, 0xc4U, 0x42U, 0x98U, 0xfcU, 0x1cU, 0x14U,
        0x9aU, 0xfbU, 0xf4U, 0xc8U, 0x99U, 0x6fU, 0xb9U, 0x24U,
        0x27U, 0xaeU, 0x41U, 0xe4U, 0x64U, 0x9bU, 0x93U, 0x4cU,
        0xa4U, 0x95U, 0x99U, 0x1bU, 0x78U, 0x52U, 0xb8U, 0x55U
    };
    uint8_t digest[32];
    ref_sha256((const uint8_t *)"abc", 3U, digest);
    assert(memcmp(digest, abc_expected, 32U) == 0);
    ref_sha256((const uint8_t *)"", 0U, digest);
    assert(memcmp(digest, empty_expected, 32U) == 0);

    /* And the production one-shot must match the same anchors. */
    sdb_sha256((const uint8_t *)"abc", 3U, digest);
    assert(memcmp(digest, abc_expected, 32U) == 0);
    sdb_sha256((const uint8_t *)"", 0U, digest);
    assert(memcmp(digest, empty_expected, 32U) == 0);
}

static void test_differential(void)
{
    uint8_t input[2049];
    uint8_t produced[32];
    uint8_t reference[32];
    size_t len;
    size_t i;
    for (i = 0U; i < sizeof(input); ++i) {
        input[i] = (uint8_t)((i * 131U + 7U) & 0xffU);
    }
    for (len = 0U; len <= 2048U; ++len) {
        sdb_sha256(input, len, produced);
        ref_sha256(input, len, reference);
        assert(memcmp(produced, reference, 32U) == 0);
    }
}

int main(void)
{
    const sdb_cpu_features *cpu = sdb_cpu_features_get();
    (void)printf(
        "test_sha256_simd: backend sha_ni=%d (SDB_NO_SIMD forces scalar)\n",
        (int)cpu->sha
    );
    test_reference_kat();
    test_differential();
    (void)printf("sha256 simd tests: ok\n");
    return 0;
}
