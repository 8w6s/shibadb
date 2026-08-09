#ifndef SHIBADB_SHA256_SIMD_H
#define SHIBADB_SHA256_SIMD_H

#include "cpu_features.h"
#include "crypto.h"

/* The round constants live in sha256.c; the SHA-NI kernel loads them directly
 * (as __m128i groups of four) so both backends share one source of truth. */
extern const uint32_t sdb_sha256_constants[64];

#if SDB_CPU_X86
/* Hardware SHA-256 single-block compression via the Intel SHA extensions.
 * Same contract as the scalar transform: folds one 64-byte block into
 * context->state[8]. Only call when sdb_cpu_features_get()->sha is set. */
void sdb_sha256_transform_shani(
    sdb_sha256_context *context, const uint8_t block[64]
);
#endif

#endif
