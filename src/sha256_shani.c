#include "sha256_simd.h"

#if SDB_CPU_X86

#include <immintrin.h>
#include <stdint.h>

/*
 * SHA-256 single-block compression using the Intel SHA extensions
 * (SHA-NI: sha256rnds2 / sha256msg1 / sha256msg2), plus SSSE3 (pshufb,
 * palignr) and SSE4.1 (pblendw). Structure follows the well-known public
 * reference by Sean Gulley / Jeffrey Walton, specialized to one 64-byte
 * block. Round constants are pulled from the shared sdb_sha256_constants
 * table (four per _mm_loadu_si128) rather than re-hardcoded, removing a
 * transcription hazard. The byte-swap mask converts each big-endian message
 * word to host order, matching the scalar path's sdb_read_u32_be, so output
 * is bit-identical to the scalar transform.
 */
void sdb_sha256_transform_shani(
    sdb_sha256_context *context, const uint8_t block[64]
)
{
    const __m128i shuffle_mask = _mm_set_epi64x(
        (long long)0x0c0d0e0f08090a0bULL, (long long)0x0405060700010203ULL
    );
    const uint32_t *k = sdb_sha256_constants;
    __m128i state0;
    __m128i state1;
    __m128i msg;
    __m128i tmp;
    __m128i msg0;
    __m128i msg1;
    __m128i msg2;
    __m128i msg3;
    __m128i abef_save;
    __m128i cdgh_save;

    /* Load current state (A B C D / E F G H) and shuffle into the
     * ABEF / CDGH register layout the SHA-NI rounds operate on. */
    tmp = _mm_loadu_si128((const __m128i *)(const void *)&context->state[0]);
    state1 = _mm_loadu_si128((const __m128i *)(const void *)&context->state[4]);
    tmp = _mm_shuffle_epi32(tmp, 0xB1);            /* CDAB */
    state1 = _mm_shuffle_epi32(state1, 0x1B);      /* EFGH -> HGFE */
    state0 = _mm_alignr_epi8(tmp, state1, 8);      /* ABEF */
    state1 = _mm_blend_epi16(state1, tmp, 0xF0);   /* CDGH */

    abef_save = state0;
    cdgh_save = state1;

    /* Rounds 0-3 */
    msg0 = _mm_shuffle_epi8(
        _mm_loadu_si128((const __m128i *)(const void *)(block + 0)), shuffle_mask
    );
    msg = _mm_add_epi32(
        msg0, _mm_loadu_si128((const __m128i *)(const void *)(k + 0))
    );
    state1 = _mm_sha256rnds2_epu32(state1, state0, msg);
    msg = _mm_shuffle_epi32(msg, 0x0E);
    state0 = _mm_sha256rnds2_epu32(state0, state1, msg);

    /* Rounds 4-7 */
    msg1 = _mm_shuffle_epi8(
        _mm_loadu_si128((const __m128i *)(const void *)(block + 16)), shuffle_mask
    );
    msg = _mm_add_epi32(
        msg1, _mm_loadu_si128((const __m128i *)(const void *)(k + 4))
    );
    state1 = _mm_sha256rnds2_epu32(state1, state0, msg);
    msg = _mm_shuffle_epi32(msg, 0x0E);
    state0 = _mm_sha256rnds2_epu32(state0, state1, msg);
    msg0 = _mm_sha256msg1_epu32(msg0, msg1);

    /* Rounds 8-11 */
    msg2 = _mm_shuffle_epi8(
        _mm_loadu_si128((const __m128i *)(const void *)(block + 32)), shuffle_mask
    );
    msg = _mm_add_epi32(
        msg2, _mm_loadu_si128((const __m128i *)(const void *)(k + 8))
    );
    state1 = _mm_sha256rnds2_epu32(state1, state0, msg);
    msg = _mm_shuffle_epi32(msg, 0x0E);
    state0 = _mm_sha256rnds2_epu32(state0, state1, msg);
    msg1 = _mm_sha256msg1_epu32(msg1, msg2);

    /* Rounds 12-15 */
    msg3 = _mm_shuffle_epi8(
        _mm_loadu_si128((const __m128i *)(const void *)(block + 48)), shuffle_mask
    );
    msg = _mm_add_epi32(
        msg3, _mm_loadu_si128((const __m128i *)(const void *)(k + 12))
    );
    state1 = _mm_sha256rnds2_epu32(state1, state0, msg);
    tmp = _mm_alignr_epi8(msg3, msg2, 4);
    msg0 = _mm_add_epi32(msg0, tmp);
    msg0 = _mm_sha256msg2_epu32(msg0, msg3);
    msg = _mm_shuffle_epi32(msg, 0x0E);
    state0 = _mm_sha256rnds2_epu32(state0, state1, msg);
    msg2 = _mm_sha256msg1_epu32(msg2, msg3);

    /* Rounds 16-19 */
    msg = _mm_add_epi32(
        msg0, _mm_loadu_si128((const __m128i *)(const void *)(k + 16))
    );
    state1 = _mm_sha256rnds2_epu32(state1, state0, msg);
    tmp = _mm_alignr_epi8(msg0, msg3, 4);
    msg1 = _mm_add_epi32(msg1, tmp);
    msg1 = _mm_sha256msg2_epu32(msg1, msg0);
    msg = _mm_shuffle_epi32(msg, 0x0E);
    state0 = _mm_sha256rnds2_epu32(state0, state1, msg);
    msg3 = _mm_sha256msg1_epu32(msg3, msg0);

    /* Rounds 20-23 */
    msg = _mm_add_epi32(
        msg1, _mm_loadu_si128((const __m128i *)(const void *)(k + 20))
    );
    state1 = _mm_sha256rnds2_epu32(state1, state0, msg);
    tmp = _mm_alignr_epi8(msg1, msg0, 4);
    msg2 = _mm_add_epi32(msg2, tmp);
    msg2 = _mm_sha256msg2_epu32(msg2, msg1);
    msg = _mm_shuffle_epi32(msg, 0x0E);
    state0 = _mm_sha256rnds2_epu32(state0, state1, msg);
    msg0 = _mm_sha256msg1_epu32(msg0, msg1);

    /* Rounds 24-27 */
    msg = _mm_add_epi32(
        msg2, _mm_loadu_si128((const __m128i *)(const void *)(k + 24))
    );
    state1 = _mm_sha256rnds2_epu32(state1, state0, msg);
    tmp = _mm_alignr_epi8(msg2, msg1, 4);
    msg3 = _mm_add_epi32(msg3, tmp);
    msg3 = _mm_sha256msg2_epu32(msg3, msg2);
    msg = _mm_shuffle_epi32(msg, 0x0E);
    state0 = _mm_sha256rnds2_epu32(state0, state1, msg);
    msg1 = _mm_sha256msg1_epu32(msg1, msg2);

    /* Rounds 28-31 */
    msg = _mm_add_epi32(
        msg3, _mm_loadu_si128((const __m128i *)(const void *)(k + 28))
    );
    state1 = _mm_sha256rnds2_epu32(state1, state0, msg);
    tmp = _mm_alignr_epi8(msg3, msg2, 4);
    msg0 = _mm_add_epi32(msg0, tmp);
    msg0 = _mm_sha256msg2_epu32(msg0, msg3);
    msg = _mm_shuffle_epi32(msg, 0x0E);
    state0 = _mm_sha256rnds2_epu32(state0, state1, msg);
    msg2 = _mm_sha256msg1_epu32(msg2, msg3);

    /* Rounds 32-35 */
    msg = _mm_add_epi32(
        msg0, _mm_loadu_si128((const __m128i *)(const void *)(k + 32))
    );
    state1 = _mm_sha256rnds2_epu32(state1, state0, msg);
    tmp = _mm_alignr_epi8(msg0, msg3, 4);
    msg1 = _mm_add_epi32(msg1, tmp);
    msg1 = _mm_sha256msg2_epu32(msg1, msg0);
    msg = _mm_shuffle_epi32(msg, 0x0E);
    state0 = _mm_sha256rnds2_epu32(state0, state1, msg);
    msg3 = _mm_sha256msg1_epu32(msg3, msg0);

    /* Rounds 36-39 */
    msg = _mm_add_epi32(
        msg1, _mm_loadu_si128((const __m128i *)(const void *)(k + 36))
    );
    state1 = _mm_sha256rnds2_epu32(state1, state0, msg);
    tmp = _mm_alignr_epi8(msg1, msg0, 4);
    msg2 = _mm_add_epi32(msg2, tmp);
    msg2 = _mm_sha256msg2_epu32(msg2, msg1);
    msg = _mm_shuffle_epi32(msg, 0x0E);
    state0 = _mm_sha256rnds2_epu32(state0, state1, msg);
    msg0 = _mm_sha256msg1_epu32(msg0, msg1);

    /* Rounds 40-43 */
    msg = _mm_add_epi32(
        msg2, _mm_loadu_si128((const __m128i *)(const void *)(k + 40))
    );
    state1 = _mm_sha256rnds2_epu32(state1, state0, msg);
    tmp = _mm_alignr_epi8(msg2, msg1, 4);
    msg3 = _mm_add_epi32(msg3, tmp);
    msg3 = _mm_sha256msg2_epu32(msg3, msg2);
    msg = _mm_shuffle_epi32(msg, 0x0E);
    state0 = _mm_sha256rnds2_epu32(state0, state1, msg);
    msg1 = _mm_sha256msg1_epu32(msg1, msg2);

    /* Rounds 44-47 */
    msg = _mm_add_epi32(
        msg3, _mm_loadu_si128((const __m128i *)(const void *)(k + 44))
    );
    state1 = _mm_sha256rnds2_epu32(state1, state0, msg);
    tmp = _mm_alignr_epi8(msg3, msg2, 4);
    msg0 = _mm_add_epi32(msg0, tmp);
    msg0 = _mm_sha256msg2_epu32(msg0, msg3);
    msg = _mm_shuffle_epi32(msg, 0x0E);
    state0 = _mm_sha256rnds2_epu32(state0, state1, msg);
    msg2 = _mm_sha256msg1_epu32(msg2, msg3);

    /* Rounds 48-51 */
    msg = _mm_add_epi32(
        msg0, _mm_loadu_si128((const __m128i *)(const void *)(k + 48))
    );
    state1 = _mm_sha256rnds2_epu32(state1, state0, msg);
    tmp = _mm_alignr_epi8(msg0, msg3, 4);
    msg1 = _mm_add_epi32(msg1, tmp);
    msg1 = _mm_sha256msg2_epu32(msg1, msg0);
    msg = _mm_shuffle_epi32(msg, 0x0E);
    state0 = _mm_sha256rnds2_epu32(state0, state1, msg);
    msg3 = _mm_sha256msg1_epu32(msg3, msg0);

    /* Rounds 52-55 */
    msg = _mm_add_epi32(
        msg1, _mm_loadu_si128((const __m128i *)(const void *)(k + 52))
    );
    state1 = _mm_sha256rnds2_epu32(state1, state0, msg);
    tmp = _mm_alignr_epi8(msg1, msg0, 4);
    msg2 = _mm_add_epi32(msg2, tmp);
    msg2 = _mm_sha256msg2_epu32(msg2, msg1);
    msg = _mm_shuffle_epi32(msg, 0x0E);
    state0 = _mm_sha256rnds2_epu32(state0, state1, msg);

    /* Rounds 56-59 */
    msg = _mm_add_epi32(
        msg2, _mm_loadu_si128((const __m128i *)(const void *)(k + 56))
    );
    state1 = _mm_sha256rnds2_epu32(state1, state0, msg);
    tmp = _mm_alignr_epi8(msg2, msg1, 4);
    msg3 = _mm_add_epi32(msg3, tmp);
    msg3 = _mm_sha256msg2_epu32(msg3, msg2);
    msg = _mm_shuffle_epi32(msg, 0x0E);
    state0 = _mm_sha256rnds2_epu32(state0, state1, msg);

    /* Rounds 60-63 */
    msg = _mm_add_epi32(
        msg3, _mm_loadu_si128((const __m128i *)(const void *)(k + 60))
    );
    state1 = _mm_sha256rnds2_epu32(state1, state0, msg);
    msg = _mm_shuffle_epi32(msg, 0x0E);
    state0 = _mm_sha256rnds2_epu32(state0, state1, msg);

    /* Fold the compressed block back into the saved state. */
    state0 = _mm_add_epi32(state0, abef_save);
    state1 = _mm_add_epi32(state1, cdgh_save);

    /* Unshuffle ABEF / CDGH back into A B C D / E F G H and store. */
    tmp = _mm_shuffle_epi32(state0, 0x1B);         /* FEBA */
    state1 = _mm_shuffle_epi32(state1, 0xB1);      /* DCHG */
    state0 = _mm_blend_epi16(tmp, state1, 0xF0);   /* DCBA */
    state1 = _mm_alignr_epi8(state1, tmp, 8);      /* ABEF -> HGFE */

    _mm_storeu_si128((__m128i *)(void *)&context->state[0], state0);
    _mm_storeu_si128((__m128i *)(void *)&context->state[4], state1);
}

#endif /* SDB_CPU_X86 */
