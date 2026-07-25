#include "internal.h"

#include <limits.h>

bool sdb_checked_add_size(size_t left, size_t right, size_t *result)
{
    if (result == NULL || right > SIZE_MAX - left) {
        return false;
    }
    *result = left + right;
    return true;
}

bool sdb_checked_mul_size(size_t left, size_t right, size_t *result)
{
    if (result == NULL || (left != 0U && right > SIZE_MAX / left)) {
        return false;
    }
    *result = left * right;
    return true;
}

uint16_t sdb_read_u16_le(const uint8_t *input)
{
    return (uint16_t)((uint16_t)input[0]
        | (uint16_t)((uint16_t)input[1] << 8U));
}

uint32_t sdb_read_u32_le(const uint8_t *input)
{
    return (uint32_t)((uint32_t)input[0]
        | ((uint32_t)input[1] << 8U)
        | ((uint32_t)input[2] << 16U)
        | ((uint32_t)input[3] << 24U));
}

uint64_t sdb_read_u64_le(const uint8_t *input)
{
    uint64_t value = 0U;
    unsigned int index;
    for (index = 0U; index < 8U; ++index) {
        value |= (uint64_t)input[index] << (index * 8U);
    }
    return value;
}

void sdb_write_u16_le(uint8_t *output, uint16_t value)
{
    output[0] = (uint8_t)(value & UINT16_C(0xff));
    output[1] = (uint8_t)((value >> 8U) & UINT16_C(0xff));
}

void sdb_write_u32_le(uint8_t *output, uint32_t value)
{
    unsigned int index;
    for (index = 0U; index < 4U; ++index) {
        output[index] = (uint8_t)((value >> (index * 8U)) & UINT32_C(0xff));
    }
}

void sdb_write_u64_le(uint8_t *output, uint64_t value)
{
    unsigned int index;
    for (index = 0U; index < 8U; ++index) {
        output[index] = (uint8_t)((value >> (index * 8U)) & UINT64_C(0xff));
    }
}

