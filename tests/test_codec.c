#include "internal.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static void test_little_endian_roundtrip(void)
{
    uint8_t bytes[8];
    sdb_write_u16_le(bytes, UINT16_C(0x1234));
    assert(bytes[0] == UINT8_C(0x34));
    assert(bytes[1] == UINT8_C(0x12));
    assert(sdb_read_u16_le(bytes) == UINT16_C(0x1234));
    sdb_write_u32_le(bytes, UINT32_C(0x89abcdef));
    assert(bytes[0] == UINT8_C(0xef));
    assert(bytes[1] == UINT8_C(0xcd));
    assert(bytes[2] == UINT8_C(0xab));
    assert(bytes[3] == UINT8_C(0x89));
    assert(sdb_read_u32_le(bytes) == UINT32_C(0x89abcdef));
    sdb_write_u64_le(bytes, UINT64_C(0x0123456789abcdef));
    assert(bytes[0] == UINT8_C(0xef));
    assert(bytes[1] == UINT8_C(0xcd));
    assert(bytes[2] == UINT8_C(0xab));
    assert(bytes[3] == UINT8_C(0x89));
    assert(bytes[4] == UINT8_C(0x67));
    assert(bytes[5] == UINT8_C(0x45));
    assert(bytes[6] == UINT8_C(0x23));
    assert(bytes[7] == UINT8_C(0x01));
    assert(sdb_read_u64_le(bytes) == UINT64_C(0x0123456789abcdef));
}

static void test_little_endian_read_from_fixed_bytes(void)
{
    static const uint8_t fixed[8] = {
        0x11U, 0x22U, 0x33U, 0x44U, 0x55U, 0x66U, 0x77U, 0x88U
    };
    assert(sdb_read_u16_le(fixed) == UINT16_C(0x2211));
    assert(sdb_read_u32_le(fixed) == UINT32_C(0x44332211));
    assert(sdb_read_u64_le(fixed) == UINT64_C(0x8877665544332211));
}

static void test_checked_arithmetic(void)
{
    size_t result = 0U;
    assert(sdb_checked_add_size(10U, 20U, &result));
    assert(result == 30U);
    assert(sdb_checked_add_size(SIZE_MAX, 0U, &result));
    assert(result == SIZE_MAX);
    assert(!sdb_checked_add_size(SIZE_MAX, 1U, &result));
    assert(!sdb_checked_add_size(1U, SIZE_MAX, &result));
    assert(sdb_checked_mul_size(10U, 20U, &result));
    assert(result == 200U);
    assert(sdb_checked_mul_size(0U, SIZE_MAX, &result));
    assert(result == 0U);
    assert(sdb_checked_mul_size(SIZE_MAX, 0U, &result));
    assert(result == 0U);
    assert(sdb_checked_mul_size(SIZE_MAX, 1U, &result));
    assert(result == SIZE_MAX);
    assert(!sdb_checked_mul_size(SIZE_MAX, 2U, &result));
    assert(!sdb_checked_mul_size(2U, SIZE_MAX, &result));
    assert(!sdb_checked_add_size(1U, 1U, NULL));
    assert(!sdb_checked_mul_size(1U, 1U, NULL));
}

static void test_crc32_known_vector(void)
{
    static const uint8_t input[] = {
        (uint8_t)'1', (uint8_t)'2', (uint8_t)'3',
        (uint8_t)'4', (uint8_t)'5', (uint8_t)'6',
        (uint8_t)'7', (uint8_t)'8', (uint8_t)'9'
    };
    assert(sdb_crc32(input, sizeof(input)) == UINT32_C(0xcbf43926));
    /* Empty input: init ^ xorout of CRC-32/ISO-HDLC is 0. */
    assert(sdb_crc32(input, 0U) == UINT32_C(0x00000000));
}

/*
 * Independent bit-at-a-time reference (the literal definition of
 * CRC-32/ISO-HDLC, poly 0xEDB88320). Any table-based implementation — the
 * byte-at-a-time table or the slice-by-8 variant — MUST agree with this for
 * every length and every zeroed sub-range, or an on-disk checksum would shift
 * and silently reject valid data / accept corrupt data. This is the gate that
 * makes the slice-by-8 rewrite safe: it is byte-identical iff these pass.
 */
