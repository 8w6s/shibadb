#ifndef SHIBADB_REPLACE_H
#define SHIBADB_REPLACE_H

#include "shibadb.h"

sdb_status sdb_replace_prepare(
    const char *database_path,
    const uint8_t replacement_file_id[SDB_FILE_ID_SIZE]
);
sdb_status sdb_replace_finish(const char *database_path);
sdb_status sdb_replace_abort(const char *database_path);
sdb_status sdb_replace_recover(
    const char *database_path,
    const uint8_t current_file_id[SDB_FILE_ID_SIZE]
);

#endif
