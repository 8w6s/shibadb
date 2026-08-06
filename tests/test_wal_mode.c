/* tests/test_wal_mode.c — multi-txn WAL append + multi-txn recovery. */
#include "file.h"
#include "internal.h"
#include "page.h"
#include "pager.h"
#include "wal.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *wal_path = "test-wal-mode.tmp.wal";
static const char *db_path = "test-wal-mode.tmp.db";
static const char *pager_path = "test-wal-mode-pager.tmp";

static void encode_page(
    uint8_t *buf, size_t page_size, uint64_t page_id, uint64_t page_lsn
)
{
    static const uint8_t payload[] = "hello";
    const sdb_status status = sdb_page_encode(
        buf,
        page_size,
        (uint16_t)SDB_PAGE_TYPE_DATA,
        page_id,
        page_lsn,
        payload,
        sizeof(payload)
    );
    assert(status == SDB_OK);
}

static void test_append_two_txns(void)
{
    sdb_superblock_v1 sb;
    sdb_file wal;
    uint8_t page_a0[4096];
    uint8_t page_a1[4096];
    uint8_t page_b0[4096];
    sdb_wal_page pages_a[2];
    sdb_wal_page pages_b[1];
    uint8_t commit_buf[SDB_WAL_COMMIT_SIZE];
    sdb_wal_commit_rec rec;
    uint32_t running_crc = 0U;
    const size_t frame_size = sdb_wal_frame_size_v5((size_t)4096U);
    uint64_t off = (uint64_t)SDB_WAL_HEADER_SIZE;
    uint64_t next = off;
    uint64_t first_commit_offset;

    (void)memset(&sb, 0, sizeof(sb));
    sb.page_size = 4096U;
    sb.next_page_id = 100U;
    sb.freelist_page = 0U;

    encode_page(page_a0, (size_t)4096U, 3U, 1U);
    encode_page(page_a1, (size_t)4096U, 5U, 1U);
    encode_page(page_b0, (size_t)4096U, 7U, 2U);
    pages_a[0].page_id = 3U;
    pages_a[0].bytes = page_a0;
    pages_a[1].page_id = 5U;
    pages_a[1].bytes = page_a1;
    pages_b[0].page_id = 7U;
    pages_b[0].bytes = page_b0;

    (void)remove(wal_path);
    assert(sdb_file_create_new(wal_path, &wal) == SDB_OK);

    assert(
        sdb_wal_append_txn(&wal, off, &sb, 1U, pages_a, 2U, &next) == SDB_OK
    );
    assert(
        next == off + (uint64_t)(2U * frame_size)
                    + (uint64_t)SDB_WAL_COMMIT_SIZE
    );
    first_commit_offset = off + (uint64_t)(2U * frame_size);

    off = next;
    assert(
        sdb_wal_append_txn(&wal, off, &sb, 2U, pages_b, 1U, &next) == SDB_OK
    );
    assert(
        next == off + (uint64_t)frame_size + (uint64_t)SDB_WAL_COMMIT_SIZE
    );
    assert(next > off);

    assert(sdb_file_close(&wal) == SDB_OK);

    assert(sdb_file_open_existing(wal_path, false, &wal) == SDB_OK);
    assert(
        sdb_file_read_full(
            &wal, first_commit_offset, commit_buf, SDB_WAL_COMMIT_SIZE
        ) == SDB_OK
    );
    assert(
        sdb_wal_decode_commit_rec(
            commit_buf, SDB_WAL_COMMIT_SIZE, &rec, &running_crc
        ) == SDB_OK
    );
    assert(rec.txn_id == 1U);
    assert(rec.frame_count == 2U);
    assert(rec.next_page_id == 100U);
    assert(rec.freelist_page == 0U);

    assert(
        sdb_file_read_full(
            &wal,
            next - (uint64_t)SDB_WAL_COMMIT_SIZE,
            commit_buf,
            SDB_WAL_COMMIT_SIZE
        ) == SDB_OK
    );
    assert(
        sdb_wal_decode_commit_rec(
            commit_buf, SDB_WAL_COMMIT_SIZE, &rec, &running_crc
        ) == SDB_OK
    );
    assert(rec.txn_id == 2U);
    assert(rec.frame_count == 1U);
    assert(rec.next_page_id == 100U);
    assert(rec.freelist_page == 0U);

    assert(sdb_file_close(&wal) == SDB_OK);
    (void)remove(wal_path);
    (void)puts("wal-mode append two txns: ok");
}

/*
 * Fixed superblock used by every recovery scenario below. Kept in one
 * place so the torn-tail coverage tests share exactly one WAL/DB shape.
 * next_page_id=20 leaves plenty of room for pages 3, 5, 7, 9, 11, 13.
 */
static void init_scenario_sb(sdb_superblock_v1 *sb)
{
    (void)memset(sb, 0, sizeof(*sb));
    sb->page_size = 4096U;
    sb->next_page_id = 20U;
    sb->freelist_page = 0U;
    sb->checkpoint_lsn = 0U;
}

/*
 * Build a fresh WAL containing three fully-committed txns:
 *   txn 1 writes pages 3, 5, 7 (page_lsn=1)
 *   txn 2 writes pages 3, 9    (page_lsn=2, overwrites page 3)
 *   txn 3 writes page 11       (page_lsn=3)
 * The WAL is left open in *wal_out (writable) with *off_after_out set to
 * the byte offset where txn 4 would start.
 */
static void build_three_committed_txns(
    const sdb_superblock_v1 *sb,
    sdb_file *wal_out,
    uint64_t *off_after_out
)
{
    uint8_t page_t1_a[4096];
    uint8_t page_t1_b[4096];
    uint8_t page_t1_c[4096];
    uint8_t page_t2_a[4096];
    uint8_t page_t2_b[4096];
    uint8_t page_t3_a[4096];
    sdb_wal_page pages[3];
    uint64_t off = (uint64_t)SDB_WAL_HEADER_SIZE;
    uint64_t next = off;

    encode_page(page_t1_a, (size_t)4096U, 3U, 1U);
    encode_page(page_t1_b, (size_t)4096U, 5U, 1U);
    encode_page(page_t1_c, (size_t)4096U, 7U, 1U);
    encode_page(page_t2_a, (size_t)4096U, 3U, 2U);
    encode_page(page_t2_b, (size_t)4096U, 9U, 2U);
    encode_page(page_t3_a, (size_t)4096U, 11U, 3U);

    (void)remove(wal_path);
    assert(sdb_file_create_new(wal_path, wal_out) == SDB_OK);

    pages[0].page_id = 3U;
    pages[0].bytes = page_t1_a;
    pages[1].page_id = 5U;
    pages[1].bytes = page_t1_b;
    pages[2].page_id = 7U;
    pages[2].bytes = page_t1_c;
    assert(
        sdb_wal_append_txn(wal_out, off, sb, 1U, pages, 3U, &next) == SDB_OK
    );
    off = next;

    pages[0].page_id = 3U;
    pages[0].bytes = page_t2_a;
    pages[1].page_id = 9U;
    pages[1].bytes = page_t2_b;
    assert(
        sdb_wal_append_txn(wal_out, off, sb, 2U, pages, 2U, &next) == SDB_OK
    );
    off = next;

    pages[0].page_id = 11U;
    pages[0].bytes = page_t3_a;
    assert(
        sdb_wal_append_txn(wal_out, off, sb, 3U, pages, 1U, &next) == SDB_OK
    );
    *off_after_out = next;
}

static void open_empty_data_file(sdb_file *db_out)
{
    const uint64_t data_offset =
        (uint64_t)(SDB_SUPERBLOCK_SLOT_SIZE * SDB_SUPERBLOCK_SLOT_COUNT);
    (void)remove(db_path);
    assert(sdb_file_create_new(db_path, db_out) == SDB_OK);
    assert(
        sdb_file_resize(db_out, data_offset + (uint64_t)19U * 4096U) == SDB_OK
    );
    assert(sdb_file_sync(db_out) == SDB_OK);
}

static void assert_page_lsn(
    sdb_file *db, uint64_t page_id, uint64_t expected_lsn
)
{
    const uint64_t data_offset =
        (uint64_t)(SDB_SUPERBLOCK_SLOT_SIZE * SDB_SUPERBLOCK_SLOT_COUNT);
    uint8_t page_out[4096];
    sdb_page_view view;
    assert(
        sdb_file_read_full(
            db,
            data_offset + (page_id - 1U) * 4096U,
            page_out,
            4096U
        ) == SDB_OK
    );
    assert(sdb_page_decode(page_out, (size_t)4096U, page_id, &view) == SDB_OK);
    assert(view.page_lsn == expected_lsn);
}