static uint32_t ref_crc32_bitwise(const uint8_t *data, size_t size)
{
    uint32_t crc = UINT32_C(0xffffffff);
    size_t i;
    unsigned bit;
    for (i = 0U; i < size; ++i) {
        crc ^= data[i];
        for (bit = 0U; bit < 8U; ++bit) {
            crc = (crc >> 1U) ^ ((crc & 1U) ? UINT32_C(0xedb88320) : 0U);
        }
    }
    return ~crc;
}

static uint32_t ref_crc32_zeroed_bitwise(
    const uint8_t *data, size_t size, size_t zero_offset, size_t zero_size
)
{
    uint32_t crc = UINT32_C(0xffffffff);
    size_t i;
    unsigned bit;
    size_t zero_end;
    /* Mirror the implementation's out-of-range guard exactly. */
    if (zero_offset > size || zero_size > size - zero_offset) {
        return UINT32_C(0);
    }
    zero_end = zero_offset + zero_size;
    for (i = 0U; i < size; ++i) {
        const uint8_t byte =
            (i >= zero_offset && i < zero_end) ? 0U : data[i];
        crc ^= byte;
        for (bit = 0U; bit < 8U; ++bit) {
            crc = (crc >> 1U) ^ ((crc & 1U) ? UINT32_C(0xedb88320) : 0U);
        }
    }
    return ~crc;
}

/*
 * Exhaustive differential check. Lengths 0..512 cover every slice-by-8
 * boundary (8-byte main loop + 0..7-byte tail) and the byte-table path alike.
 * The data pattern is deterministic so a failure is reproducible.
 */
static void test_crc32_differential_plain(void)
{
    static uint8_t buffer[512];
    size_t size;
    for (size = 0U; size < sizeof(buffer); ++size) {
        buffer[size] = (uint8_t)((size * 31U + 7U) & 0xffU);
    }
    for (size = 0U; size <= sizeof(buffer); ++size) {
        assert(sdb_crc32(buffer, size) == ref_crc32_bitwise(buffer, size));
    }
}

/*
 * Differential check of the zeroed-range variant across a matrix of
 * (size, zero_offset, zero_size) — including the degenerate cases the WAL and
 * page codecs actually use (a 4-byte checksum hole), zero-length holes, holes
 * at the very start/end, and holes spanning the whole buffer.
 */
static void test_crc32_differential_zeroed(void)
{
    static uint8_t buffer[260];
    size_t size;
    size_t zero_offset;
    size_t zero_size;
    for (size = 0U; size < sizeof(buffer); ++size) {
        buffer[size] = (uint8_t)((size * 131U + 17U) & 0xffU);
    }
    for (size = 0U; size <= sizeof(buffer); size += 1U) {
        for (zero_offset = 0U; zero_offset <= size; ++zero_offset) {
            static const size_t hole_sizes[] = {0U, 1U, 4U, 8U, 33U};
            size_t idx;
            for (idx = 0U; idx < sizeof(hole_sizes) / sizeof(hole_sizes[0]);
                 ++idx) {
                zero_size = hole_sizes[idx];
                assert(
                    sdb_crc32_zeroed_range(buffer, size, zero_offset, zero_size)
                    == ref_crc32_zeroed_bitwise(
                           buffer, size, zero_offset, zero_size
                       )
                );
            }
            /* Hole running exactly to the end. */
            assert(
                sdb_crc32_zeroed_range(
                    buffer, size, zero_offset, size - zero_offset
                )
                == ref_crc32_zeroed_bitwise(
                       buffer, size, zero_offset, size - zero_offset
                   )
            );
        }
    }
    /* Out-of-range guard: both must return 0, not read past the buffer. */
    assert(sdb_crc32_zeroed_range(buffer, 10U, 11U, 0U) == 0U);
    assert(sdb_crc32_zeroed_range(buffer, 10U, 5U, 6U) == 0U);
}

int main(void)
{
    test_little_endian_roundtrip();
    test_little_endian_read_from_fixed_bytes();
    test_checked_arithmetic();
    test_crc32_known_vector();
    test_crc32_differential_plain();
    test_crc32_differential_zeroed();
    (void)puts("codec tests: ok");
    return 0;
}
