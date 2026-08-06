
#include "shibadb.h"
#include "wal.h"
#include "file.h"
#include "page.h"
#include "internal.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

static const char *db_path = "test-wal-recover-error.tmp";
static const char *wal_path = "test-wal-recover-error.tmp.wal";

static void cleanup(void)
{

#ifndef _WIN32
    (void)chmod(wal_path, 0600);
#endif
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

    sdb_superblock_v1 sb = make_superblock();
    sdb_file db;
    uint64_t txn_id = UINT64_MAX;
    uint64_t next_page_id = 0U;
    uint64_t freelist_page = 0U;
    bool replayed = true;

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

    cleanup();
    assert(sdb_wal_clear(wal_path) == SDB_OK);
    (void)puts("absent wal clear → SDB_OK: ok");
}

static void test_unreadable_wal_propagates_error(void)
{
#ifdef _WIN32
    (void)puts("skip: chmod-000 unreadable WAL arm is POSIX-only");
#else

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

    assert(recover_status != SDB_OK);
    assert(recover_status == SDB_E_IO);
    assert(!replayed);
    assert(sdb_file_close(&db) == SDB_OK);
    cleanup();
    (void)puts("unreadable wal → propagated SDB_E_IO: ok");
#endif
}

static void test_unreadable_wal_clear_propagates_error(void)
{
#ifdef _WIN32
    (void)puts("skip: chmod-000 unreadable WAL-clear arm is POSIX-only");
#else
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
#endif
}

/*
 * Regression (adversarial finding CRITICAL #2): the single-record legacy
 * reader (sdb_wal_recover) must replay a REAL v2 WAL written by the old
 * binary, whose commit record is 24 bytes — not the 44-byte v3 commit size.
 *
 * When SDB_WAL_COMMIT_SIZE grew from 24 to 44 for the v3 multi-txn WAL, the
 * legacy reader kept computing its expected file size with the 44-byte
 * constant, so a genuine 24-byte-commit WAL failed the size gate by 20
 * bytes and was silently discarded (returned SDB_OK, replayed=false),
 * losing a crash-committed pre-upgrade txn. This test crafts a byte-exact
 * 24-byte-commit v2 WAL image (the historical layout, hardcoded here so a
 * future size-constant change cannot mask the regression) and asserts the
 * reader replays it.
 */
#define V2_WAL_HEADER_SIZE ((size_t)64)
#define V2_WAL_RECORD_HEADER_SIZE ((size_t)8)
#define V2_WAL_COMMIT_SIZE ((size_t)24)
#define V2_WAL_VERSION ((uint16_t)2)

