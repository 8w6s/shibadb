/*
 * Byte-identical gate for the CRC-32 backend (slice-by-8 vs PCLMULQDQ).
 *
 * Same idea as test_codec.c's CRC differential, widened to exercise every
 * PCLMULQDQ fold path: lengths 0..2048 (covers the <16 tail, the 16..63
 * single-fold, and the >=64 fold-by-4 loop) plus large sizes (4096, odd 8191,
 * 65535) so the multi-block loop runs many iterations. The independent
 * bit-at-a-time reference (poly 0xEDB88320, reflected) anchors correctness so
 * a shared blind spot can't slip through. sdb_crc32_zeroed_range is checked
 * against a reference that literally zeroes the hole. Run with SDB_NO_SIMD=1
 * to force and gate the scalar slice-by-8 path too.
 */

#include "cpu_features.h"
#include "internal.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t ref_crc32(const uint8_t *data, size_t len)
{
    uint32_t crc = UINT32_C(0xffffffff);
    size_t i;
    unsigned bit;
    for (i = 0U; i < len; ++i) {
        crc ^= (uint32_t)data[i];
        for (bit = 0U; bit < 8U; ++bit) {
            if ((crc & 1U) != 0U) {
                crc = (crc >> 1) ^ UINT32_C(0xedb88320);
            } else {
                crc = crc >> 1;
            }
        }
    }
    return ~crc;
}

static void test_kat(void)
{
    assert(sdb_crc32((const uint8_t *)"123456789", 9U) == UINT32_C(0xcbf43926));
    assert(sdb_crc32((const uint8_t *)"", 0U) == UINT32_C(0));
    assert(ref_crc32((const uint8_t *)"123456789", 9U) == UINT32_C(0xcbf43926));
}

static void test_differential(uint8_t *buffer, size_t len)
{
    const uint32_t produced = sdb_crc32(buffer, len);
    const uint32_t reference = ref_crc32(buffer, len);
    assert(produced == reference);
}

static void test_zeroed(uint8_t *buffer, size_t len)
{
    /* Compare sdb_crc32_zeroed_range against a reference that physically
     * zeroes the hole then runs the plain reference CRC. */
    size_t offset;
    size_t hole;
    for (offset = 0U; offset <= len; offset += (len / 7U) + 1U) {
        for (hole = 0U; offset + hole <= len; hole += (len / 5U) + 1U) {
            uint8_t *copy = (uint8_t *)malloc(len == 0U ? 1U : len);
            uint32_t got;
            uint32_t want;
            assert(copy != NULL);
            if (len != 0U) {
                (void)memcpy(copy, buffer, len);
            }
            if (hole != 0U) {
                (void)memset(copy + offset, 0, hole);
            }
            got = sdb_crc32_zeroed_range(buffer, len, offset, hole);
            want = ref_crc32(copy, len);
            assert(got == want);
            free(copy);
        }
    }
}

int main(void)
{
    const sdb_cpu_features *cpu = sdb_cpu_features_get();
    const size_t big_sizes[3] = { 4096U, 8191U, 65535U };
    uint8_t *buffer = (uint8_t *)malloc(65536U);
    size_t len;
    size_t i;

    assert(buffer != NULL);
    for (i = 0U; i < 65536U; ++i) {
        buffer[i] = (uint8_t)((i * 137U + 29U) & 0xffU);
    }
    (void)printf(
        "test_crc32_simd: backend pclmulqdq=%d (SDB_NO_SIMD forces scalar)\n",
        (int)cpu->pclmulqdq
    );

    test_kat();
    for (len = 0U; len <= 2048U; ++len) {
        test_differential(buffer, len);
    }
    for (i = 0U; i < 3U; ++i) {
        test_differential(buffer, big_sizes[i]);
    }
    for (len = 0U; len <= 300U; len += 17U) {
        test_zeroed(buffer, len);
    }
    test_zeroed(buffer, 4096U);

    free(buffer);
    (void)printf("crc32 simd tests: ok\n");
    return 0;
}
