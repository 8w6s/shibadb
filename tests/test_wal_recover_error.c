/*
 * sdb_wal_recover / sdb_wal_clear error-propagation tests.
 *
 * Both entry points used to convert ANY sdb_file_open_existing failure
 * on the WAL path into SDB_OK, on the theory that a missing WAL is
 * the healthy "no recovery required" state. In practice the same
 * status also fired for EACCES, EMFILE, and transient EIO, which
 * meant a WAL with fsynced commit records but broken read access
 * silently looked cleared: every subsequent open reported a healthy
 * DB whose most recent transactions were gone.
 *
 * The fix distinguishes SDB_E_NOT_FOUND (still legitimately mapped
 * to SDB_OK) from every other error. These tests guard both
 * directions:
 *   1. absent WAL → SDB_OK, replayed=false (regression anchor).
 *   2. unreadable WAL (chmod 000) → SDB_E_IO / SDB_E_AUTHENTICATION-
 *      like open failure propagated, never SDB_OK.
 *
 * The chmod-000 arm is skipped when the effective uid is 0, because
 * root bypasses DAC and would see the file as readable.
 */

#include "shibadb.h"
#include "wal.h"
#include "file.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static const char *db_path = "test-wal-recover-error.tmp";
static const char *wal_path = "test-wal-recover-error.tmp.wal";

static void cleanup(void)
{
    /* Restore perms before remove so chmod-000 arm can be re-run. */
    (void)chmod(wal_path, 0600);
    (void)remove(wal_path);
    (void)remove(db_path);
}

static sdb_superblock_v1 make_superblock(void)
{
    sdb_superblock_v1 sb;
    size_t i;
    (void)memset(&sb, 0, sizeof(sb));
    sb.page_size = 4096U;
    sb.generation = 1U;
    sb.root_page = 2U;
    sb.next_page_id = 3U;
    for (i = 0U; i < SDB_SALT_SIZE; ++i) {
        sb.salt[i] = (uint8_t)(i + 1U);
    }
    for (i = 0U; i < SDB_FILE_ID_SIZE; ++i) {
        sb.file_id[i] = (uint8_t)(0x80U + i);
    }
    return sb;
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

static void test_absent_wal_is_ok(void)
{
    /* Regression anchor: SDB_E_NOT_FOUND from open must still surface
     * to callers as SDB_OK with replayed_out=false. */
    sdb_superblock_v1 sb = make_superblock();
    sdb_file db;
    uint64_t txn_id = UINT64_MAX;
    uint64_t next_page_id = 0U;
    uint64_t freelist_page = 0U;
    bool replayed = true; /* Set to true; expect fix-up to false. */

    cleanup();
    write_bytes(db_path, (const uint8_t *)"\0", 1U);
    assert(sdb_file_open_existing(db_path, false, &db) == SDB_OK);

    assert(sdb_wal_recover(
        wal_path, &db, &sb, NULL,
        &txn_id, &next_page_id, &freelist_page, &replayed
    ) == SDB_OK);
    assert(!replayed);
    assert(txn_id == 0U);
    assert(next_page_id == sb.next_page_id);
    assert(freelist_page == sb.freelist_page);
    assert(sdb_file_close(&db) == SDB_OK);
    cleanup();
    (void)puts("absent wal → SDB_OK, replayed=false: ok");
}

static void test_absent_wal_clear_is_ok(void)
{
    /* Regression anchor for sdb_wal_clear on missing WAL. */
    cleanup();
    assert(sdb_wal_clear(wal_path) == SDB_OK);
    (void)puts("absent wal clear → SDB_OK: ok");
}

static void test_unreadable_wal_propagates_error(void)
{
    /* Real fix under test: a WAL whose open fails for a reason other
     * than "does not exist" must NOT be silently converted to OK. */
    sdb_superblock_v1 sb = make_superblock();
    sdb_file db;
    uint64_t txn_id = 0U;
    uint64_t next_page_id = 0U;
    uint64_t freelist_page = 0U;
    bool replayed = false;
    sdb_status recover_status;
    uint8_t dummy[8];

    if (geteuid() == 0U) {
        (void)puts("skip: chmod-000 arm requires non-root euid");
        return;
    }

    cleanup();
    write_bytes(db_path, (const uint8_t *)"\0", 1U);
    (void)memset(dummy, 0xAA, sizeof(dummy));
    write_bytes(wal_path, dummy, sizeof(dummy));
    assert(chmod(wal_path, 0) == 0);

    assert(sdb_file_open_existing(db_path, false, &db) == SDB_OK);
    recover_status = sdb_wal_recover(
        wal_path, &db, &sb, NULL,
        &txn_id, &next_page_id, &freelist_page, &replayed
    );
    /* We must NOT collapse a permission error into SDB_OK. Prior to
     * the fix this assertion failed (status was SDB_OK, silently
     * dropping any pending replay). */
    assert(recover_status != SDB_OK);
    assert(recover_status == SDB_E_IO);
    assert(!replayed);
    assert(sdb_file_close(&db) == SDB_OK);
    cleanup();
    (void)puts("unreadable wal → propagated SDB_E_IO: ok");
}

static void test_unreadable_wal_clear_propagates_error(void)
{
    sdb_status clear_status;
    uint8_t dummy[8];

    if (geteuid() == 0U) {
        (void)puts("skip: chmod-000 arm requires non-root euid");
        return;
    }

    cleanup();
    (void)memset(dummy, 0xBB, sizeof(dummy));
    write_bytes(wal_path, dummy, sizeof(dummy));
    assert(chmod(wal_path, 0) == 0);

    clear_status = sdb_wal_clear(wal_path);
    assert(clear_status != SDB_OK);
    assert(clear_status == SDB_E_IO);
    cleanup();
    (void)puts("unreadable wal clear → propagated SDB_E_IO: ok");
}

int main(void)
{
    test_absent_wal_is_ok();
    test_absent_wal_clear_is_ok();
    test_unreadable_wal_propagates_error();
    test_unreadable_wal_clear_propagates_error();
    (void)puts("wal recover error tests: ok");
    return 0;
}