static void test_legacy_v2_24byte_commit_replays(void)
{
    sdb_superblock_v1 sb = make_superblock();
    const size_t page_size = (size_t)sb.page_size;
    const size_t frame_size = V2_WAL_RECORD_HEADER_SIZE + page_size;
    const size_t data_size = V2_WAL_HEADER_SIZE + frame_size;
    const size_t total_size = data_size + V2_WAL_COMMIT_SIZE;
    const uint64_t txn_id = sb.checkpoint_lsn + 1U; /* 1 */
    const uint8_t payload[] = "legacy-v2-payload";
    uint8_t *buffer;
    uint8_t *header;
    uint8_t *frame;
    uint8_t *commit;
    uint32_t header_checksum;
    uint32_t commit_checksum;
    sdb_file db;
    uint64_t out_txn_id = 0U;
    uint64_t out_next_page_id = 0U;
    uint64_t out_freelist_page = 0U;
    bool replayed = false;

    cleanup();
    write_bytes(db_path, (const uint8_t *)"\0", 1U);

    buffer = (uint8_t *)calloc(total_size, 1U);
    assert(buffer != NULL);
    header = buffer;
    frame = buffer + V2_WAL_HEADER_SIZE;
    commit = buffer + data_size;

    /* --- v2 header --- */
    (void)memcpy(header, "SWAL", 4U);
    sdb_write_u16_le(header + 4U, V2_WAL_VERSION);
    sdb_write_u16_le(header + 6U, (uint16_t)V2_WAL_HEADER_SIZE);
    sdb_write_u64_le(header + 8U, txn_id);
    sdb_write_u32_le(header + 16U, sb.page_size);
    sdb_write_u32_le(header + 20U, 1U); /* page_count */
    (void)memcpy(header + 24U, sb.file_id, SDB_FILE_ID_SIZE);
    sdb_write_u64_le(header + 40U, sb.next_page_id);
    sdb_write_u64_le(header + 48U, sb.freelist_page);
    /* [56,60) reserved = 0; [60,64) header checksum. */
    header_checksum = sdb_crc32_zeroed_range(
        header, V2_WAL_HEADER_SIZE, (size_t)60, sizeof(uint32_t)
    );
    sdb_write_u32_le(header + 60U, header_checksum);

    /* --- single frame: page_id=1 with a page whose page_lsn == txn_id --- */
    sdb_write_u64_le(frame, 1U);
    assert(sdb_page_encode(
        frame + V2_WAL_RECORD_HEADER_SIZE, page_size,
        (uint16_t)SDB_PAGE_TYPE_DATA, 1U, txn_id,
        payload, sizeof(payload)
    ) == SDB_OK);

    /* --- v2 commit record: 24 bytes, checksum at offset 20 --- */
    (void)memcpy(commit, "SCMT", 4U);
    sdb_write_u16_le(commit + 4U, V2_WAL_VERSION);
    sdb_write_u16_le(commit + 6U, (uint16_t)V2_WAL_COMMIT_SIZE);
    sdb_write_u64_le(commit + 8U, txn_id);
    sdb_write_u32_le(commit + 16U, sdb_crc32(buffer, data_size));
    commit_checksum = sdb_crc32_zeroed_range(
        commit, V2_WAL_COMMIT_SIZE, (size_t)20, sizeof(uint32_t)
    );
    sdb_write_u32_le(commit + 20U, commit_checksum);

    write_bytes(wal_path, buffer, total_size);
    free(buffer);

    assert(sdb_file_open_existing(db_path, true, &db) == SDB_OK);
    assert(sdb_wal_recover(
        wal_path, &db, &sb, NULL,
        &out_txn_id, &out_next_page_id, &out_freelist_page, &replayed
    ) == SDB_OK);
    /* The genuine 24-byte-commit v2 WAL MUST be replayed, not discarded. */
    assert(replayed);
    assert(out_txn_id == txn_id);
    assert(out_next_page_id == sb.next_page_id);
    assert(out_freelist_page == sb.freelist_page);
    assert(sdb_file_close(&db) == SDB_OK);
    cleanup();
    (void)puts("legacy v2 24-byte-commit WAL replays: ok");
}

/*
 * Regression (adversarial finding: durability, lost committed txns). A v4
 * WAL carries a 64-byte self-identifying header sealed by its own CRC. If
 * that header sector is torn/damaged (CRC fails) but every v4 frame and its
 * commit-record are intact, recover_all MUST still replay all committed txns.
 *
 * The pre-fix bug: use_v4 was derived ONLY from (crc_ok && version == V4). A
 * failed header CRC therefore forced the legacy v3 stride (8+page_size) over
 * what is really a v4 layout (24+page_size) — the scan never found the commit
 * magic, treated the WAL as empty (replayed=false, checkpoint_lsn unmoved),
 * and the caller would clear the WAL, silently destroying every committed
 * txn. The fix probes the self-describing v4 first frame when the header CRC
 * fails and, if it is an intact v4 txn, selects the v4 path anyway.
 */
static void build_v4_txn(
    sdb_file *wal, uint64_t *offset, const sdb_superblock_v1 *sb,
    uint64_t txn_id, uint64_t page_id, const uint8_t *payload, size_t plen
)
{
    const size_t page_size = (size_t)sb->page_size;
    uint8_t *pg = (uint8_t *)malloc(page_size);
    sdb_wal_page wp;
    uint64_t new_offset = 0U;

    assert(pg != NULL);
    assert(sdb_page_encode(
        pg, page_size, (uint16_t)SDB_PAGE_TYPE_DATA, page_id, txn_id,
        payload, plen
    ) == SDB_OK);
    wp.page_id = page_id;
    wp.bytes = pg;
    assert(sdb_wal_append_txn(
        wal, *offset, sb, txn_id, &wp, 1U, &new_offset
    ) == SDB_OK);
    *offset = new_offset;
    free(pg);
}

