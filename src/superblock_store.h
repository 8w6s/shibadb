#ifndef SHIBADB_SUPERBLOCK_STORE_H
#define SHIBADB_SUPERBLOCK_STORE_H

#include "file.h"

sdb_status sdb_superblock_store_read_file(
    sdb_file *file, sdb_superblock_read_result *result_out
);
sdb_status sdb_superblock_store_heal_file(
    sdb_file *file,
    const sdb_superblock_read_result *current,
    const uint8_t *data_key
);
sdb_status sdb_superblock_store_read_authenticated_file(
    sdb_file *file,
    const uint8_t data_key[32],
    sdb_superblock_read_result *result_out
);
sdb_status sdb_superblock_store_create_authenticated(
    const char *path,
    const sdb_superblock_v1 *superblock,
    const uint8_t data_key[32]
);
sdb_status sdb_superblock_store_update_file(
    sdb_file *file, const sdb_superblock_v1 *superblock
);
sdb_status sdb_superblock_store_update_file_authenticated(
    sdb_file *file,
    const sdb_superblock_v1 *superblock,
    const uint8_t data_key[32]
);

#endif