/* Assert the slot for `page_id` is untouched (all zeros → bad magic). */
static void assert_page_untouched(sdb_file *db, uint64_t page_id)
{
    const uint64_t data_offset =
        (uint64_t)(SDB_SUPERBLOCK_SLOT_SIZE * SDB_SUPERBLOCK_SLOT_COUNT);
    uint8_t page_out[4096];
    sdb_page_view view;
    assert(
        sdb_file_read_full(
            db,
            data_offset + (page_id - 1U) * 4096U,
            page_out,
            4096U
        ) == SDB_OK
    );
    assert(sdb_page_decode(page_out, (size_t)4096U, page_id, &view) != SDB_OK);
}

/*
 * (a) Truncated commit-record: append a valid txn 4 then chop off its
 * trailing SDB_WAL_COMMIT_SIZE bytes. Recovery must apply txns 1..3 and
 * discard txn 4.
 */
static void test_recover_truncated_commit_rec(void)
{
    sdb_superblock_v1 sb;
    sdb_file wal;
    sdb_file db;
    uint8_t page_t4_a[4096];
    sdb_wal_page pages[1];
    uint64_t off_after_3 = 0U;
    uint64_t next = 0U;
    uint64_t last_lsn = 999U;
    uint64_t recovered_next_page = 0U;
    uint64_t recovered_freelist = 999U;
    bool replayed = false;

    init_scenario_sb(&sb);
    build_three_committed_txns(&sb, &wal, &off_after_3);

    encode_page(page_t4_a, (size_t)4096U, 13U, 4U);
    pages[0].page_id = 13U;
    pages[0].bytes = page_t4_a;
    assert(
        sdb_wal_append_txn(&wal, off_after_3, &sb, 4U, pages, 1U, &next)
            == SDB_OK
    );
    /* Torn tail: chop the commit-record off txn 4. Frames remain on disk. */
    assert(
        sdb_file_resize(&wal, next - (uint64_t)SDB_WAL_COMMIT_SIZE)
            == SDB_OK
    );
    assert(sdb_file_sync(&wal) == SDB_OK);
    assert(sdb_file_close(&wal) == SDB_OK);

    open_empty_data_file(&db);
    assert(
        sdb_wal_recover_all(
            wal_path, &db, &sb, NULL,
            &last_lsn, &recovered_next_page, &recovered_freelist, &replayed
        ) == SDB_OK
    );
    assert(replayed == true);
    assert(last_lsn == 3U);
    assert(recovered_next_page == sb.next_page_id);
    assert(recovered_freelist == sb.freelist_page);

    assert_page_lsn(&db, 3U, 2U);
    assert_page_lsn(&db, 5U, 1U);
    assert_page_lsn(&db, 7U, 1U);
    assert_page_lsn(&db, 9U, 2U);
    assert_page_lsn(&db, 11U, 3U);
    assert_page_untouched(&db, 13U);

    assert(sdb_file_close(&db) == SDB_OK);
    (void)remove(wal_path);
    (void)remove(db_path);
    (void)puts(
        "wal-mode recover: truncated commit-rec discarded (txns 1..3 ok)"
    );
}

/*
 * (b) Commit-record inner-CRC corruption: append a full valid txn 4,
 * then flip one byte in txn 4's commit-record's own CRC field. Decode
 * of the commit-record will fail → torn tail → txns 1..3 applied, txn
 * 4 discarded.
 */
static void test_recover_corrupt_commit_rec_crc(void)
{
    sdb_superblock_v1 sb;
    sdb_file wal;
    sdb_file db;
    uint8_t page_t4_a[4096];
    sdb_wal_page pages[1];
    uint64_t off_after_3 = 0U;
    uint64_t next = 0U;
    uint64_t last_lsn = 999U;
    uint64_t recovered_next_page = 0U;
    uint64_t recovered_freelist = 999U;
    bool replayed = false;
    uint64_t commit_crc_offset;
    uint8_t crc_byte;

    init_scenario_sb(&sb);
    build_three_committed_txns(&sb, &wal, &off_after_3);

    encode_page(page_t4_a, (size_t)4096U, 13U, 4U);
    pages[0].page_id = 13U;
    pages[0].bytes = page_t4_a;
    assert(
        sdb_wal_append_txn(&wal, off_after_3, &sb, 4U, pages, 1U, &next)
            == SDB_OK
    );
    /*
     * Flip the low byte of the commit-record's own CRC. Layout of a v3
     * commit-record: the inner CRC lives at offset 40 within the 44-byte
     * record. The record sits at `next - SDB_WAL_COMMIT_SIZE`.
     */
    commit_crc_offset =
        next - (uint64_t)SDB_WAL_COMMIT_SIZE + (uint64_t)40U;
    assert(
        sdb_file_read_full(&wal, commit_crc_offset, &crc_byte, 1U) == SDB_OK
    );
    crc_byte ^= (uint8_t)0xFFU;
    assert(
        sdb_file_write_full(&wal, commit_crc_offset, &crc_byte, 1U) == SDB_OK
    );
    assert(sdb_file_sync(&wal) == SDB_OK);
    assert(sdb_file_close(&wal) == SDB_OK);

    open_empty_data_file(&db);
    assert(
        sdb_wal_recover_all(
            wal_path, &db, &sb, NULL,
            &last_lsn, &recovered_next_page, &recovered_freelist, &replayed
        ) == SDB_OK
    );
    assert(replayed == true);
    assert(last_lsn == 3U);
    assert(recovered_next_page == sb.next_page_id);
    assert(recovered_freelist == sb.freelist_page);

    assert_page_lsn(&db, 3U, 2U);
    assert_page_lsn(&db, 5U, 1U);
    assert_page_lsn(&db, 7U, 1U);
    assert_page_lsn(&db, 9U, 2U);
    assert_page_lsn(&db, 11U, 3U);
    assert_page_untouched(&db, 13U);

    assert(sdb_file_close(&db) == SDB_OK);
    (void)remove(wal_path);
    (void)remove(db_path);
    (void)puts(
        "wal-mode recover: corrupt commit-rec CRC discarded (txns 1..3 ok)"
    );
}

/*
 * (c) Out-of-order txn_id: append a valid txn with txn_id=6 (expected 4)
 * as the tail. The commit-record decodes and its running-CRC matches,
 * but the txn is not contiguous with the previous last_lsn — recovery
 * must treat this as torn and drop it.
 */
static void test_recover_out_of_order_txn_id(void)
{
    sdb_superblock_v1 sb;
    sdb_file wal;
    sdb_file db;
    uint8_t page_t4_a[4096];
    sdb_wal_page pages[1];
    uint64_t off_after_3 = 0U;
    uint64_t next = 0U;
    uint64_t last_lsn = 999U;
    uint64_t recovered_next_page = 0U;
    uint64_t recovered_freelist = 999U;
    bool replayed = false;

    init_scenario_sb(&sb);
    build_three_committed_txns(&sb, &wal, &off_after_3);

    /*
     * Encode page for txn_id=6 (page_lsn=6 to satisfy append_txn's
     * per-frame lsn check).
     */
    encode_page(page_t4_a, (size_t)4096U, 13U, 6U);
    pages[0].page_id = 13U;
    pages[0].bytes = page_t4_a;
    assert(
        sdb_wal_append_txn(&wal, off_after_3, &sb, 6U, pages, 1U, &next)
            == SDB_OK
    );
    assert(sdb_file_sync(&wal) == SDB_OK);
    assert(sdb_file_close(&wal) == SDB_OK);

    open_empty_data_file(&db);
    assert(
        sdb_wal_recover_all(
            wal_path, &db, &sb, NULL,
            &last_lsn, &recovered_next_page, &recovered_freelist, &replayed
        ) == SDB_OK
    );
    assert(replayed == true);
    assert(last_lsn == 3U);
    assert(recovered_next_page == sb.next_page_id);
    assert(recovered_freelist == sb.freelist_page);

    assert_page_lsn(&db, 3U, 2U);
    assert_page_lsn(&db, 5U, 1U);
    assert_page_lsn(&db, 7U, 1U);
    assert_page_lsn(&db, 9U, 2U);
    assert_page_lsn(&db, 11U, 3U);
    /* Page 13 belongs to the out-of-order txn 6 — must NOT land on disk. */
    assert_page_untouched(&db, 13U);

    assert(sdb_file_close(&db) == SDB_OK);
    (void)remove(wal_path);
    (void)remove(db_path);
    (void)puts(
        "wal-mode recover: out-of-order txn_id discarded (txns 1..3 ok)"
    );
}

