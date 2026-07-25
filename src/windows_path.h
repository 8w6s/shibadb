#ifndef SHIBADB_WINDOWS_PATH_H
#define SHIBADB_WINDOWS_PATH_H

#ifdef _WIN32

#include "shibadb.h"

#include <wchar.h>

sdb_status sdb_windows_path_from_utf8(
    const char *path, wchar_t **wide_path_out
);
sdb_status sdb_windows_path_to_utf8(
    const wchar_t *wide_path, char **path_out
);

#endif

#endif
