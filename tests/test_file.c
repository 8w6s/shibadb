#include "file.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static const char *test_path = "test-file-io.tmp";

static void cleanup(void)
{
    (void)remove(test_path);
}

static void test_resolve_database_path_rejects_special(void)
{
    char *resolved = NULL;
    /*
     * "." and ".." would resolve to a directory, which is not a valid
     * database file — check the file-name-level rejection.
     */
    assert(sdb_file_resolve_database_path(".", false, &resolved)
        == SDB_E_INVALID_ARGUMENT);
    assert(resolved == NULL);
    assert(sdb_file_resolve_database_path("..", false, &resolved)
        == SDB_E_INVALID_ARGUMENT);
    assert(resolved == NULL);
    /* Trailing slash = directory, also rejected. */
    assert(sdb_file_resolve_database_path("/tmp/", false, &resolved)
        == SDB_E_INVALID_ARGUMENT);
    assert(resolved == NULL);
    /* Empty path is rejected at the top. */
    assert(sdb_file_resolve_database_path("", false, &resolved)
        == SDB_E_INVALID_ARGUMENT);
    assert(resolved == NULL);
    /* NULL args. */
    assert(sdb_file_resolve_database_path(NULL, false, &resolved)
        == SDB_E_INVALID_ARGUMENT);
    assert(sdb_file_resolve_database_path("/tmp/x", false, NULL)
        == SDB_E_INVALID_ARGUMENT);
    /* Non-existent parent dir (must_exist=false so name-part alone is fine
     * but directory realpath fails). */
    assert(sdb_file_resolve_database_path(
        "/no/such/parent/dir/file", false, &resolved
    ) == SDB_E_IO);
    (void)puts("resolve_database_path corner cases: ok");
}

static void test_open_existing_missing_returns_not_found(void)
{
    /*
     * A caller that wants to distinguish "file does not exist yet"
     * from "real I/O error" (e.g. sdb_wal_recover, which converts the
     * first to a no-op replay and must propagate the second) needs
     * sdb_file_open_existing to surface ENOENT / ERROR_FILE_NOT_FOUND
     * with a distinct status. Before this change, every open failure
     * collapsed to SDB_E_IO, so a wal_recover call against a WAL that
     * was unreadable due to EACCES/EMFILE/EIO looked identical to a
     * healthy "no WAL yet" open — silently dropping any fsynced
     * transactions still on that WAL.
     */
    sdb_file file;
    (void)remove("test-file-does-not-exist.tmp");
    assert(sdb_file_open_existing(
        "test-file-does-not-exist.tmp", false, &file
    ) == SDB_E_NOT_FOUND);
    /* A missing parent directory is also an ENOENT surface. */
    assert(sdb_file_open_existing(
        "/no/such/parent/dir/file.tmp", false, &file
    ) == SDB_E_NOT_FOUND);
    (void)puts("open_existing missing returns not-found: ok");
}

int main(void)
{
    sdb_file file;
    uint8_t input[257];
    uint8_t output[257];
    uint64_t size = 0U;
    size_t index;

    cleanup();
    for (index = 0U; index < sizeof(input); ++index) {
        input[index] = (uint8_t)(index & 0xffU);
    }
    (void)memset(output, 0, sizeof(output));

    assert(sdb_file_create_new(test_path, &file) == SDB_OK);
    sdb_file_set_io_limit_for_testing(&file, 7U);
    assert(sdb_file_write_full(&file, 13U, input, sizeof(input)) == SDB_OK);
    assert(sdb_file_sync(&file) == SDB_OK);
    assert(sdb_file_size(&file, &size) == SDB_OK);
    assert(size == 13U + (uint64_t)sizeof(input));
    assert(sdb_file_read_full(&file, 13U, output, sizeof(output)) == SDB_OK);
    assert(memcmp(input, output, sizeof(input)) == 0);
    assert(sdb_file_read_full(&file, size, output, 1U) == SDB_E_TRUNCATED);
    assert(sdb_file_resize(&file, 64U) == SDB_OK);
    assert(sdb_file_size(&file, &size) == SDB_OK);
    assert(size == 64U);
    assert(sdb_file_close(&file) == SDB_OK);
    cleanup();

    test_resolve_database_path_rejects_special();
    test_open_existing_missing_returns_not_found();

    (void)puts("file tests: ok");
    return 0;
}

