#include "windows_path.h"

typedef int sdb_windows_path_placeholder;

#ifdef _WIN32

#include <windows.h>

#include <limits.h>
#include <stdbool.h>
#include <stdlib.h>
#include <wchar.h>

static bool sdb_windows_path_is_drive_absolute(const wchar_t *path)
{
    return ((path[0] >= L'A' && path[0] <= L'Z')
            || (path[0] >= L'a' && path[0] <= L'z'))
        && path[1] == L':'
        && (path[2] == L'\\' || path[2] == L'/');
}

static sdb_status sdb_windows_path_add_extended_prefix(
    wchar_t **path_in_out
)
{
    static const wchar_t extended_prefix[] = L"\\\\?\\";
    static const wchar_t extended_unc_prefix[] = L"\\\\?\\UNC\\";
    wchar_t *path = *path_in_out;
    const wchar_t *source;
    const wchar_t *prefix;
    size_t prefix_length;
    size_t source_length;
    size_t total_length;
    wchar_t *extended;

    if (wcsncmp(path, L"\\\\?\\", 4U) == 0
        || wcsncmp(path, L"\\\\.\\", 4U) == 0) {
        return SDB_OK;
    }
    if (path[0] == L'\\' && path[1] == L'\\') {
        prefix = extended_unc_prefix;
        prefix_length = 8U;
        source = path + 2;
    } else if (sdb_windows_path_is_drive_absolute(path)) {
        prefix = extended_prefix;
        prefix_length = 4U;
        source = path;
    } else {
        return SDB_OK;
    }

    source_length = wcslen(source);
    if (source_length > SIZE_MAX - prefix_length - 1U) {
        return SDB_E_INVALID_ARGUMENT;
    }
    total_length = prefix_length + source_length + 1U;
    if (total_length > SIZE_MAX / sizeof(*extended)) {
        return SDB_E_INVALID_ARGUMENT;
    }
    extended = (wchar_t *)malloc(total_length * sizeof(*extended));
    if (extended == NULL) {
        return SDB_E_OUT_OF_MEMORY;
    }
    (void)wmemcpy(extended, prefix, prefix_length);
    (void)wmemcpy(
        extended + prefix_length, source, source_length + 1U
    );
    free(path);
    *path_in_out = extended;
    return SDB_OK;
}

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
        return SDB_E_OUT_OF_MEMORY;
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
    return sdb_windows_path_add_extended_prefix(wide_path_out);
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
        return SDB_E_OUT_OF_MEMORY;
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
