#include "chacha_simd.h"

#if SDB_CPU_X86

#include <immintrin.h>
#include <stdint.h>

/*
 * AVX2 ChaCha20 keystream, 8 blocks in parallel ("vertical" SIMD): each of the
 * 16 state words is held in one __m256i whose 8 lanes are that word for blocks
 * counter+0 .. counter+7. The 20 rounds are then plain add/xor/rotate on those
 * registers with no in-round shuffles (the diagonal quarter-rounds just pick
 * different registers). After adding the initial state, the 16 words per block
 * are gathered and XORed into the output. The gather/XOR tail is done scalarly
 * — it is cheap next to the rounds and removes any risk of a mis-transposed
 * SIMD store, keeping the result provably byte-identical to the scalar block.
 */

#define SDB_ROTV(x, n) \
    _mm256_or_si256(_mm256_slli_epi32((x), (n)), _mm256_srli_epi32((x), 32 - (n)))

#define SDB_QR(a, b, c, d) \
    (a) = _mm256_add_epi32((a), (b)); (d) = _mm256_xor_si256((d), (a)); \
    (d) = SDB_ROTV((d), 16); \
    (c) = _mm256_add_epi32((c), (d)); (b) = _mm256_xor_si256((b), (c)); \
    (b) = SDB_ROTV((b), 12); \
    (a) = _mm256_add_epi32((a), (b)); (d) = _mm256_xor_si256((d), (a)); \
    (d) = SDB_ROTV((d), 8); \
    (c) = _mm256_add_epi32((c), (d)); (b) = _mm256_xor_si256((b), (c)); \
    (b) = SDB_ROTV((b), 7)

static uint32_t sdb_rd32le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8U)
        | ((uint32_t)p[2] << 16U) | ((uint32_t)p[3] << 24U);
}

static void sdb_wr32le(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8U);
    p[2] = (uint8_t)(v >> 16U);
    p[3] = (uint8_t)(v >> 24U);
}

void sdb_chacha8_xor(
    const uint8_t key[32], const uint8_t nonce[12], uint32_t counter,
    const uint8_t *input, uint8_t *output, size_t groups
)
{
    const __m256i lane = _mm256_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7);
    __m256i init[16];
    size_t g;
    size_t i;
    size_t j;

    init[0] = _mm256_set1_epi32((int)0x61707865U);
    init[1] = _mm256_set1_epi32((int)0x3320646eU);
    init[2] = _mm256_set1_epi32((int)0x79622d32U);
    init[3] = _mm256_set1_epi32((int)0x6b206574U);
    for (i = 0U; i < 8U; ++i) {
        init[4U + i] = _mm256_set1_epi32((int)sdb_rd32le(key + i * 4U));
    }
    init[13] = _mm256_set1_epi32((int)sdb_rd32le(nonce));
    init[14] = _mm256_set1_epi32((int)sdb_rd32le(nonce + 4U));
    init[15] = _mm256_set1_epi32((int)sdb_rd32le(nonce + 8U));

    for (g = 0U; g < groups; ++g) {
        __m256i v[16];
        uint32_t ks[16][8];
        unsigned r;
        const size_t base = g * 512U;

        init[12] = _mm256_add_epi32(_mm256_set1_epi32((int)counter), lane);
        for (i = 0U; i < 16U; ++i) {
            v[i] = init[i];
        }
        for (r = 0U; r < 10U; ++r) {
            SDB_QR(v[0], v[4], v[8], v[12]);
            SDB_QR(v[1], v[5], v[9], v[13]);
            SDB_QR(v[2], v[6], v[10], v[14]);
            SDB_QR(v[3], v[7], v[11], v[15]);
            SDB_QR(v[0], v[5], v[10], v[15]);
            SDB_QR(v[1], v[6], v[11], v[12]);
            SDB_QR(v[2], v[7], v[8], v[13]);
            SDB_QR(v[3], v[4], v[9], v[14]);
        }
        for (i = 0U; i < 16U; ++i) {
            v[i] = _mm256_add_epi32(v[i], init[i]);
            _mm256_storeu_si256((__m256i *)(void *)ks[i], v[i]);
        }
        for (j = 0U; j < 8U; ++j) {
            uint8_t keystream[64];
            const size_t off = base + j * 64U;
            for (i = 0U; i < 16U; ++i) {
                sdb_wr32le(keystream + i * 4U, ks[i][j]);
            }
            for (i = 0U; i < 64U; ++i) {
                output[off + i] = (uint8_t)(input[off + i] ^ keystream[i]);
            }
        }
        counter += 8U;
    }
}

#endif /* SDB_CPU_X86 */
