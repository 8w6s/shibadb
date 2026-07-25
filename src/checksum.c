#include "internal.h"

static uint32_t sdb_crc32_step(uint32_t crc, uint8_t byte)
{
    unsigned int bit;
    crc ^= (uint32_t)byte;
    for (bit = 0U; bit < 8U; ++bit) {
        const uint32_t mask = (uint32_t)(0U - (crc & UINT32_C(1)));
        crc = (crc >> 1U) ^ (UINT32_C(0xedb88320) & mask);
    }
    return crc;
}

uint32_t sdb_crc32(const uint8_t *data, size_t size)
{
    uint32_t crc = UINT32_C(0xffffffff);
    size_t index;
    for (index = 0U; index < size; ++index) {
        crc = sdb_crc32_step(crc, data[index]);
    }
    return ~crc;
}

uint32_t sdb_crc32_zeroed_range(
    const uint8_t *data, size_t size, size_t zero_offset, size_t zero_size
)
{
    uint32_t crc = UINT32_C(0xffffffff);
    size_t index;
    size_t zero_end;
    if (zero_offset > size || zero_size > size - zero_offset) {
        return UINT32_C(0);
    }
    zero_end = zero_offset + zero_size;
    for (index = 0U; index < size; ++index) {
        const uint8_t byte =
            index >= zero_offset && index < zero_end ? 0U : data[index];
        crc = sdb_crc32_step(crc, byte);
    }
    return ~crc;
}
