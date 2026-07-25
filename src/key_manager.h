#ifndef SHIBADB_KEY_MANAGER_H
#define SHIBADB_KEY_MANAGER_H

#include "crypto.h"

sdb_status sdb_key_wrap(
    sdb_superblock_v1 *superblock,
    const uint8_t *password,
    size_t password_size,
    const uint8_t data_key[32]
);
sdb_status sdb_key_unwrap(
    const sdb_superblock_v1 *superblock,
    const uint8_t *password,
    size_t password_size,
    uint8_t data_key_out[32]
);

#endif