/*
 * (d) Corruption inside a txn body mid-stream: flip a byte inside txn
 * 3's frame data. txn 3's commit-record still decodes cleanly, but its
 * running-CRC will no longer match the frames on disk. Recovery must
 * apply txns 1..2 and stop — txns 3 AND 4 discarded. Page 11 (touched
 * only by txn 3) and page 13 (touched only by txn 4) must both stay
 * untouched.
 */
static void test_recover_frame_body_corruption(void)
{
    sdb_superblock_v1 sb;
    sdb_file wal;
    sdb_file db;
    uint8_t page_t4_a[4096];
    sdb_wal_page pages[1];
    uint64_t off_after_3 = 0U;
    uint64_t next = 0U;
    uint64_t last_lsn = 999U;
    uint64_t recovered_next_page = 0U;
    uint64_t recovered_freelist = 999U;
    bool replayed = false;
    const size_t frame_size = sdb_wal_frame_size_v5((size_t)4096U);
    uint64_t txn3_start;
    uint64_t corrupt_offset;
    uint8_t body_byte;

    init_scenario_sb(&sb);
    build_three_committed_txns(&sb, &wal, &off_after_3);

    /*
     * Append a well-formed txn 4 too, to prove that recovery stops at the
     * corrupted txn 3 and never touches txn 4 either.
     */
    encode_page(page_t4_a, (size_t)4096U, 13U, 4U);
    pages[0].page_id = 13U;
    pages[0].bytes = page_t4_a;
    assert(
        sdb_wal_append_txn(&wal, off_after_3, &sb, 4U, pages, 1U, &next)
            == SDB_OK
    );

    /*
     * Txn 3 has exactly one frame. It sits immediately before txn 3's
     * commit-record, i.e. at off_after_3 - (frame_size + COMMIT_SIZE).
     * A v4 frame is [24-byte self-describing header] [page bytes
     * (page_size)]; poke a byte well inside the page body to guarantee a
     * CRC mismatch on the per-txn running_crc without breaking the txn 3
     * commit-record itself.
     */
    txn3_start = off_after_3
        - (uint64_t)frame_size
        - (uint64_t)SDB_WAL_COMMIT_SIZE;
    corrupt_offset = txn3_start
        + (uint64_t)SDB_WAL_RECORD_HEADER_SIZE_V5 + 100U;
    assert(
        sdb_file_read_full(&wal, corrupt_offset, &body_byte, 1U) == SDB_OK
    );
    body_byte ^= (uint8_t)0x5AU;
    assert(
        sdb_file_write_full(&wal, corrupt_offset, &body_byte, 1U) == SDB_OK
    );
    assert(sdb_file_sync(&wal) == SDB_OK);
    assert(sdb_file_close(&wal) == SDB_OK);

    open_empty_data_file(&db);
    assert(
        sdb_wal_recover_all(
            wal_path, &db, &sb, NULL,
            &last_lsn, &recovered_next_page, &recovered_freelist, &replayed
        ) == SDB_OK
    );
    assert(replayed == true);
    assert(last_lsn == 2U);
    assert(recovered_next_page == sb.next_page_id);
    assert(recovered_freelist == sb.freelist_page);

    /* Pages from txns 1..2 landed. Page 3 was rewritten by txn 2. */
    assert_page_lsn(&db, 3U, 2U);
    assert_page_lsn(&db, 5U, 1U);
    assert_page_lsn(&db, 7U, 1U);
    assert_page_lsn(&db, 9U, 2U);
    /* Page 11 (txn 3) and page 13 (txn 4) must be untouched. */
    assert_page_untouched(&db, 11U);
    assert_page_untouched(&db, 13U);

    assert(sdb_file_close(&db) == SDB_OK);
    (void)remove(wal_path);
    (void)remove(db_path);
    (void)puts(
        "wal-mode recover: mid-stream frame corruption discarded "
        "txn 3+ (txns 1..2 ok)"
    );
}

/*
 * Pager-level round-trip through the WAL-mode commit path:
 *   - Open a fresh pager (data-file only; WAL is untouched at create).
 *   - Allocate three pages via the direct pager path (writes to data file).
 *   - Run three txns that each rewrite one of those pages. In WAL-mode the
 *     commit path appends to the WAL (one fsync under R2a) and updates the
 *     in-memory superblock + page cache — the data file is NOT touched.
 *   - After each commit assert (a) pager.current_lsn advanced monotonically,
 *     (b) pager.wal_tail advanced past the frame + commit-rec bytes we
 *     just wrote, and (c) the frame offsets we handed the WAL index round
 *     trip through sdb_wal_index_get.
 *   - Close and reopen the pager. Recovery replays every committed txn
 *     onto the data file, so subsequent reads (which don't hit the WAL
 *     yet — read-through is Task 6) see the latest values.
 *   - Keep total WAL bytes well below the ~1000-page checkpoint threshold
 *     so no checkpoint trigger fires. Task 7 leaves the trigger unwired.
 */
static void fill_identity_bytes(uint8_t out[16], uint8_t base)
{
    size_t index;
    for (index = 0U; index < 16U; ++index) {
        out[index] = (uint8_t)(base + index);
    }
}

static void assert_pager_payload(
    sdb_pager *pager,
    uint64_t page_id,
    const uint8_t *expected,
    size_t expected_size
)
{
    uint8_t page[4096];
    sdb_page_view view;
    assert(sdb_pager_read(pager, page_id, page, sizeof(page), &view) == SDB_OK);
    assert(view.payload_size == expected_size);
    assert(memcmp(view.payload, expected, expected_size) == 0);
}

