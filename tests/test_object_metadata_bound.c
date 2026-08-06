/*
 * Whitebox regression for BUG#5 (defense-in-depth hardening).
 *
 * sdb_object_metadata_decode validated the chunk_count == ceil(total_size /
 * chunk_size) relationship but never bounded chunk_size against the real
 * ceiling. The write path (sdb_object_plan) always clamps chunk_size to
 * SDB_OBJECT_CHUNK_TARGET, so no legitimately produced object can carry a
 * larger value. A tampered, unencrypted object could however declare an
 * arbitrary chunk_size (e.g. 0xFFFFFFFF with total_size=1, chunk_count=1) that
 * still satisfies the ceiling relationship, feeding an oversized allocation to
 * sdb_verify_object.
 *
 * The decoder is static, so we include the translation unit directly to reach
 * it. CMake links the remaining core sources (everything except engine.c) into
 * this target, so there is no duplicate-symbol conflict. Values stay just above
 * the clamp (3000 / 2049) to prove the bound without risking an OOM.
 */
#include "../src/engine.c"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

static sdb_status decode_with_chunk_size(
    uint32_t chunk_size,
    uint64_t total_size,
    uint32_t chunk_count
)
{
    sdb_object_metadata source;
    sdb_object_metadata decoded;
    uint8_t encoded[SDB_OBJECT_METADATA_SIZE];
    (void)memset(&source, 0, sizeof(source));
    source.kind = (uint16_t)SDB_OBJECT_KV;
    source.generation = UINT64_C(1);
    source.total_size = total_size;
    source.chunk_count = chunk_count;
    source.chunk_size = chunk_size;
    /* digest stays zeroed; the decoder does not validate it. */
    sdb_object_metadata_encode(&source, encoded);
    return sdb_object_metadata_decode(
        encoded, sizeof(encoded), (uint16_t)SDB_OBJECT_KV, &decoded
    );
}

int main(void)
{
    /*
     * Regression proving BUG#5: a chunk_size the write path can never emit must
     * be rejected as corrupt. Before the fix the decoder returns SDB_OK here.
     */
    assert(decode_with_chunk_size(3000U, 1U, 1U) == SDB_E_CORRUPT);
    /* One byte above the clamp boundary is already out of range. */
    assert(decode_with_chunk_size(2049U, 1U, 1U) == SDB_E_CORRUPT);
    /* The clamp boundary and everything below it must still round-trip. */
    assert(decode_with_chunk_size(2048U, 2048U, 1U) == SDB_OK);
    assert(decode_with_chunk_size(1U, 1U, 1U) == SDB_OK);
    (void)puts("object metadata bound tests: ok");
    return 0;
}
