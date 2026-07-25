/*
 * Regression coverage for sdb_replace_recover cleanup-branch guards.
 *
 * Prior to the guard extension, only SDB_OK and SDB_E_CORRUPT status
 * codes from the marker read/decode phase led to the unlink cleanup;
 * SDB_E_TRUNCATED (short marker) and SDB_E_IO propagated out. A caller
 * who saw a truncated .replace sidecar (interrupted rsync, filesystem
 * tail-loss, or an attacker who dropped bytes) would then see every
 * subsequent open fail with SDB_E_TRUNCATED — a permanent DoS until
 * an operator manually removed the marker.
 *
 * The intent of the guard, per the comment at replace.c near line 265,
 * is: "a marker that carries no actionable replacement intent is
 * cleaned up, not brick-until-operator". Truncated markers meet that
 * definition just as clearly as CRC-failed ones.
 */

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

static void cleanup(void)
{
    (void)remove(marker_path);
    (void)remove("test-replace-recover.tmp.replace.tmp");
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
    /*
     * sdb_replace_recover calls sdb_file_sync_parent_directory on the
     * DB path, which requires the parent to be openable — a plain
     * regular file at db_path is sufficient. The database itself is
     * never read by recover; only the sibling marker matters.
     */
    write_bytes(db_path, (const uint8_t *)"\0", 1U);
}

static void test_truncated_marker_cleaned_up(void)
{
    /* Short marker: 10 bytes < 32 = SDB_REPLACE_MARKER_SIZE.
     * Before fix: sdb_file_read_full returns SDB_E_TRUNCATED, the
     * guard at replace.c:264 rejects it, sdb_replace_recover returns
     * SDB_E_TRUNCATED, and the marker persists.
     * After fix: SDB_E_TRUNCATED goes down the abort branch, the
     * marker is unlinked, parent dir is fsynced, and SDB_OK is
     * returned.
     */
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
    /* Zero-length marker: same failure mode (SDB_E_TRUNCATED on first
     * read attempt). Explicit corner case in case a crash truncates
     * to zero. */
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
    /* Regression guard: when .replace is absent, recover succeeds
     * without touching the database. */
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
    /* 32-byte marker with bogus content → SDB_E_CORRUPT from decode.
     * This case was already handled correctly; keep it as a regression
     * anchor so a future refactor that narrows the guard also fails. */
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

int main(void)
{
    test_truncated_marker_cleaned_up();
    test_empty_marker_cleaned_up();
    test_no_marker_is_noop();
    test_corrupt_marker_cleaned_up();
    (void)puts("replace recover tests: ok");
    return 0;
}