static void test_pager_wal_mode_multi_txn_round_trip(void)
{
    sdb_pager pager;
    uint8_t salt[16];
    uint8_t file_id[16];
    uint64_t p1;
    uint64_t p2;
    uint64_t p3;
    const uint8_t v1[] = "hello-txn-one-payload";
    const uint8_t v2[] = "second-txn-payload!!";
    const uint8_t v3[] = "third-and-final-txn";
    const size_t frame_size = sdb_wal_frame_size_v5((size_t)4096U);
    uint64_t wal_tail_at_start;
    uint64_t frame_offset_p1;
    uint64_t frame_offset_p2;
    uint64_t stored_offset = 0U;

    (void)remove(pager_path);
    (void)remove("test-wal-mode-pager.tmp.wal");
    fill_identity_bytes(salt, (uint8_t)0x11);
    fill_identity_bytes(file_id, (uint8_t)0x22);

    assert(sdb_pager_create(pager_path, 4096U, salt, file_id, &pager) == SDB_OK);
    /* Pre-allocate three pages so txn_put targets are within next_page_id. */
    assert(sdb_pager_allocate(&pager, &p1) == SDB_OK);
    assert(sdb_pager_allocate(&pager, &p2) == SDB_OK);
    assert(sdb_pager_allocate(&pager, &p3) == SDB_OK);

    /* Task 7 foundation: pager must expose the four new fields. */
    assert(pager.wal_tail == (uint64_t)SDB_WAL_HEADER_SIZE);
    assert(pager.current_lsn == pager.superblock.checkpoint_lsn);
    assert(pager.checkpoint_threshold > 0U);
    /* Threshold must be BIG enough that this test never trips it. */
    assert(pager.checkpoint_threshold >= (uint64_t)(1000U) * 4096U);

    wal_tail_at_start = pager.wal_tail;

    /* --- txn 1: p1 and p2 --- */
    {
        sdb_txn txn;
        const uint64_t lsn_before = pager.current_lsn;
        const uint64_t tail_before = pager.wal_tail;

        assert(sdb_txn_begin(&pager, &txn) == SDB_OK);
        assert(sdb_txn_put(
            &txn, p1, (uint16_t)SDB_PAGE_TYPE_DATA, v1, sizeof(v1)
        ) == SDB_OK);
        assert(sdb_txn_put(
            &txn, p2, (uint16_t)SDB_PAGE_TYPE_DATA, v2, sizeof(v2)
        ) == SDB_OK);
        assert(sdb_txn_commit(&txn) == SDB_OK);

        assert(pager.current_lsn == lsn_before + 1U);
        assert(
            pager.wal_tail == tail_before
                + (uint64_t)(2U * frame_size)
                + (uint64_t)SDB_WAL_COMMIT_SIZE
        );
        /*
         * wal_index tracks per-page frame offsets so Task 6 can read
         * from WAL. Frames for txn N start at tail_before.
         */
        frame_offset_p1 = tail_before;
        frame_offset_p2 = tail_before + (uint64_t)frame_size;
        assert(sdb_wal_index_get(&pager.wal_index, p1, &stored_offset));
        assert(stored_offset == frame_offset_p1);
        assert(sdb_wal_index_get(&pager.wal_index, p2, &stored_offset));
        assert(stored_offset == frame_offset_p2);
    }

    /* --- txn 2: rewrite p1 with new value --- */
    {
        sdb_txn txn;
        const uint64_t lsn_before = pager.current_lsn;
        const uint64_t tail_before = pager.wal_tail;
        const uint8_t v1_new[] = "txn-two-rewrites-p1!";

        assert(sdb_txn_begin(&pager, &txn) == SDB_OK);
        assert(sdb_txn_put(
            &txn, p1, (uint16_t)SDB_PAGE_TYPE_DATA, v1_new, sizeof(v1_new)
        ) == SDB_OK);
        assert(sdb_txn_commit(&txn) == SDB_OK);

        assert(pager.current_lsn == lsn_before + 1U);
        assert(
            pager.wal_tail == tail_before
                + (uint64_t)frame_size + (uint64_t)SDB_WAL_COMMIT_SIZE
        );
        /* Newest-wins: p1's frame offset now points at txn 2's frame. */
        assert(sdb_wal_index_get(&pager.wal_index, p1, &stored_offset));
        assert(stored_offset == tail_before);
    }

    /* --- txn 3: p3 --- */
    {
        sdb_txn txn;
        assert(sdb_txn_begin(&pager, &txn) == SDB_OK);
        assert(sdb_txn_put(
            &txn, p3, (uint16_t)SDB_PAGE_TYPE_DATA, v3, sizeof(v3)
        ) == SDB_OK);
        assert(sdb_txn_commit(&txn) == SDB_OK);
    }

    assert(pager.current_lsn == 3U);
    /* Well under a 1000-page threshold — no checkpoint trigger. */
    assert(pager.wal_tail < pager.checkpoint_threshold);
    /*
     * Reads within the same session hit the page cache (updated by
     * commit), so v3 is visible even though read-through-WAL is
     * deferred to Task 6.
     */
    assert_pager_payload(&pager, p3, v3, sizeof(v3));

    assert(sdb_pager_close(&pager) == SDB_OK);

    /* Reopen — recovery must apply all three txns from the WAL. */
    assert(sdb_pager_open(pager_path, &pager) == SDB_OK);
    assert(pager.superblock.checkpoint_lsn == 3U);
    assert(pager.current_lsn == 3U);
    assert(pager.wal_tail == (uint64_t)SDB_WAL_HEADER_SIZE);
    (void)wal_tail_at_start;

    {
        const uint8_t v1_new[] = "txn-two-rewrites-p1!";
        assert_pager_payload(&pager, p1, v1_new, sizeof(v1_new));
    }
    assert_pager_payload(&pager, p2, v2, sizeof(v2));
    assert_pager_payload(&pager, p3, v3, sizeof(v3));

    assert(sdb_pager_close(&pager) == SDB_OK);
    (void)remove(pager_path);
    (void)remove("test-wal-mode-pager.tmp.wal");
    (void)puts(
        "wal-mode pager: multi-txn commit + open recovery round-trip: ok"
    );
}

/*
 * Task 6 — read-through-WAL after cache eviction.
 *
 * Commit a page via a WAL-mode txn (writes the frame to the WAL and populates
 * wal_index; the data file at p1's slot still holds the empty page written
 * by sdb_pager_allocate — no checkpoint has run). Then force p1 out of the
 * page cache by allocating MORE than SDB_PAGER_CACHE_CAPACITY additional
 * pages: each allocate calls sdb_pager_write which sdb_page_cache_puts the
 * fresh page, so the LRU eventually evicts p1's slot. A subsequent read of
 * p1 must return v1 — which can only come from the WAL frame because the
 * data file's p1 slot is still empty. Proves sdb_pager_read consults the
 * wal_index between the page cache and the data file.
 */
static void test_pager_read_through_wal_after_eviction(void)
{
    sdb_pager pager;
    uint8_t salt[16];
    uint8_t file_id[16];
    uint64_t p1;
    const uint8_t v1[] = "wal-read-through-value";
    uint64_t stored_offset = 0U;
    size_t index;

    (void)remove(pager_path);
    (void)remove("test-wal-mode-pager.tmp.wal");
    fill_identity_bytes(salt, (uint8_t)0x33);
    fill_identity_bytes(file_id, (uint8_t)0x44);

    assert(
        sdb_pager_create(pager_path, 4096U, salt, file_id, &pager) == SDB_OK
    );
    assert(sdb_pager_allocate(&pager, &p1) == SDB_OK);

    /* Commit v1 to p1 via the WAL-mode txn path. */
    {
        sdb_txn txn;
        assert(sdb_txn_begin(&pager, &txn) == SDB_OK);
        assert(sdb_txn_put(
            &txn, p1, (uint16_t)SDB_PAGE_TYPE_DATA, v1, sizeof(v1)
        ) == SDB_OK);
        assert(sdb_txn_commit(&txn) == SDB_OK);
    }
    /* wal_index must now record a frame offset for p1. */
    assert(sdb_wal_index_get(&pager.wal_index, p1, &stored_offset));
    assert(stored_offset >= (uint64_t)SDB_WAL_HEADER_SIZE);
    assert(stored_offset < pager.wal_tail);

    /*
     * Evict p1 from the page cache by allocating more than the cache
     * capacity's worth of fresh pages. Each allocate → write puts the
     * new page into the cache with a fresh stamp; p1's stamp is the
     * oldest, so the LRU replaces it.
     */
    for (index = 0U; index < (size_t)SDB_PAGER_CACHE_CAPACITY + 8U; ++index) {
        uint64_t tmp;
        assert(sdb_pager_allocate(&pager, &tmp) == SDB_OK);
    }

    /*
     * Read p1. The data file's p1 slot still holds the empty page written
     * by sdb_pager_allocate (no checkpoint has run), so if the read path
     * doesn't consult the WAL index it will return empty payload. A
     * correct read-through picks up v1 from the WAL frame.
     */
    assert_pager_payload(&pager, p1, v1, sizeof(v1));

    assert(sdb_pager_close(&pager) == SDB_OK);
    (void)remove(pager_path);
    (void)remove("test-wal-mode-pager.tmp.wal");
    (void)puts(
        "wal-mode pager: read-through WAL after cache eviction: ok"
    );
}

/*
 * Task 5 — automatic checkpoint-threshold trigger.
 *
 * With the threshold lowered so a handful of commits trips it, drive many
 * commits to the same page. Once wal_tail crosses checkpoint_threshold the
 * commit path drains the WAL into the data file, so wal_tail snaps back to
 * the header and wal_index empties — proving the WAL cannot grow without
 * bound. The newest value must survive every checkpoint, both in-session and
 * after a close/reopen (the checkpointed data file is authoritative).
 */
static void test_pager_auto_checkpoint_threshold(void)
{
    sdb_pager pager;
    uint8_t salt[16];
    uint8_t file_id[16];
    uint64_t p1;
    const size_t frame_size = sdb_wal_frame_size_v5((size_t)4096U);
    const uint64_t per_commit =
        (uint64_t)frame_size + (uint64_t)SDB_WAL_COMMIT_SIZE;
    bool checkpoint_fired = false;
    size_t i;
    uint8_t value[8];
    uint8_t last_value[8];

    (void)remove(pager_path);
    (void)remove("test-wal-mode-pager.tmp.wal");
    fill_identity_bytes(salt, (uint8_t)0x55);
    fill_identity_bytes(file_id, (uint8_t)0x66);
    assert(
        sdb_pager_create(pager_path, 4096U, salt, file_id, &pager) == SDB_OK
    );
    assert(sdb_pager_allocate(&pager, &p1) == SDB_OK);

    /* Lower the threshold so ~4 commits trip it (default is ~1000 pages). */
    pager.checkpoint_threshold = 4U * per_commit;

    (void)memset(last_value, 0, sizeof(last_value));
    for (i = 0U; i < 20U; ++i) {
        sdb_txn txn;
        uint64_t tail_before;
        (void)memset(value, 0, sizeof(value));
        value[0] = (uint8_t)(i + 1U);
        assert(sdb_txn_begin(&pager, &txn) == SDB_OK);
        assert(sdb_txn_put(
            &txn, p1, (uint16_t)SDB_PAGE_TYPE_DATA, value, sizeof(value)
        ) == SDB_OK);
        tail_before = pager.wal_tail;
        assert(sdb_txn_commit(&txn) == SDB_OK);
        /* wal_tail must never run away far past the threshold. */
        assert(pager.wal_tail <= pager.checkpoint_threshold + per_commit);
        if (pager.wal_tail < tail_before) {
            /* A checkpoint drained the WAL back to the header. */
            checkpoint_fired = true;
            assert(pager.wal_tail == (uint64_t)SDB_WAL_HEADER_SIZE);
            assert(!sdb_wal_index_get(&pager.wal_index, p1, NULL));
        }
        (void)memcpy(last_value, value, sizeof(value));
    }
    assert(checkpoint_fired);
    /* Newest value survived every checkpoint. */
    assert_pager_payload(&pager, p1, last_value, sizeof(last_value));

    assert(sdb_pager_close(&pager) == SDB_OK);
    /* Reopen: the checkpointed data file holds the newest value. */
    assert(sdb_pager_open(pager_path, &pager) == SDB_OK);
    assert_pager_payload(&pager, p1, last_value, sizeof(last_value));
    assert(sdb_pager_close(&pager) == SDB_OK);
    (void)remove(pager_path);
    (void)remove("test-wal-mode-pager.tmp.wal");
    (void)puts(
        "wal-mode pager: auto checkpoint threshold drains WAL: ok"
    );
}

