#ifndef SHIBADB_CHACHA_SIMD_H
#define SHIBADB_CHACHA_SIMD_H

#include "cpu_features.h"

#include <stddef.h>
#include <stdint.h>

#if SDB_CPU_X86
/* AVX2 ChaCha20 keystream: processes `groups` groups of 8 blocks (512 bytes
 * each) in parallel, XORing the keystream for counters counter..counter+8*groups-1
 * into input -> output. Byte-identical to the scalar block loop. The caller
 * handles any trailing bytes (< 512) with the scalar path. Only call when
 * sdb_cpu_features_get()->avx2 is set. */
void sdb_chacha8_xor(
    const uint8_t key[32], const uint8_t nonce[12], uint32_t counter,
    const uint8_t *input, uint8_t *output, size_t groups
);
#endif

#endif
