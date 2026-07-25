#include "windows_path.h"

#ifdef _WIN32

#include <windows.h>

#include <stdlib.h>

sdb_status sdb_windows_path_from_utf8(
    const char *path, wchar_t **wide_path_out
)
{
    int wide_size;
    wchar_t *wide_path;
    if (path == NULL || path[0] == '\0' || wide_path_out == NULL) {
        return SDB_E_INVALID_ARGUMENT;
    }
    *wide_path_out = NULL;
    wide_size = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, NULL, 0
    );
    if (wide_size <= 0
        || (size_t)wide_size > SIZE_MAX / sizeof(*wide_path)) {
        return SDB_E_INVALID_ARGUMENT;
    }
    wide_path = (wchar_t *)malloc(
        (size_t)wide_size * sizeof(*wide_path)
    );
    if (wide_path == NULL) {
        return SDB_E_INTERNAL;
    }
    if (MultiByteToWideChar(
            CP_UTF8,
            MB_ERR_INVALID_CHARS,
            path,
            -1,
            wide_path,
            wide_size
        ) != wide_size) {
        free(wide_path);
        return SDB_E_INVALID_ARGUMENT;
    }
    *wide_path_out = wide_path;
    return SDB_OK;
}

sdb_status sdb_windows_path_to_utf8(
    const wchar_t *wide_path, char **path_out
)
{
    int path_size;
    char *path;
    if (wide_path == NULL || wide_path[0] == L'\0' || path_out == NULL) {
        return SDB_E_INVALID_ARGUMENT;
    }
    *path_out = NULL;
    path_size = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, wide_path, -1, NULL, 0, NULL, NULL
    );
    if (path_size <= 0) {
        return SDB_E_INVALID_ARGUMENT;
    }
    path = (char *)malloc((size_t)path_size);
    if (path == NULL) {
        return SDB_E_INTERNAL;
    }
    if (WideCharToMultiByte(
            CP_UTF8,
            WC_ERR_INVALID_CHARS,
            wide_path,
            -1,
            path,
            path_size,
            NULL,
            NULL
        ) != path_size) {
        free(path);
        return SDB_E_INVALID_ARGUMENT;
    }
    *path_out = path;
    return SDB_OK;
}

#endif