/*
 * R1 Phần A — WAL identity header + recover_all validate gate (bug a).
 *
 * The v3 append path now writes a 64-byte identity header at [0,64) on the
 * first append (append_offset == SDB_WAL_HEADER_SIZE); recover_all validates
 * it before replaying a single frame. A pre-R1 WAL left the header as an
 * all-zero hole — those must still recover (backward-compat).
 */
#define WAL_HDR_VERSION_OFFSET   ((size_t)4)
#define WAL_HDR_SIZE_OFFSET      ((size_t)6)
#define WAL_HDR_PAGE_SIZE_OFFSET ((size_t)16)
#define WAL_HDR_FILE_ID_OFFSET   ((size_t)24)
#define WAL_HDR_CRC_OFFSET       ((size_t)60)

static const uint8_t wal_hdr_magic[4] = {
    (uint8_t)'S', (uint8_t)'W', (uint8_t)'A', (uint8_t)'L'
};

/*
 * Build a WAL holding one committed txn (page `pid`@lsn=1, txn_id=1) via the
 * real append path, then close it. On a post-fix binary the first append
 * stamps the identity header from `sb`.
 */
static void build_single_txn_wal(const sdb_superblock_v1 *sb, uint64_t pid)
{
    sdb_file wal;
    uint8_t page[4096];
    sdb_wal_page pages[1];
    uint64_t next = 0U;

    encode_page(page, (size_t)4096U, pid, 1U);
    pages[0].page_id = pid;
    pages[0].bytes = page;
    (void)remove(wal_path);
    assert(sdb_file_create_new(wal_path, &wal) == SDB_OK);
    assert(
        sdb_wal_append_txn(
            &wal, (uint64_t)SDB_WAL_HEADER_SIZE, sb, 1U, pages, 1U, &next
        ) == SDB_OK
    );
    assert(sdb_file_sync(&wal) == SDB_OK);
    assert(sdb_file_close(&wal) == SDB_OK);
}

/*
 * (a) Foreign file_id: a valid WAL whose identity header carries file_id X is
 * recovered against a superblock with file_id Y. recover_all must refuse with
 * SDB_E_CORRUPT and leave the data file untouched — the core foreign-WAL gate.
 */
static void test_recover_foreign_file_id_refused(void)
{
    sdb_superblock_v1 sb_native;
    sdb_superblock_v1 sb_foreign;
    sdb_file db;
    uint64_t last_lsn = 999U;
    uint64_t recovered_next_page = 0U;
    uint64_t recovered_freelist = 999U;
    bool replayed = true;

    init_scenario_sb(&sb_native);
    fill_identity_bytes(sb_native.file_id, (uint8_t)0xA0);
    build_single_txn_wal(&sb_native, 3U);

    sb_foreign = sb_native;
    fill_identity_bytes(sb_foreign.file_id, (uint8_t)0xB0);

    open_empty_data_file(&db);
    assert(
        sdb_wal_recover_all(
            wal_path, &db, &sb_foreign, NULL,
            &last_lsn, &recovered_next_page, &recovered_freelist, &replayed
        ) == SDB_E_CORRUPT
    );
    assert(replayed == false);
    assert_page_untouched(&db, 3U);

    assert(sdb_file_close(&db) == SDB_OK);
    (void)remove(wal_path);
    (void)remove(db_path);
    (void)puts(
        "wal-mode recover: foreign file_id header refused (data untouched)"
    );
}

static void tear_wal_identity_header(void)
{
    sdb_file wal;
    uint8_t byte;
    assert(sdb_file_open_existing(wal_path, true, &wal) == SDB_OK);
    assert(sdb_file_read_full(&wal, 0U, &byte, 1U) == SDB_OK);
    byte ^= UINT8_C(0x01);
    assert(sdb_file_write_full(&wal, 0U, &byte, 1U) == SDB_OK);
    assert(sdb_file_sync(&wal) == SDB_OK);
    assert(sdb_file_close(&wal) == SDB_OK);
}

static void test_recover_v5_torn_header_uses_frame_identity(void)
{
    sdb_superblock_v1 native;
    sdb_superblock_v1 foreign;
    sdb_file db;
    uint64_t last_lsn;
    uint64_t recovered_next_page;
    uint64_t recovered_freelist;
    bool replayed;

    init_scenario_sb(&native);
    fill_identity_bytes(native.file_id, (uint8_t)0xA8);
    build_single_txn_wal(&native, 3U);
    tear_wal_identity_header();

    last_lsn = 999U;
    recovered_next_page = 0U;
    recovered_freelist = 999U;
    replayed = false;
    open_empty_data_file(&db);
    assert(sdb_wal_recover_all(
        wal_path, &db, &native, NULL,
        &last_lsn, &recovered_next_page, &recovered_freelist, &replayed
    ) == SDB_OK);
    assert(replayed);
    assert_page_lsn(&db, 3U, 1U);
    assert(sdb_file_close(&db) == SDB_OK);

    foreign = native;
    fill_identity_bytes(foreign.file_id, (uint8_t)0xB8);
    last_lsn = 999U;
    recovered_next_page = 0U;
    recovered_freelist = 999U;
    replayed = true;
    open_empty_data_file(&db);
    assert(sdb_wal_recover_all(
        wal_path, &db, &foreign, NULL,
        &last_lsn, &recovered_next_page, &recovered_freelist, &replayed
    ) == SDB_E_CORRUPT);
    assert(!replayed);
    assert_page_untouched(&db, 3U);
    assert(sdb_file_close(&db) == SDB_OK);

    (void)remove(wal_path);
    (void)remove(db_path);
    (void)puts(
        "wal-mode v5: torn header recovers only with frame-bound file_id"
    );
}

/*
 * (b) Wrong page_size in the identity header. The header's CRC is re-sealed
 * after the poke so ONLY the page_size gate can trip.
 */
static void test_recover_header_wrong_page_size_refused(void)
{
    sdb_superblock_v1 sb;
    sdb_file wal;
    sdb_file db;
    uint8_t hdr[SDB_WAL_HEADER_SIZE];
    uint64_t last_lsn = 999U;
    uint64_t recovered_next_page = 0U;
    uint64_t recovered_freelist = 999U;
    bool replayed = true;

    init_scenario_sb(&sb);
    fill_identity_bytes(sb.file_id, (uint8_t)0xC0);
    build_single_txn_wal(&sb, 3U);

    assert(sdb_file_open_existing(wal_path, true, &wal) == SDB_OK);
    assert(sdb_file_read_full(&wal, 0U, hdr, SDB_WAL_HEADER_SIZE) == SDB_OK);
    sdb_write_u32_le(hdr + WAL_HDR_PAGE_SIZE_OFFSET, sb.page_size * 2U);
    sdb_write_u32_le(
        hdr + WAL_HDR_CRC_OFFSET,
        sdb_crc32_zeroed_range(
            hdr, SDB_WAL_HEADER_SIZE, WAL_HDR_CRC_OFFSET, sizeof(uint32_t)
        )
    );
    assert(sdb_file_write_full(&wal, 0U, hdr, SDB_WAL_HEADER_SIZE) == SDB_OK);
    assert(sdb_file_sync(&wal) == SDB_OK);
    assert(sdb_file_close(&wal) == SDB_OK);

    open_empty_data_file(&db);
    assert(
        sdb_wal_recover_all(
            wal_path, &db, &sb, NULL,
            &last_lsn, &recovered_next_page, &recovered_freelist, &replayed
        ) == SDB_E_CORRUPT
    );
    assert(replayed == false);
    assert_page_untouched(&db, 3U);

    assert(sdb_file_close(&db) == SDB_OK);
    (void)remove(wal_path);
    (void)remove(db_path);
    (void)puts("wal-mode recover: header page_size mismatch refused");
}