static void test_torn_v4_header_still_replays(void)
{
    sdb_superblock_v1 sb = make_superblock();
    const size_t page_size = (size_t)sb.page_size;
    const uint64_t data_offset =
        (uint64_t)(SDB_SUPERBLOCK_SLOT_SIZE * SDB_SUPERBLOCK_SLOT_COUNT);
    const uint8_t pa[] = "txn1-page1-A";
    const uint8_t pb[] = "txn2-page2-B";
    const uint8_t pc[] = "txn3-page1-C";
    sdb_file wal;
    sdb_file db;
    uint64_t offset = (uint64_t)SDB_WAL_HEADER_SIZE;
    uint64_t out_txn_id = 0U;
    uint64_t out_next_page_id = 0U;
    uint64_t out_freelist_page = 0U;
    bool replayed = false;
    FILE *hf;
    int poke_byte;
    uint8_t *page_buf;
    sdb_page_view view;

    sb.next_page_id = 10U;
    sb.freelist_page = 0U;
    sb.checkpoint_lsn = 0U;

    cleanup();
    write_bytes(db_path, (const uint8_t *)"\0", 1U);

    /*
     * Build a genuine 3-txn v4 WAL through the real append path so the
     * 64-byte identity header is written and CRC-sealed.
     */
    assert(sdb_file_create_new(wal_path, &wal) == SDB_OK);
    build_v4_txn(&wal, &offset, &sb, 1U, 1U, pa, sizeof(pa));
    build_v4_txn(&wal, &offset, &sb, 2U, 2U, pb, sizeof(pb));
    build_v4_txn(&wal, &offset, &sb, 3U, 1U, pc, sizeof(pc));
    assert(sdb_file_close(&wal) == SDB_OK);

    /*
     * Damage ONE byte inside the identity header [0,64) and do NOT re-seal
     * the header CRC: crc_ok becomes false while every v4 frame + commit
     * record stays intact.
     */
    hf = fopen(wal_path, "r+b");
    assert(hf != NULL);
    assert(fseek(hf, 10L, SEEK_SET) == 0);
    poke_byte = fgetc(hf);
    assert(poke_byte != EOF);
    assert(fseek(hf, 10L, SEEK_SET) == 0);
    assert(fputc((poke_byte ^ 0xFF) & 0xFF, hf) != EOF);
    assert(fclose(hf) == 0);

    assert(sdb_file_open_existing(db_path, true, &db) == SDB_OK);
    assert(sdb_wal_recover_all(
        wal_path, &db, &sb, NULL,
        &out_txn_id, &out_next_page_id, &out_freelist_page, &replayed
    ) == SDB_OK);

    /* Every committed txn MUST be replayed despite the torn header. */
    assert(replayed);
    assert(out_txn_id == 3U);
    assert(out_next_page_id == 10U);
    assert(out_freelist_page == 0U);

    /* Verify applied page contents: page 2 = B, page 1 = C (txn3 wins). */
    page_buf = (uint8_t *)malloc(page_size);
    assert(page_buf != NULL);

    assert(sdb_file_read_full(
        &db, data_offset + (2U - 1U) * (uint64_t)page_size,
        page_buf, page_size
    ) == SDB_OK);
    assert(sdb_page_decode(page_buf, page_size, 2U, &view) == SDB_OK);
    assert(view.payload_size == (uint32_t)sizeof(pb));
    assert(memcmp(view.payload, pb, sizeof(pb)) == 0);

    assert(sdb_file_read_full(
        &db, data_offset + (1U - 1U) * (uint64_t)page_size,
        page_buf, page_size
    ) == SDB_OK);
    assert(sdb_page_decode(page_buf, page_size, 1U, &view) == SDB_OK);
    assert(view.payload_size == (uint32_t)sizeof(pc));
    assert(memcmp(view.payload, pc, sizeof(pc)) == 0);

    free(page_buf);
    assert(sdb_file_close(&db) == SDB_OK);
    cleanup();
    (void)puts(
        "torn v4 identity header still replays all committed txns: ok"
    );
}

int main(void)
{
    test_absent_wal_is_ok();
    test_absent_wal_clear_is_ok();
    test_unreadable_wal_propagates_error();
    test_unreadable_wal_clear_propagates_error();
    test_legacy_v2_24byte_commit_replays();
    test_torn_v4_header_still_replays();
    (void)puts("wal recover error tests: ok");
    return 0;
}
