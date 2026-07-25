#ifndef SHIBADB_INTERNAL_H
#define SHIBADB_INTERNAL_H

#include "shibadb.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SDB_SUPERBLOCK_MAGIC_SIZE ((size_t)8)
#define SDB_SUPERBLOCK_CHECKSUM_OFFSET ((size_t)88)

extern const uint8_t sdb_superblock_magic[SDB_SUPERBLOCK_MAGIC_SIZE];

bool sdb_checked_add_size(size_t left, size_t right, size_t *result);
bool sdb_checked_mul_size(size_t left, size_t right, size_t *result);

uint16_t sdb_read_u16_le(const uint8_t *input);
uint32_t sdb_read_u32_le(const uint8_t *input);
uint64_t sdb_read_u64_le(const uint8_t *input);
void sdb_write_u16_le(uint8_t *output, uint16_t value);
void sdb_write_u32_le(uint8_t *output, uint32_t value);
void sdb_write_u64_le(uint8_t *output, uint64_t value);

uint32_t sdb_crc32(const uint8_t *data, size_t size);
uint32_t sdb_crc32_zeroed_range(
    const uint8_t *data, size_t size, size_t zero_offset, size_t zero_size
);

#endif