/*
 * (c) Corrupt magic. CRC is re-sealed after the poke so ONLY the magic gate
 * can trip.
 */
static void test_recover_header_bad_magic_refused(void)
{
    sdb_superblock_v1 sb;
    sdb_file wal;
    sdb_file db;
    uint8_t hdr[SDB_WAL_HEADER_SIZE];
    uint64_t last_lsn = 999U;
    uint64_t recovered_next_page = 0U;
    uint64_t recovered_freelist = 999U;
    bool replayed = true;

    init_scenario_sb(&sb);
    fill_identity_bytes(sb.file_id, (uint8_t)0xD0);
    build_single_txn_wal(&sb, 3U);

    assert(sdb_file_open_existing(wal_path, true, &wal) == SDB_OK);
    assert(sdb_file_read_full(&wal, 0U, hdr, SDB_WAL_HEADER_SIZE) == SDB_OK);
    hdr[0] = (uint8_t)'X'; /* "XWAL" — no longer the WAL magic. */
    sdb_write_u32_le(
        hdr + WAL_HDR_CRC_OFFSET,
        sdb_crc32_zeroed_range(
            hdr, SDB_WAL_HEADER_SIZE, WAL_HDR_CRC_OFFSET, sizeof(uint32_t)
        )
    );
    assert(sdb_file_write_full(&wal, 0U, hdr, SDB_WAL_HEADER_SIZE) == SDB_OK);
    assert(sdb_file_sync(&wal) == SDB_OK);
    assert(sdb_file_close(&wal) == SDB_OK);

    open_empty_data_file(&db);
    assert(
        sdb_wal_recover_all(
            wal_path, &db, &sb, NULL,
            &last_lsn, &recovered_next_page, &recovered_freelist, &replayed
        ) == SDB_E_CORRUPT
    );
    assert(replayed == false);
    assert_page_untouched(&db, 3U);

    assert(sdb_file_close(&db) == SDB_OK);
    (void)remove(wal_path);
    (void)remove(db_path);
    (void)puts("wal-mode recover: header bad magic refused");
}

/*
 * (d) Regression: a WAL whose identity header matches the superblock replays
 * every txn. Also asserts the write path actually stamped a well-formed
 * header (magic/version/size/page_size/file_id/CRC).
 */
static void test_recover_header_identity_replays_matching(void)
{
    sdb_superblock_v1 sb;
    sdb_file wal;
    sdb_file db;
    uint8_t hdr[SDB_WAL_HEADER_SIZE];
    uint64_t off_after_3 = 0U;
    uint64_t last_lsn = 999U;
    uint64_t recovered_next_page = 0U;
    uint64_t recovered_freelist = 999U;
    bool replayed = false;

    init_scenario_sb(&sb);
    fill_identity_bytes(sb.file_id, (uint8_t)0x77);
    build_three_committed_txns(&sb, &wal, &off_after_3);
    assert(sdb_file_sync(&wal) == SDB_OK);
    assert(sdb_file_close(&wal) == SDB_OK);

    /* Write-path: the identity header must be present and well-formed. */
    assert(sdb_file_open_existing(wal_path, false, &wal) == SDB_OK);
    assert(sdb_file_read_full(&wal, 0U, hdr, SDB_WAL_HEADER_SIZE) == SDB_OK);
    assert(sdb_file_close(&wal) == SDB_OK);
    assert(memcmp(hdr, wal_hdr_magic, sizeof(wal_hdr_magic)) == 0);
    assert(
        sdb_read_u16_le(hdr + WAL_HDR_VERSION_OFFSET) == SDB_WAL_VERSION_V5
    );
    assert(
        sdb_read_u16_le(hdr + WAL_HDR_SIZE_OFFSET)
            == (uint16_t)SDB_WAL_HEADER_SIZE
    );
    assert(sdb_read_u32_le(hdr + WAL_HDR_PAGE_SIZE_OFFSET) == sb.page_size);
    assert(
        memcmp(hdr + WAL_HDR_FILE_ID_OFFSET, sb.file_id, SDB_FILE_ID_SIZE) == 0
    );
    assert(
        sdb_crc32_zeroed_range(
            hdr, SDB_WAL_HEADER_SIZE, WAL_HDR_CRC_OFFSET, sizeof(uint32_t)
        ) == sdb_read_u32_le(hdr + WAL_HDR_CRC_OFFSET)
    );

    /* Read-path: matching identity → replay all three txns. */
    open_empty_data_file(&db);
    assert(
        sdb_wal_recover_all(
            wal_path, &db, &sb, NULL,
            &last_lsn, &recovered_next_page, &recovered_freelist, &replayed
        ) == SDB_OK
    );
    assert(replayed == true);
    assert(last_lsn == 3U);
    assert_page_lsn(&db, 3U, 2U);
    assert_page_lsn(&db, 5U, 1U);
    assert_page_lsn(&db, 7U, 1U);
    assert_page_lsn(&db, 9U, 2U);
    assert_page_lsn(&db, 11U, 3U);

    assert(sdb_file_close(&db) == SDB_OK);
    (void)remove(wal_path);
    (void)remove(db_path);
    (void)puts(
        "wal-mode recover: matching identity header replays (write+read path)"
    );
}

/*
 * (e) Security policy: a pre-R1 plaintext WAL with an all-zero identity-header
 * hole is not bound to any database. Refuse it instead of replaying a foreign
 * sidecar. Legacy v3 remains compatible when its CRC-valid header carries the
 * matching file_id (covered below).
 */
static void build_v3_wal_manual(const sdb_superblock_v1 *sb, bool zero_header);

static void test_recover_zero_hole_plaintext_refused(void)
{
    sdb_superblock_v1 sb;
    sdb_file db;
    uint64_t last_lsn = 999U;
    uint64_t recovered_next_page = 0U;
    uint64_t recovered_freelist = 999U;
    bool replayed = false;

    init_scenario_sb(&sb);
    fill_identity_bytes(sb.file_id, (uint8_t)0xE0);
    /*
     * A true pre-R1 WAL: all-zero header hole AND legacy 8-byte v3 frames
     * (a pre-Part-B binary never wrote a v4 frame). With no authenticated page
     * key and no file_id anywhere in the WAL, ownership is unknowable.
     */
    build_v3_wal_manual(&sb, true);

    open_empty_data_file(&db);
    assert(
        sdb_wal_recover_all(
            wal_path, &db, &sb, NULL,
            &last_lsn, &recovered_next_page, &recovered_freelist, &replayed
        ) == SDB_E_CORRUPT
    );
    assert(replayed == false);
    assert(last_lsn == sb.checkpoint_lsn);

    assert(sdb_file_close(&db) == SDB_OK);
    (void)remove(wal_path);
    (void)remove(db_path);
    (void)puts(
        "wal-mode recover: unbound pre-R1 plaintext WAL refused"
    );
}

/*
 * R1 Phần B — self-describing frames (WAL v5 current, v4 compatible).
 *
 * (f) The write path stamps a 24-byte v4 frame header
 * [page_id | txn_id | frame_index | frame_count] and a v4 identity header.
 * Reading the raw bytes back must show exactly that layout: every frame in
 * the txn carries the same txn_id/frame_count and a contiguous frame_index.
 */
