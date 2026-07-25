#ifndef SHIBADB_SUPERBLOCK_STORE_H
#define SHIBADB_SUPERBLOCK_STORE_H

#include "file.h"

sdb_status sdb_superblock_store_read_file(
    sdb_file *file, sdb_superblock_read_result *result_out
);
sdb_status sdb_superblock_store_update_file(
    sdb_file *file, const sdb_superblock_v1 *superblock
);

#endif
