#include "internal.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static void test_little_endian_roundtrip(void)
{
    uint8_t bytes[8];
    sdb_write_u16_le(bytes, UINT16_C(0x1234));
    assert(sdb_read_u16_le(bytes) == UINT16_C(0x1234));
    sdb_write_u32_le(bytes, UINT32_C(0x89abcdef));
    assert(sdb_read_u32_le(bytes) == UINT32_C(0x89abcdef));
    sdb_write_u64_le(bytes, UINT64_C(0x0123456789abcdef));
    assert(sdb_read_u64_le(bytes) == UINT64_C(0x0123456789abcdef));
}

static void test_checked_arithmetic(void)
{
    size_t result = 0U;
    assert(sdb_checked_add_size(10U, 20U, &result));
    assert(result == 30U);
    assert(!sdb_checked_add_size(SIZE_MAX, 1U, &result));
    assert(sdb_checked_mul_size(10U, 20U, &result));
    assert(result == 200U);
    assert(!sdb_checked_mul_size(SIZE_MAX, 2U, &result));
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
}

int main(void)
{
    test_little_endian_roundtrip();
    test_checked_arithmetic();
    test_crc32_known_vector();
    (void)puts("codec tests: ok");
    return 0;
}

