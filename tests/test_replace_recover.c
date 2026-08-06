
#include "shibadb.h"
#include "replace.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static const char *db_path = "test-replace-recover.tmp";
static const char *marker_path = "test-replace-recover.tmp.replace";
static const char *wal_path = "test-replace-recover.tmp.wal";

static void cleanup(void)
{
    (void)remove(marker_path);
    (void)remove("test-replace-recover.tmp.replace.tmp");
    (void)remove(wal_path);
    (void)remove(db_path);
}

static void write_bytes(const char *path, const uint8_t *data, size_t size)
{
    FILE *f = fopen(path, "wb");
    assert(f != NULL);
    if (size > 0U) {
        assert(fwrite(data, 1U, size, f) == size);
    }
    (void)fclose(f);
}

static int file_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

static void seed_database_file(void)
{

    write_bytes(db_path, (const uint8_t *)"\0", 1U);
}

static void test_truncated_marker_cleaned_up(void)
{

    uint8_t current_id[SDB_FILE_ID_SIZE];
    uint8_t short_marker[10];
    sdb_status status;

    cleanup();
    seed_database_file();
    (void)memset(current_id, 0x11, sizeof(current_id));
    (void)memset(short_marker, 0xAB, sizeof(short_marker));
    write_bytes(marker_path, short_marker, sizeof(short_marker));
    assert(file_exists(marker_path));

    status = sdb_replace_recover(db_path, current_id);
    assert(status == SDB_OK);
    assert(!file_exists(marker_path));
    cleanup();
    (void)puts("truncated marker cleaned up: ok");
}

static void test_empty_marker_cleaned_up(void)
{

    uint8_t current_id[SDB_FILE_ID_SIZE];
    sdb_status status;

    cleanup();
    seed_database_file();
    (void)memset(current_id, 0x22, sizeof(current_id));
    write_bytes(marker_path, NULL, 0U);
    assert(file_exists(marker_path));

    status = sdb_replace_recover(db_path, current_id);
    assert(status == SDB_OK);
    assert(!file_exists(marker_path));
    cleanup();
    (void)puts("empty marker cleaned up: ok");
}

static void test_no_marker_is_noop(void)
{

    uint8_t current_id[SDB_FILE_ID_SIZE];
    sdb_status status;

    cleanup();
    seed_database_file();
    (void)memset(current_id, 0x33, sizeof(current_id));
    assert(!file_exists(marker_path));

    status = sdb_replace_recover(db_path, current_id);
    assert(status == SDB_OK);
    cleanup();
    (void)puts("absent marker is no-op: ok");
}

static void test_corrupt_marker_cleaned_up(void)
{

    uint8_t current_id[SDB_FILE_ID_SIZE];
    uint8_t bogus[32];
    sdb_status status;

    cleanup();
    seed_database_file();
    (void)memset(current_id, 0x44, sizeof(current_id));
    (void)memset(bogus, 0xFF, sizeof(bogus));
    write_bytes(marker_path, bogus, sizeof(bogus));
    assert(file_exists(marker_path));

    status = sdb_replace_recover(db_path, current_id);
    assert(status == SDB_OK);
    assert(!file_exists(marker_path));
    cleanup();
    (void)puts("corrupt full-size marker cleaned up: ok");
}

static void test_matching_marker_drives_finish(void)
{

    uint8_t file_id[SDB_FILE_ID_SIZE];
    uint8_t wal_bytes[8];
    sdb_status status;

    cleanup();
    seed_database_file();
    (void)memset(file_id, 0x55, sizeof(file_id));
    (void)memset(wal_bytes, 0xCD, sizeof(wal_bytes));
    /* A real (checksum-valid) marker naming replacement file_id 0x55. */
    assert(sdb_replace_prepare(db_path, file_id) == SDB_OK);
    assert(file_exists(marker_path));
    write_bytes(wal_path, wal_bytes, sizeof(wal_bytes));
    assert(file_exists(wal_path));

    /*
     * current file_id matches the marker: the swap is already in place, so
     * recover must drive sdb_replace_finish -> stale WAL and marker removed.
     */
    status = sdb_replace_recover(db_path, file_id);
    assert(status == SDB_OK);
    assert(!file_exists(wal_path));
    assert(!file_exists(marker_path));
    cleanup();
    (void)puts("matching marker drives finish (stale wal removed): ok");
}

static void test_matching_marker_without_wal_is_idempotent(void)
{

    uint8_t file_id[SDB_FILE_ID_SIZE];
    sdb_status status;

    cleanup();
    seed_database_file();
    (void)memset(file_id, 0x66, sizeof(file_id));
    assert(sdb_replace_prepare(db_path, file_id) == SDB_OK);
    assert(file_exists(marker_path));
    /*
     * Models a crash in the finish window AFTER the WAL unlink but BEFORE the
     * marker unlink: the WAL is already gone, the marker still names the
     * (already swapped) data file. Re-running finish must be a no-op on the
     * missing WAL and still remove the marker.
     */
    assert(!file_exists(wal_path));

    status = sdb_replace_recover(db_path, file_id);
    assert(status == SDB_OK);
    assert(!file_exists(marker_path));
    assert(!file_exists(wal_path));
    cleanup();
    (void)puts("crash-between-unlinks re-run finishes cleanly: ok");
}

static void test_nonmatching_marker_rolled_back(void)
{

    uint8_t marker_id[SDB_FILE_ID_SIZE];
    uint8_t current_id[SDB_FILE_ID_SIZE];
    uint8_t wal_bytes[8];
    sdb_status status;

    cleanup();
    seed_database_file();
    (void)memset(marker_id, 0x77, sizeof(marker_id));
    (void)memset(current_id, 0x88, sizeof(current_id));
    (void)memset(wal_bytes, 0xEF, sizeof(wal_bytes));
    assert(sdb_replace_prepare(db_path, marker_id) == SDB_OK);
    assert(file_exists(marker_path));
    write_bytes(wal_path, wal_bytes, sizeof(wal_bytes));

    /*
     * current file_id does NOT match the marker: the swap never landed, so
     * recover must roll the replace back — drop the marker but leave the
     * source WAL (which belongs to the un-swapped data file) intact.
     */
    status = sdb_replace_recover(db_path, current_id);
    assert(status == SDB_OK);
    assert(!file_exists(marker_path));
    assert(file_exists(wal_path));
    cleanup();
    (void)puts("non-matching marker rolled back (source wal preserved): ok");
}

int main(void)
{
    test_truncated_marker_cleaned_up();
    test_empty_marker_cleaned_up();
    test_no_marker_is_noop();
    test_corrupt_marker_cleaned_up();
    test_matching_marker_drives_finish();
    test_matching_marker_without_wal_is_idempotent();
    test_nonmatching_marker_rolled_back();
    (void)puts("replace recover tests: ok");
    return 0;
}