static void test_frame_v5_self_describing_layout(void)
{
    sdb_superblock_v1 sb;
    sdb_file wal;
    uint8_t page0[4096];
    uint8_t page1[4096];
    uint8_t page2[4096];
    sdb_wal_page pages[3];
    uint64_t next = 0U;
    const size_t fs = sdb_wal_frame_size_v5((size_t)4096U);
    const uint64_t base = (uint64_t)SDB_WAL_HEADER_SIZE;
    uint8_t fhdr[SDB_WAL_RECORD_HEADER_SIZE_V5];
    uint8_t hdr[SDB_WAL_HEADER_SIZE];
    size_t idx;
    static const uint64_t ids[3] = { 3U, 5U, 7U };

    init_scenario_sb(&sb);
    fill_identity_bytes(sb.file_id, (uint8_t)0x10);
    encode_page(page0, (size_t)4096U, 3U, 7U);
    encode_page(page1, (size_t)4096U, 5U, 7U);
    encode_page(page2, (size_t)4096U, 7U, 7U);
    pages[0].page_id = 3U;
    pages[0].bytes = page0;
    pages[1].page_id = 5U;
    pages[1].bytes = page1;
    pages[2].page_id = 7U;
    pages[2].bytes = page2;

    (void)remove(wal_path);
    assert(sdb_file_create_new(wal_path, &wal) == SDB_OK);
    assert(sdb_wal_append_txn(&wal, base, &sb, 7U, pages, 3U, &next) == SDB_OK);
    assert(next == base + 3U * (uint64_t)fs + (uint64_t)SDB_WAL_COMMIT_SIZE);

    for (idx = 0U; idx < 3U; ++idx) {
        assert(
            sdb_file_read_full(
                &wal, base + (uint64_t)idx * (uint64_t)fs, fhdr, sizeof(fhdr)
            ) == SDB_OK
        );
        assert(sdb_read_u64_le(fhdr) == ids[idx]);
        assert(
            sdb_read_u64_le(fhdr + SDB_WAL_FRAME_V4_TXN_ID_OFFSET) == 7U
        );
        assert(
            sdb_read_u32_le(fhdr + SDB_WAL_FRAME_V4_FRAME_INDEX_OFFSET)
                == (uint32_t)idx
        );
        assert(
            sdb_read_u32_le(fhdr + SDB_WAL_FRAME_V4_FRAME_COUNT_OFFSET) == 3U
        );
        assert(memcmp(
            fhdr + SDB_WAL_FRAME_V5_FILE_ID_OFFSET,
            sb.file_id,
            SDB_FILE_ID_SIZE
        ) == 0);
    }

    assert(sdb_file_read_full(&wal, 0U, hdr, sizeof(hdr)) == SDB_OK);
    assert(sdb_read_u16_le(hdr + WAL_HDR_VERSION_OFFSET) == SDB_WAL_VERSION_V5);

    assert(sdb_file_close(&wal) == SDB_OK);
    (void)remove(wal_path);
    (void)puts(
        "wal-mode v5: self-describing bound frame layout "
        "(page/txn/idx/count/file_id)"
    );
}

/*
 * Encode a page whose payload is packed with the commit magic 'SCMT' so the
 * raw frame bytes are littered with it.
 */
static void encode_page_scmt(
    uint8_t *buf, size_t page_size, uint64_t page_id, uint64_t page_lsn
)
{
    uint8_t payload[256];
    size_t i;
    for (i = 0U; i < sizeof(payload); i += 4U) {
        payload[i] = (uint8_t)'S';
        payload[i + 1U] = (uint8_t)'C';
        payload[i + 2U] = (uint8_t)'M';
        payload[i + 3U] = (uint8_t)'T';
    }
    assert(
        sdb_page_encode(
            buf, page_size, (uint16_t)SDB_PAGE_TYPE_DATA,
            page_id, page_lsn, payload, sizeof(payload)
        ) == SDB_OK
    );
}

/*
 * (g) Deterministic txn boundary: a v4 txn whose page data is stuffed with
 * 'SCMT' must NOT be mistaken for an early commit-record. Recovery reads
 * frame_count from the first frame, so both txns replay in full.
 */
static void test_recover_v4_scmt_in_page_data_not_torn(void)
{
    sdb_superblock_v1 sb;
    sdb_file wal;
    sdb_file db;
    uint8_t p3[4096];
    uint8_t p5[4096];
    uint8_t p7[4096];
    sdb_wal_page pages[2];
    uint64_t off = (uint64_t)SDB_WAL_HEADER_SIZE;
    uint64_t next = 0U;
    uint64_t last_lsn = 999U;
    uint64_t recovered_next_page = 0U;
    uint64_t recovered_freelist = 999U;
    bool replayed = false;

    init_scenario_sb(&sb);
    fill_identity_bytes(sb.file_id, (uint8_t)0x20);

    /* txn 1: two frames, both stuffed with 'SCMT' in the page body. */
    encode_page_scmt(p3, (size_t)4096U, 3U, 1U);
    encode_page_scmt(p5, (size_t)4096U, 5U, 1U);
    pages[0].page_id = 3U;
    pages[0].bytes = p3;
    pages[1].page_id = 5U;
    pages[1].bytes = p5;
    (void)remove(wal_path);
    assert(sdb_file_create_new(wal_path, &wal) == SDB_OK);
    assert(sdb_wal_append_txn(&wal, off, &sb, 1U, pages, 2U, &next) == SDB_OK);
    off = next;

    /* txn 2: one frame, also stuffed. */
    encode_page_scmt(p7, (size_t)4096U, 7U, 2U);
    pages[0].page_id = 7U;
    pages[0].bytes = p7;
    assert(sdb_wal_append_txn(&wal, off, &sb, 2U, pages, 1U, &next) == SDB_OK);
    assert(sdb_file_sync(&wal) == SDB_OK);
    assert(sdb_file_close(&wal) == SDB_OK);

    open_empty_data_file(&db);
    assert(
        sdb_wal_recover_all(
            wal_path, &db, &sb, NULL,
            &last_lsn, &recovered_next_page, &recovered_freelist, &replayed
        ) == SDB_OK
    );
    assert(replayed == true);
    assert(last_lsn == 2U);
    assert_page_lsn(&db, 3U, 1U);
    assert_page_lsn(&db, 5U, 1U);
    assert_page_lsn(&db, 7U, 2U);

    assert(sdb_file_close(&db) == SDB_OK);
    (void)remove(wal_path);
    (void)remove(db_path);
    (void)puts(
        "wal-mode v4: 'SCMT' in page data does not fool boundary detection"
    );
}

/*
 * Hand-build a legacy v3 WAL (8-byte frame headers, commit-record version=3)
 * that a pre-Part-B binary would have written. With zero_header=false the
 * 64-byte identity header carries version 3 (post-Part-A layout); with
 * zero_header=true it is left as an all-zero hole (a true pre-R1 WAL). Both
 * must recover through the preserved v3 magic-scan path.
 */
static void build_v3_wal_manual(const sdb_superblock_v1 *sb, bool zero_header)
{
    sdb_file wal;
    const size_t page_size = (size_t)4096U;
    const size_t fs = sdb_wal_frame_size(page_size); /* v3: 8 + page_size */
    uint8_t *buf;
    uint8_t commit[SDB_WAL_COMMIT_SIZE];
    sdb_wal_commit_rec rec;
    uint32_t running_crc;
    size_t frames_bytes = 2U * fs;
    size_t total = SDB_WAL_HEADER_SIZE + frames_bytes;
    uint8_t page[4096];
    size_t i;
    static const uint64_t ids[2] = { 3U, 5U };

    buf = (uint8_t *)calloc(total, 1U);
    assert(buf != NULL);

    /* v3 identity header at [0,64). */
    buf[0] = (uint8_t)'S';
    buf[1] = (uint8_t)'W';
    buf[2] = (uint8_t)'A';
    buf[3] = (uint8_t)'L';
    sdb_write_u16_le(buf + WAL_HDR_VERSION_OFFSET, SDB_WAL_VERSION_V3);
    sdb_write_u16_le(buf + WAL_HDR_SIZE_OFFSET, (uint16_t)SDB_WAL_HEADER_SIZE);
    sdb_write_u64_le(buf + 8U, 1U);
    sdb_write_u32_le(buf + WAL_HDR_PAGE_SIZE_OFFSET, sb->page_size);
    sdb_write_u32_le(buf + 20U, 2U);
    (void)memcpy(buf + WAL_HDR_FILE_ID_OFFSET, sb->file_id, SDB_FILE_ID_SIZE);
    sdb_write_u64_le(buf + 40U, sb->next_page_id);
    sdb_write_u64_le(buf + 48U, sb->freelist_page);
    sdb_write_u32_le(
        buf + WAL_HDR_CRC_OFFSET,
        sdb_crc32_zeroed_range(
            buf, SDB_WAL_HEADER_SIZE, WAL_HDR_CRC_OFFSET, sizeof(uint32_t)
        )
    );

    /* Pre-R1 WALs left the header slot as an all-zero hole. */
    if (zero_header) {
        (void)memset(buf, 0, SDB_WAL_HEADER_SIZE);
    }

    /* Two v3 frames: [page_id:8][page bytes]. */
    for (i = 0U; i < 2U; ++i) {
        uint8_t *frame = buf + SDB_WAL_HEADER_SIZE + i * fs;
        encode_page(page, page_size, ids[i], 1U);
        sdb_write_u64_le(frame, ids[i]);
        (void)memcpy(frame + SDB_WAL_RECORD_HEADER_SIZE, page, page_size);
    }

    /*
     * v3 commit-record: encode, then force version=3 and re-seal its CRC so
     * this reproduces a pre-Part-B image regardless of the current writer's
     * commit-record version.
     */
    running_crc = sdb_crc32(buf + SDB_WAL_HEADER_SIZE, frames_bytes);
    rec.txn_id = 1U;
    rec.frame_count = 2U;
    rec.next_page_id = sb->next_page_id;
    rec.freelist_page = sb->freelist_page;
    sdb_wal_encode_commit_rec(commit, &rec, running_crc);
    sdb_write_u16_le(commit + 4U, SDB_WAL_VERSION_V3);
    sdb_write_u32_le(commit + 40U, 0U);
    sdb_write_u32_le(
        commit + 40U,
        sdb_crc32_zeroed_range(commit, SDB_WAL_COMMIT_SIZE, 40U, sizeof(uint32_t))
    );

    (void)remove(wal_path);
    assert(sdb_file_create_new(wal_path, &wal) == SDB_OK);
    assert(sdb_file_write_full(&wal, 0U, buf, total) == SDB_OK);
    assert(
        sdb_file_write_full(&wal, (uint64_t)total, commit, SDB_WAL_COMMIT_SIZE)
            == SDB_OK
    );
    assert(sdb_file_sync(&wal) == SDB_OK);
    assert(sdb_file_close(&wal) == SDB_OK);
    free(buf);
}

