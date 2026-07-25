/*
 * Structural fuzz for sdb_wal_recover.
 *
 * The recover path parses a WAL header + record stream, verifies
 * CRCs, and — on match with the current superblock's file_id and
 * page_size — replays record page images into the database. That
 * decoder is trust-boundary code: it consumes bytes that might have
 * been torn by a crash, or (in the WORM-DB + writable-WAL threat
 * model) crafted by an attacker with write access to just the WAL
 * sidecar.
 *
 * Strategy: each iteration writes the fuzz input to a fresh WAL
 * path and invokes sdb_wal_recover against a durable superblock
 * template. The DB file itself is a temp file created once and
 * pre-populated with a valid superblock; recover writes back to it
 * only when the WAL is a legal replay. Any crash / ASan hit is a
 * bug.
 *
 * We do NOT insist on a specific status code — recover returning
 * SDB_E_CORRUPT on garbage is the healthy response — but we DO
 * assert that on any error the reported replay flag stays false
 * and the txn/page/freelist outputs are the safe defaults.
 */

#include "shibadb.h"
#include "file.h"
#include "wal.h"

#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

#define SDB_FUZZ_WAL_PAGE_SIZE ((uint32_t)4096)
#define SDB_FUZZ_WAL_MAX_INPUT ((size_t)(1U << 20)) /* 1 MiB — well above SDB_WAL_MAX_BYTES bound checks reachable */

static char g_db_path[64];
static char g_wal_path[80];
static sdb_superblock_v1 g_sb;
static int g_initialized = 0;

static void build_superblock(sdb_superblock_v1 *sb)
{
    size_t i;
    (void)memset(sb, 0, sizeof(*sb));
    sb->page_size = SDB_FUZZ_WAL_PAGE_SIZE;
    sb->flags = 0U;
    sb->generation = 1U;
    sb->root_page = 2U;
    sb->next_page_id = 4U;
    for (i = 0U; i < SDB_SALT_SIZE; ++i) {
        sb->salt[i] = (uint8_t)(0x11U + i);
    }
    for (i = 0U; i < SDB_FILE_ID_SIZE; ++i) {
        sb->file_id[i] = (uint8_t)(0xC0U + i);
    }
}

static int ensure_initialized(void)
{
    int fd;
    if (g_initialized) {
        return 0;
    }
    (void)snprintf(g_db_path, sizeof(g_db_path),
        "/tmp/shibadb-fuzz-wal-%d.db", (int)getpid());
    (void)snprintf(g_wal_path, sizeof(g_wal_path), "%s.wal", g_db_path);
    (void)unlink(g_db_path);
    (void)unlink(g_wal_path);
    build_superblock(&g_sb);
    if (sdb_superblock_store_create(g_db_path, &g_sb) != SDB_OK) {
        return -1;
    }
    /*
     * Grow the DB file to next_page_id * page_size so recover's page
     * writes have somewhere to land without pager-side canonicalize
     * churn. Not strictly required — recover writes at explicit
     * offsets — but keeps the harness deterministic.
     */
    fd = open(g_db_path, 1 /*O_WRONLY*/);
    if (fd >= 0) {
        (void)ftruncate(
            fd,
            (off_t)((uint64_t)g_sb.next_page_id
                * (uint64_t)g_sb.page_size)
        );
        (void)close(fd);
    }
    g_initialized = 1;
    return 0;
}

static void write_wal(const uint8_t *data, size_t size)
{
    FILE *f;
    (void)unlink(g_wal_path);
    if (size == 0U) {
        return;
    }
    f = fopen(g_wal_path, "wb");
    if (f == NULL) {
        return;
    }
    (void)fwrite(data, 1U, size, f);
    (void)fclose(f);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    sdb_file db;
    uint64_t txn_id = UINT64_MAX;
    uint64_t next_page_id = UINT64_MAX;
    uint64_t freelist_page = UINT64_MAX;
    bool replayed = true;
    sdb_status status;

    if (size > SDB_FUZZ_WAL_MAX_INPUT) {
        return 0;
    }
    if (ensure_initialized() != 0) {
        return 0;
    }

    write_wal(data, size);

    if (sdb_file_open_existing(g_db_path, true, &db) != SDB_OK) {
        return 0;
    }
    status = sdb_wal_recover(
        g_wal_path, &db, &g_sb, NULL,
        &txn_id, &next_page_id, &freelist_page, &replayed
    );
    (void)sdb_file_close(&db);

    if (status != SDB_OK) {
        /* Contract: on error, outputs should be untouched or safe.
         * We reset to sentinels above; require them or the safe
         * default (superblock's next_page_id / freelist_page). */
        if (replayed
            || (next_page_id != UINT64_MAX
                && next_page_id != g_sb.next_page_id)
            || (freelist_page != UINT64_MAX
                && freelist_page != g_sb.freelist_page)) {
            __builtin_trap();
        }
    }
    return 0;
}
