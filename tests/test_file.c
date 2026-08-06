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

    assert(sdb_file_resolve_database_path(".", false, &resolved)
        == SDB_E_INVALID_ARGUMENT);
    assert(resolved == NULL);
    assert(sdb_file_resolve_database_path("..", false, &resolved)
        == SDB_E_INVALID_ARGUMENT);
    assert(resolved == NULL);

    assert(sdb_file_resolve_database_path("/tmp/", false, &resolved)
        == SDB_E_INVALID_ARGUMENT);
    assert(resolved == NULL);

    assert(sdb_file_resolve_database_path("", false, &resolved)
        == SDB_E_INVALID_ARGUMENT);
    assert(resolved == NULL);

    assert(sdb_file_resolve_database_path(NULL, false, &resolved)
        == SDB_E_INVALID_ARGUMENT);
    assert(sdb_file_resolve_database_path("/tmp/x", false, NULL)
        == SDB_E_INVALID_ARGUMENT);

    assert(sdb_file_resolve_database_path(
        "/no/such/parent/dir/file", false, &resolved
    ) == SDB_E_IO);
    (void)puts("resolve_database_path corner cases: ok");
}

static void test_open_existing_missing_returns_not_found(void)
{

    sdb_file file;
    (void)remove("test-file-does-not-exist.tmp");
    assert(sdb_file_open_existing(
        "test-file-does-not-exist.tmp", false, &file
    ) == SDB_E_NOT_FOUND);

    assert(sdb_file_open_existing(
        "/no/such/parent/dir/file.tmp", false, &file
    ) == SDB_E_NOT_FOUND);
    (void)puts("open_existing missing returns not-found: ok");
}

static void test_create_new_existing_returns_conflict(void)
{
    sdb_file file;
    cleanup();
    assert(sdb_file_create_new(test_path, &file) == SDB_OK);
    assert(sdb_file_close(&file) == SDB_OK);
    /*
     * Creating onto an existing path must surface SDB_E_CONFLICT ("already
     * exists"), not a generic SDB_E_IO — a second app run / backup onto an
     * existing file must be distinguishable from a real disk error.
     */
    assert(sdb_file_create_new(test_path, &file) == SDB_E_CONFLICT);
    cleanup();
    (void)puts("create_new on existing returns conflict: ok");
}

static void test_injected_io_fault_surfaces(void)
{

    sdb_file file;
    uint8_t buffer[16];
    (void)memset(buffer, 0x5AU, sizeof(buffer));

    cleanup();
    assert(sdb_file_create_new(test_path, &file) == SDB_OK);
    assert(sdb_file_write_full(&file, 0U, buffer, sizeof(buffer)) == SDB_OK);
    assert(sdb_file_sync(&file) == SDB_OK);

    /* Arm the fault on the very next op; each primitive must surface IO. */
    sdb_file_fail_after_for_testing(&file, 0U);
    assert(sdb_file_write_full(&file, 0U, buffer, sizeof(buffer)) == SDB_E_IO);
    sdb_file_clear_failure_for_testing(&file);

    sdb_file_fail_after_for_testing(&file, 0U);
    assert(sdb_file_sync(&file) == SDB_E_IO);
    sdb_file_clear_failure_for_testing(&file);

    sdb_file_fail_after_for_testing(&file, 0U);
    assert(sdb_file_read_full(&file, 0U, buffer, sizeof(buffer)) == SDB_E_IO);
    sdb_file_clear_failure_for_testing(&file);

    /* Clearing the fault restores normal operation on the same handle. */
    assert(sdb_file_read_full(&file, 0U, buffer, sizeof(buffer)) == SDB_OK);
    assert(sdb_file_close(&file) == SDB_OK);
    cleanup();
    (void)puts("injected io fault surfaces SDB_E_IO: ok");
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
    test_create_new_existing_returns_conflict();
    test_injected_io_fault_surfaces();

    (void)puts("file tests: ok");
    return 0;
}