static void build_v4_wal_manual(const sdb_superblock_v1 *sb)
{
    sdb_file wal;
    const size_t page_size = (size_t)4096U;
    const size_t fs = sdb_wal_frame_size_v4(page_size);
    const size_t frames_bytes = 2U * fs;
    const size_t total = SDB_WAL_HEADER_SIZE + frames_bytes
        + SDB_WAL_COMMIT_SIZE;
    uint8_t *buf = (uint8_t *)calloc(total, 1U);
    uint8_t page[4096];
    sdb_wal_commit_rec rec;
    uint8_t *commit;
    uint32_t running_crc;
    size_t i;
    static const uint64_t ids[2] = { 3U, 5U };
    assert(buf != NULL);

    (void)memcpy(buf, wal_hdr_magic, sizeof(wal_hdr_magic));
    sdb_write_u16_le(buf + WAL_HDR_VERSION_OFFSET, SDB_WAL_VERSION_V4);
    sdb_write_u16_le(buf + WAL_HDR_SIZE_OFFSET, (uint16_t)SDB_WAL_HEADER_SIZE);
    sdb_write_u64_le(buf + 8U, 1U);
    sdb_write_u32_le(buf + WAL_HDR_PAGE_SIZE_OFFSET, sb->page_size);
    sdb_write_u32_le(buf + 20U, 2U);
    (void)memcpy(buf + WAL_HDR_FILE_ID_OFFSET, sb->file_id, SDB_FILE_ID_SIZE);
    sdb_write_u64_le(buf + 40U, sb->next_page_id);
    sdb_write_u64_le(buf + 48U, sb->freelist_page);
    sdb_write_u32_le(
        buf + WAL_HDR_CRC_OFFSET,
        sdb_crc32_zeroed_range(
            buf, SDB_WAL_HEADER_SIZE, WAL_HDR_CRC_OFFSET, sizeof(uint32_t)
        )
    );
    for (i = 0U; i < 2U; ++i) {
        uint8_t *frame = buf + SDB_WAL_HEADER_SIZE + i * fs;
        encode_page(page, page_size, ids[i], 1U);
        sdb_write_u64_le(frame, ids[i]);
        sdb_write_u64_le(frame + SDB_WAL_FRAME_V4_TXN_ID_OFFSET, 1U);
        sdb_write_u32_le(
            frame + SDB_WAL_FRAME_V4_FRAME_INDEX_OFFSET, (uint32_t)i
        );
        sdb_write_u32_le(frame + SDB_WAL_FRAME_V4_FRAME_COUNT_OFFSET, 2U);
        (void)memcpy(frame + SDB_WAL_RECORD_HEADER_SIZE_V4, page, page_size);
    }
    running_crc = sdb_crc32(buf + SDB_WAL_HEADER_SIZE, frames_bytes);
    rec.txn_id = 1U;
    rec.frame_count = 2U;
    rec.next_page_id = sb->next_page_id;
    rec.freelist_page = sb->freelist_page;
    commit = buf + SDB_WAL_HEADER_SIZE + frames_bytes;
    sdb_wal_encode_commit_rec(commit, &rec, running_crc);
    sdb_write_u16_le(commit + 4U, SDB_WAL_VERSION_V4);
    sdb_write_u32_le(commit + 40U, 0U);
    sdb_write_u32_le(
        commit + 40U,
        sdb_crc32_zeroed_range(
            commit, SDB_WAL_COMMIT_SIZE, 40U, sizeof(uint32_t)
        )
    );

    (void)remove(wal_path);
    assert(sdb_file_create_new(wal_path, &wal) == SDB_OK);
    assert(sdb_file_write_full(&wal, 0U, buf, total) == SDB_OK);
    assert(sdb_file_sync(&wal) == SDB_OK);
    assert(sdb_file_close(&wal) == SDB_OK);
    free(buf);
}

static void test_recover_v4_backward_compat(void)
{
    sdb_superblock_v1 sb;
    sdb_file db;
    uint64_t last_lsn = 999U;
    uint64_t recovered_next_page = 0U;
    uint64_t recovered_freelist = 999U;
    bool replayed = false;
    init_scenario_sb(&sb);
    fill_identity_bytes(sb.file_id, (uint8_t)0x38);
    build_v4_wal_manual(&sb);
    open_empty_data_file(&db);
    assert(sdb_wal_recover_all(
        wal_path, &db, &sb, NULL,
        &last_lsn, &recovered_next_page, &recovered_freelist, &replayed
    ) == SDB_OK);
    assert(replayed);
    assert(last_lsn == 1U);
    assert_page_lsn(&db, 3U, 1U);
    assert_page_lsn(&db, 5U, 1U);
    assert(sdb_file_close(&db) == SDB_OK);
    (void)remove(wal_path);
    (void)remove(db_path);
    (void)puts("wal-mode v5 reader: bound legacy v4 WAL recovers");
}

/*
 * (h) Backward-compat: a legacy v3 WAL (8-byte frames, header version=3)
 * must still recover with the v4-capable binary via the preserved v3 scan.
 */
static void test_recover_v3_backward_compat(void)
{
    sdb_superblock_v1 sb;
    sdb_file db;
    uint64_t last_lsn = 999U;
    uint64_t recovered_next_page = 0U;
    uint64_t recovered_freelist = 999U;
    bool replayed = false;

    init_scenario_sb(&sb);
    fill_identity_bytes(sb.file_id, (uint8_t)0x30);
    build_v3_wal_manual(&sb, false);

    open_empty_data_file(&db);
    assert(
        sdb_wal_recover_all(
            wal_path, &db, &sb, NULL,
            &last_lsn, &recovered_next_page, &recovered_freelist, &replayed
        ) == SDB_OK
    );
    assert(replayed == true);
    assert(last_lsn == 1U);
    assert(recovered_next_page == sb.next_page_id);
    assert_page_lsn(&db, 3U, 1U);
    assert_page_lsn(&db, 5U, 1U);

    assert(sdb_file_close(&db) == SDB_OK);
    (void)remove(wal_path);
    (void)remove(db_path);
    (void)puts(
        "wal-mode v4 binary: legacy v3 WAL still recovers (backward-compat)"
    );
}

int main(void)
{
    test_frame_v5_self_describing_layout();
    test_recover_v4_scmt_in_page_data_not_torn();
    test_recover_v3_backward_compat();
    test_recover_v4_backward_compat();
    test_append_two_txns();
    test_recover_truncated_commit_rec();
    test_recover_corrupt_commit_rec_crc();
    test_recover_out_of_order_txn_id();
    test_recover_frame_body_corruption();
    test_recover_foreign_file_id_refused();
    test_recover_v5_torn_header_uses_frame_identity();
    test_recover_header_wrong_page_size_refused();
    test_recover_header_bad_magic_refused();
    test_recover_header_identity_replays_matching();
    test_recover_zero_hole_plaintext_refused();
    test_pager_wal_mode_multi_txn_round_trip();
    test_pager_read_through_wal_after_eviction();
    test_pager_auto_checkpoint_threshold();
    (void)puts("wal mode: ok");
    return 0;
}
