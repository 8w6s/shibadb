#include "file.h"
#include "shibadb_engine.h"
#ifdef _WIN32
#include "windows_path.h"
#endif

#include <assert.h>
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <stdlib.h>
#include <wchar.h>
#include <windows.h>
#endif

static const char database_path[] =
    "test-utf8-"
    "\xE6\x9F\xB4\xE7\x8A\xAC"
    ".tmp";
static const char wal_path[] =
    "test-utf8-"
    "\xE6\x9F\xB4\xE7\x8A\xAC"
    ".tmp.wal";
static const char lock_path[] =
    "test-utf8-"
    "\xE6\x9F\xB4\xE7\x8A\xAC"
    ".tmp.lock";

static void cleanup(void)
{
    assert(sdb_file_remove(wal_path, true) == SDB_OK);
    assert(sdb_file_remove(database_path, true) == SDB_OK);
    assert(sdb_file_remove(lock_path, true) == SDB_OK);
}

#ifdef _WIN32
static void test_extended_path_prefixes(void)
{
    wchar_t *path = NULL;

    assert(sdb_windows_path_from_utf8("relative\\db.sdb", &path) == SDB_OK);
    assert(wcscmp(path, L"relative\\db.sdb") == 0);
    free(path);

    assert(sdb_windows_path_from_utf8("C:\\deep\\db.sdb", &path) == SDB_OK);
    assert(wcscmp(path, L"\\\\?\\C:\\deep\\db.sdb") == 0);
    free(path);

    assert(sdb_windows_path_from_utf8(
        "\\\\server\\share\\db.sdb", &path
    ) == SDB_OK);
    assert(wcscmp(path, L"\\\\?\\UNC\\server\\share\\db.sdb") == 0);
    free(path);

    assert(sdb_windows_path_from_utf8(
        "\\\\?\\C:\\deep\\db.sdb", &path
    ) == SDB_OK);
    assert(wcscmp(path, L"\\\\?\\C:\\deep\\db.sdb") == 0);
    free(path);
}

static void test_extended_path_database(void)
{
    static const char segment[] = "\\long-path-segment-0123456789";
    char directory[2048];
    char database[2048];
    char sidecar[2060];
    size_t directory_lengths[16];
    size_t depth = 0U;
    DWORD current_size;
    wchar_t *wide = NULL;
    char *current_utf8 = NULL;
    sdb_database_options options;
    sdb_database *handle = NULL;

    current_size = GetCurrentDirectoryW(0U, NULL);
    assert(current_size != 0U);
    wide = (wchar_t *)malloc((size_t)current_size * sizeof(*wide));
    assert(wide != NULL);
    assert(GetCurrentDirectoryW(current_size, wide) != 0U);
    assert(sdb_windows_path_to_utf8(wide, &current_utf8) == SDB_OK);
    free(wide);
    assert(snprintf(
        directory, sizeof(directory), "%s\\shibadb-long-%lu",
        current_utf8, (unsigned long)GetCurrentProcessId()
    ) > 0);
    free(current_utf8);

    directory_lengths[depth++] = strlen(directory);
    assert(sdb_windows_path_from_utf8(directory, &wide) == SDB_OK);
    assert(CreateDirectoryW(wide, NULL) != 0
        || GetLastError() == ERROR_ALREADY_EXISTS);
    free(wide);

    while (strlen(directory) < 320U) {
        const size_t length = strlen(directory);
        assert(depth < sizeof(directory_lengths) / sizeof(directory_lengths[0]));
        assert(length + sizeof(segment) <= sizeof(directory));
        (void)memcpy(directory + length, segment, sizeof(segment));
        directory_lengths[depth++] = strlen(directory);
        assert(sdb_windows_path_from_utf8(directory, &wide) == SDB_OK);
        assert(CreateDirectoryW(wide, NULL) != 0
            || GetLastError() == ERROR_ALREADY_EXISTS);
        free(wide);
    }
    assert(snprintf(
        database, sizeof(database), "%s\\database.sdb", directory
    ) > 0);
    assert(strlen(database) > MAX_PATH);

    sdb_database_options_init(&options);
    assert(sdb_database_create(database, &options, &handle) == SDB_OK);
    assert(sdb_kv_put(
        handle, (const uint8_t *)"ns", 2U,
        (const uint8_t *)"key", 3U, (const uint8_t *)"value", 5U
    ) == SDB_OK);
    assert(sdb_database_close(handle) == SDB_OK);
    handle = NULL;
    assert(sdb_database_open(database, &options, &handle) == SDB_OK);
    assert(sdb_database_close(handle) == SDB_OK);

    assert(snprintf(sidecar, sizeof(sidecar), "%s.wal", database) > 0);
    assert(sdb_file_remove(sidecar, true) == SDB_OK);
    assert(snprintf(sidecar, sizeof(sidecar), "%s.lock", database) > 0);
    assert(sdb_file_remove(sidecar, true) == SDB_OK);
    assert(sdb_file_remove(database, false) == SDB_OK);
    while (depth != 0U) {
        --depth;
        directory[directory_lengths[depth]] = '\0';
        assert(sdb_windows_path_from_utf8(directory, &wide) == SDB_OK);
        assert(RemoveDirectoryW(wide) != 0);
        free(wide);
        if (depth != 0U) {
            directory[directory_lengths[depth - 1U]] = '\0';
        }
    }
}
#endif

int main(void)
{
    static const uint8_t namespace_name[] = "utf8-path";
    static const uint8_t key[] = "key";
    static const uint8_t value[] = "value";
    uint8_t actual[16];
    size_t actual_size = 0U;
    sdb_database_options options;
    sdb_database *database = NULL;

#ifdef _WIN32
    test_extended_path_prefixes();
    test_extended_path_database();
#endif

    cleanup();
    sdb_database_options_init(&options);
    assert(sdb_database_create(
        database_path, &options, &database
    ) == SDB_OK);
    assert(sdb_kv_put(
        database, namespace_name, sizeof(namespace_name),
        key, sizeof(key), value, sizeof(value)
    ) == SDB_OK);
    assert(sdb_database_close(database) == SDB_OK);
    database = NULL;
    assert(sdb_database_open(
        database_path, &options, &database
    ) == SDB_OK);
    assert(sdb_kv_get(
        database, namespace_name, sizeof(namespace_name),
        key, sizeof(key), actual, sizeof(actual), &actual_size
    ) == SDB_OK);
    assert(actual_size == sizeof(value));
    assert(memcmp(actual, value, sizeof(value)) == 0);
    assert(sdb_database_close(database) == SDB_OK);
    cleanup();
    (void)puts("UTF-8 path tests: ok");
    return 0;
}
