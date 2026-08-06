
#include "shibadb.h"
#include "file.h"
#include "internal.h"
#include "page.h"
#include "wal.h"

#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

#define SDB_FUZZ_WAL_PAGE_SIZE ((uint32_t)4096)
#define SDB_FUZZ_WAL_MAX_INPUT ((size_t)(1U << 20))

/*
 * Structured-builder shape. Kept small so recovery + apply stay fast, but
 * still large enough for libFuzzer to explore multi-txn chains, gaps,
 * torn tails, and cross-txn CRC drift.
 */
#define SDB_FUZZ_WAL_MAX_TXNS   ((size_t)6U)
#define SDB_FUZZ_WAL_MAX_FRAMES ((size_t)4U)
#define SDB_FUZZ_WAL_INIT_NEXT_PAGE_ID ((uint64_t)4U)
#define SDB_FUZZ_WAL_PAGE_ID_CEIL      ((uint64_t)128U)

/*
 * v3 commit-record layout duplicated from src/wal.c. The static asserts
 * anchor these constants to SDB_WAL_COMMIT_SIZE from the public header so
 * a format change here forces a rebuild-time failure rather than a silent
 * fuzz-time drift.
 */
#define SDB_FUZZ_V3_COMMIT_CHECKSUM_OFFSET ((size_t)40)
_Static_assert(
    SDB_WAL_COMMIT_SIZE
        == SDB_FUZZ_V3_COMMIT_CHECKSUM_OFFSET + sizeof(uint32_t),
    "fuzz commit-checksum offset must match src/wal.c layout"
);

/*
 * 64-byte v4 identity-header layout, duplicated from the sdb_wal_append_txn
 * writer in src/wal.c. recover_all's identity gate reads magic/version/
 * header_size/page_size/file_id from these offsets and refuses a foreign or
 * corrupt-CRC-but-mismatched sidecar before touching a frame. The static
 * asserts anchor the field spacing to the public header constants so a layout
 * drift fails the build instead of silently under-fuzzing the v4 path.
 */
#define SDB_FUZZ_WAL_HDR_VERSION_OFFSET     ((size_t)4)
#define SDB_FUZZ_WAL_HDR_HDRSIZE_OFFSET     ((size_t)6)
#define SDB_FUZZ_WAL_HDR_TXN_ID_OFFSET      ((size_t)8)
#define SDB_FUZZ_WAL_HDR_PAGE_SIZE_OFFSET   ((size_t)16)
#define SDB_FUZZ_WAL_HDR_PAGE_COUNT_OFFSET  ((size_t)20)
#define SDB_FUZZ_WAL_HDR_FILE_ID_OFFSET     ((size_t)24)
#define SDB_FUZZ_WAL_HDR_NEXT_PAGE_OFFSET   ((size_t)40)
#define SDB_FUZZ_WAL_HDR_FREELIST_OFFSET    ((size_t)48)
#define SDB_FUZZ_WAL_HDR_CHECKSUM_OFFSET    ((size_t)60)
_Static_assert(
    SDB_FUZZ_WAL_HDR_CHECKSUM_OFFSET + sizeof(uint32_t) == SDB_WAL_HEADER_SIZE,
    "fuzz WAL header checksum offset must match src/wal.c layout"
);
_Static_assert(
    SDB_FUZZ_WAL_HDR_FILE_ID_OFFSET + SDB_FILE_ID_SIZE
        == SDB_FUZZ_WAL_HDR_NEXT_PAGE_OFFSET,
    "fuzz WAL header file_id slot must abut next_page_id"
);
_Static_assert(
    SDB_WAL_RECORD_HEADER_SIZE_V4 == SDB_WAL_FRAME_V4_FRAME_COUNT_OFFSET + 4U,
    "fuzz v4 frame header must end after frame_count"
);

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
    sb->next_page_id = SDB_FUZZ_WAL_INIT_NEXT_PAGE_ID;
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

    fd = open(g_db_path, 1  );
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

/* ---------- single-transaction v1/v2 path (legacy coverage) ----------- */

static void run_recover_single(const uint8_t *data, size_t size)
{
    sdb_file db;
    uint64_t txn_id = UINT64_MAX;
    uint64_t next_page_id = UINT64_MAX;
    uint64_t freelist_page = UINT64_MAX;
    bool replayed = true;
    sdb_status status;

    write_wal(data, size);

    if (sdb_file_open_existing(g_db_path, true, &db) != SDB_OK) {
        return;
    }
    status = sdb_wal_recover(
        g_wal_path, &db, &g_sb, NULL,
        &txn_id, &next_page_id, &freelist_page, &replayed
    );
    (void)sdb_file_close(&db);

    if (status != SDB_OK) {

        if (replayed
            || (next_page_id != UINT64_MAX
                && next_page_id != g_sb.next_page_id)
            || (freelist_page != UINT64_MAX
                && freelist_page != g_sb.freelist_page)) {
            __builtin_trap();
        }
    }
}

/* ---------- multi-transaction v3 path (recover_all) ------------------- */

static void run_recover_all(const uint8_t *data, size_t size)
{
    sdb_file db;
    uint64_t last_lsn = UINT64_MAX;
    uint64_t next_page_id = UINT64_MAX;
    uint64_t freelist_page = UINT64_MAX;
    bool replayed = true;
    sdb_status status;

    write_wal(data, size);

    if (sdb_file_open_existing(g_db_path, true, &db) != SDB_OK) {
        return;
    }
    status = sdb_wal_recover_all(
        g_wal_path, &db, &g_sb, NULL,
        &last_lsn, &next_page_id, &freelist_page, &replayed
    );
    (void)sdb_file_close(&db);

    /*
     * recover_all initializes out-params to the superblock values before
     * touching disk. On any non-SDB_OK return the recovered state must
     * still equal that initial snapshot and nothing may claim to have
     * been replayed. Any drift here indicates a partial-apply escape.
     */
    if (status != SDB_OK) {
        if (replayed
            || last_lsn != g_sb.checkpoint_lsn
            || next_page_id != g_sb.next_page_id
            || freelist_page != g_sb.freelist_page) {
            __builtin_trap();
        }
    } else {
        /*
         * On success, txn_ids must be at least the checkpoint LSN and the
         * allocation watermark must be non-shrinking.
         */
        if (last_lsn < g_sb.checkpoint_lsn
            || next_page_id < g_sb.next_page_id) {
            __builtin_trap();
        }
    }
}

/* ---------- structured v3 multi-txn image builder --------------------- */

typedef struct fuzz_reader {
    const uint8_t *buf;
    size_t len;
    size_t pos;
} fuzz_reader;

static uint8_t reader_u8(fuzz_reader *r)
{
    if (r->pos < r->len) {
        return r->buf[r->pos++];
    }
    return 0U;
}

static uint64_t reader_u64(fuzz_reader *r)
{
    uint64_t v = 0U;
    unsigned i;
    for (i = 0U; i < 8U; ++i) {
        v |= (uint64_t)reader_u8(r) << (i * 8U);
    }
    return v;
}

static void reader_fill(fuzz_reader *r, uint8_t *dst, size_t n)
{
    size_t take = 0U;
    if (r->pos < r->len) {
        take = r->len - r->pos;
        if (take > n) {
            take = n;
        }
        (void)memcpy(dst, r->buf + r->pos, take);
        r->pos += take;
    }
    if (take < n) {
        (void)memset(dst + take, 0, n - take);
    }
}

/*
 * Build a v3 multi-txn WAL image driven by the fuzz input. Every field
 * that has meaning during recovery (txn_id, frame_count, next_page_id,
 * freelist_page, running_crc, commit CRC, magic) has an independent
 * corrupt / valid switch, and the tail can be truncated at any offset
 * to exercise the torn-tail short-circuit in sdb_wal_recover_all.
 */
static size_t build_v3_image(
    fuzz_reader *r, uint8_t *out, size_t cap
)
{
    const size_t page_size = (size_t)SDB_FUZZ_WAL_PAGE_SIZE;
    const size_t frame_size = SDB_WAL_RECORD_HEADER_SIZE + page_size;
    size_t offset;
    size_t txn_count;
    size_t txn_index;
    uint64_t expected_next;
    uint64_t running_next_page_id;
    size_t body_end;
    size_t body_len;
    size_t cut;

    if (cap < SDB_WAL_HEADER_SIZE + frame_size + SDB_WAL_COMMIT_SIZE) {
        return 0U;
    }

    (void)memset(out, 0, SDB_WAL_HEADER_SIZE);
    offset = SDB_WAL_HEADER_SIZE;
    txn_count = 1U + ((size_t)reader_u8(r) % SDB_FUZZ_WAL_MAX_TXNS);
    expected_next = g_sb.checkpoint_lsn + 1U;
    running_next_page_id = g_sb.next_page_id;

    for (txn_index = 0U; txn_index < txn_count; ++txn_index) {
        size_t frame_count;
        size_t txn_size;
        uint8_t *frames_start;
        uint8_t *commit_ptr;
        size_t frame_idx;
        uint32_t running_crc;
        uint32_t running_crc_field;
        sdb_wal_commit_rec rec;
        uint64_t txn_id;
        uint64_t new_next_page_id;
        uint64_t new_freelist;
        uint32_t declared_frame_count;
        uint8_t choice;

        frame_count = 1U + ((size_t)reader_u8(r) % SDB_FUZZ_WAL_MAX_FRAMES);
        txn_size = frame_count * frame_size + SDB_WAL_COMMIT_SIZE;
        if (offset + txn_size > cap) {
            break;
        }

        /* --- txn_id: monotone / off-by-one / random. --- */
        choice = reader_u8(r);
        switch (choice & 0x3U) {
        case 0U:
            txn_id = expected_next;
            break;
        case 1U:
            txn_id = expected_next + 1U; /* gap: rejected as torn. */
            break;
        case 2U:
            txn_id = (expected_next > 1U)
                ? expected_next - 1U
                : expected_next;
            break;
        default:
            txn_id = (uint64_t)reader_u8(r) + 1U;
            break;
        }

        /* --- next_page_id: grow / same / shrink / random small. --- */
        choice = reader_u8(r);
        switch (choice & 0x3U) {
        case 0U:
            new_next_page_id =
                running_next_page_id + ((uint64_t)reader_u8(r) % 8U) + 1U;
            break;
        case 1U:
            new_next_page_id = running_next_page_id;
            break;
        case 2U:
            new_next_page_id = (running_next_page_id > 1U)
                ? running_next_page_id - 1U
                : running_next_page_id;
            break;
        default:
            new_next_page_id = (uint64_t)reader_u8(r);
            break;
        }
        if (new_next_page_id > SDB_FUZZ_WAL_PAGE_ID_CEIL) {
            new_next_page_id = SDB_FUZZ_WAL_PAGE_ID_CEIL;
        }
        if (new_next_page_id < 2U) {
            new_next_page_id = 2U;
        }

        /* --- freelist: 0 / in-range / out-of-range / random. --- */
        choice = reader_u8(r);
        switch (choice & 0x3U) {
        case 0U:
            new_freelist = 0U;
            break;
        case 1U:
            new_freelist = (new_next_page_id > 1U)
                ? new_next_page_id - 1U
                : 0U;
            break;
        case 2U:
            new_freelist = new_next_page_id; /* invalid: >= next_page_id */
            break;
        default:
            new_freelist = reader_u64(r);
            break;
        }

        /*
         * --- Frames: page_id from a small pool; page bytes valid or
         *     random. A validly encoded page with the current txn_id and
         *     a page_id < new_next_page_id is what recover_all needs to
         *     actually apply frames — everything else exercises the
         *     validate-and-reject path. ---
         */
        frames_start = out + offset;
        for (frame_idx = 0U; frame_idx < frame_count; ++frame_idx) {
            uint8_t *frame = frames_start + frame_idx * frame_size;
            uint64_t page_id = 1U + ((uint64_t)reader_u8(r) % 6U);
            uint8_t page_choice;

            sdb_write_u64_le(frame, page_id);
            page_choice = reader_u8(r);
            if ((page_choice & 0x3U) == 0U) {
                /* Valid page for potential apply. */
                (void)sdb_page_encode(
                    frame + SDB_WAL_RECORD_HEADER_SIZE,
                    page_size,
                    (uint16_t)SDB_PAGE_TYPE_DATA,
                    page_id,
                    txn_id,
                    NULL, 0U
                );
            } else {
                reader_fill(
                    r, frame + SDB_WAL_RECORD_HEADER_SIZE, page_size
                );
            }
        }

        /*
         * --- Commit record. Encode with fuzz-selected running_crc /
         *     frame_count so a mismatch flags a torn frame vs a
         *     coincidental commit-magic hit. ---
         */
        running_crc = sdb_crc32(
            frames_start, frame_count * frame_size
        );

        choice = reader_u8(r);
        running_crc_field = ((choice & 0x7U) == 0U)
            ? running_crc ^ 0xDEADBEEFU
            : running_crc;

        choice = reader_u8(r);
        declared_frame_count = (uint32_t)frame_count;
        switch (choice & 0x7U) {
        case 0U:
            declared_frame_count = (uint32_t)frame_count + 1U;
            break;
        case 1U:
            declared_frame_count = (frame_count == 0U)
                ? 0U
                : (uint32_t)frame_count - 1U;
            break;
        case 2U:
            declared_frame_count = 0U;
            break;
        default:
            break;
        }

        rec.txn_id = txn_id;
        rec.frame_count = declared_frame_count;
        rec.next_page_id = new_next_page_id;
        rec.freelist_page = new_freelist;

        commit_ptr = frames_start + frame_count * frame_size;
        sdb_wal_encode_commit_rec(commit_ptr, &rec, running_crc_field);

        /*
         * Optionally flip commit-checksum / magic bytes to drive the
         * decode-status corruption branches.
         */
        choice = reader_u8(r);
        if ((choice & 0x7U) == 0U) {
            commit_ptr[SDB_FUZZ_V3_COMMIT_CHECKSUM_OFFSET] ^= 0x55U;
        }
        if ((choice & 0x70U) == 0U) {
            commit_ptr[0] ^= 0xAAU;
        }

        offset += txn_size;
        running_next_page_id = new_next_page_id;
        expected_next = txn_id + 1U;
    }

    body_end = offset;
    body_len = (body_end > SDB_WAL_HEADER_SIZE)
        ? (body_end - SDB_WAL_HEADER_SIZE)
        : 0U;

    /*
     * Torn-tail simulation: sometimes lop the trailing K bytes from the
     * final txn / commit record to leave the recover_all scanner staring
     * at a short record.
     */
    if (body_len > 0U && (reader_u8(r) & 0x3U) == 0U) {
        cut = (size_t)reader_u8(r)
            | ((size_t)reader_u8(r) << 8);
        cut %= (body_len + 1U);
        if (cut > offset) {
            cut = offset;
        }
        offset -= cut;
    }

    return offset;
}

static void run_structured_v3(fuzz_reader *r)
{
    /* 82.5 KB worst-case: header + 6 * (4 * 4104 + 44). */
    static uint8_t image[
        SDB_WAL_HEADER_SIZE
        + SDB_FUZZ_WAL_MAX_TXNS * (
            SDB_FUZZ_WAL_MAX_FRAMES * (SDB_WAL_RECORD_HEADER_SIZE
                + (size_t)SDB_FUZZ_WAL_PAGE_SIZE)
            + SDB_WAL_COMMIT_SIZE)
    ];
    size_t image_size = build_v3_image(r, image, sizeof(image));
    if (image_size == 0U) {
        return;
    }
    run_recover_all(image, image_size);
}

/* ---------- structured v4 multi-txn image builder -------------------- */

/*
 * Stamp a 64-byte v4 identity header exactly as sdb_wal_append_txn writes it
 * on the first append. When valid_crc is false the sealing CRC is flipped,
 * which drives recover_all's "torn header, probe the self-describing frames"
 * branch (sdb_wal_first_txn_is_v4). A mismatched page_size / file_id with a
 * VALID crc drives the identity gate that returns SDB_E_CORRUPT.
 */
static void build_v4_header(
    uint8_t *out,
    uint64_t first_txn_id,
    uint32_t page_count,
    uint32_t page_size_field,
    const uint8_t *file_id,
    uint64_t next_page_id,
    uint64_t freelist,
    bool valid_crc
)
{
    uint32_t crc;
    (void)memset(out, 0, SDB_WAL_HEADER_SIZE);
    out[0] = (uint8_t)'S';
    out[1] = (uint8_t)'W';
    out[2] = (uint8_t)'A';
    out[3] = (uint8_t)'L';
    sdb_write_u16_le(
        out + SDB_FUZZ_WAL_HDR_VERSION_OFFSET, SDB_WAL_VERSION_V4
    );
    sdb_write_u16_le(
        out + SDB_FUZZ_WAL_HDR_HDRSIZE_OFFSET, (uint16_t)SDB_WAL_HEADER_SIZE
    );
    sdb_write_u64_le(out + SDB_FUZZ_WAL_HDR_TXN_ID_OFFSET, first_txn_id);
    sdb_write_u32_le(out + SDB_FUZZ_WAL_HDR_PAGE_SIZE_OFFSET, page_size_field);
    sdb_write_u32_le(out + SDB_FUZZ_WAL_HDR_PAGE_COUNT_OFFSET, page_count);
    (void)memcpy(
        out + SDB_FUZZ_WAL_HDR_FILE_ID_OFFSET, file_id, SDB_FILE_ID_SIZE
    );
    sdb_write_u64_le(out + SDB_FUZZ_WAL_HDR_NEXT_PAGE_OFFSET, next_page_id);
    sdb_write_u64_le(out + SDB_FUZZ_WAL_HDR_FREELIST_OFFSET, freelist);
    crc = sdb_crc32_zeroed_range(
        out,
        SDB_WAL_HEADER_SIZE,
        SDB_FUZZ_WAL_HDR_CHECKSUM_OFFSET,
        sizeof(uint32_t)
    );
    if (!valid_crc) {
        crc ^= 0xA5A5A5A5U;
    }
    sdb_write_u32_le(out + SDB_FUZZ_WAL_HDR_CHECKSUM_OFFSET, crc);
}

/*
 * Build a v4 self-describing multi-txn WAL image driven by the fuzz input.
 * Every field the deterministic recover_all branch consults has an
 * independent corrupt/valid switch: the per-frame v4 meta (txn_id,
 * frame_index, frame_count), the frame_count that fixes the commit-record
 * offset, the commit running-CRC / declared frame_count / magic / checksum,
 * plus the 64-byte identity header (valid, corrupt-CRC, or identity
 * mismatch). The tail can be torn at an arbitrary offset to exercise the
 * short-commit and cut-mid-frame paths.
 */
static size_t build_v4_image(fuzz_reader *r, uint8_t *out, size_t cap)
{
    const size_t page_size = (size_t)SDB_FUZZ_WAL_PAGE_SIZE;
    const size_t frame_size = SDB_WAL_RECORD_HEADER_SIZE_V4 + page_size;
    size_t offset;
    size_t txn_count;
    size_t txn_index;
    uint64_t expected_next;
    uint64_t running_next_page_id;
    uint64_t first_txn_id = 1U;
    uint32_t first_page_count = 1U;
    bool captured_first = false;
    size_t body_len;
    size_t cut;
    uint8_t hdr_choice;
    bool hdr_valid_crc = true;
    uint32_t hdr_page_size = (uint32_t)SDB_FUZZ_WAL_PAGE_SIZE;
    uint8_t hdr_file_id[SDB_FILE_ID_SIZE];

    if (cap < SDB_WAL_HEADER_SIZE + frame_size + SDB_WAL_COMMIT_SIZE) {
        return 0U;
    }

    (void)memcpy(hdr_file_id, g_sb.file_id, SDB_FILE_ID_SIZE);

    /*
     * Identity-header variant: valid / torn-CRC (probe) / page_size mismatch
     * / file_id mismatch.
     */
    hdr_choice = reader_u8(r);
    switch (hdr_choice & 0x7U) {
    case 0U:
        hdr_valid_crc = false; /* (c) probe-v4-on-corrupt-header */
        break;
    case 1U:
        hdr_page_size = (uint32_t)SDB_FUZZ_WAL_PAGE_SIZE << 1U; /* (d) gate */
        break;
    case 2U:
        hdr_file_id[0] ^= 0xFFU; /* (d) identity gate: foreign file_id */
        break;
    default:
        break; /* valid v4 identity header */
    }

    offset = SDB_WAL_HEADER_SIZE;
    txn_count = 1U + ((size_t)reader_u8(r) % SDB_FUZZ_WAL_MAX_TXNS);
    expected_next = g_sb.checkpoint_lsn + 1U;
    running_next_page_id = g_sb.next_page_id;

    for (txn_index = 0U; txn_index < txn_count; ++txn_index) {
        size_t frame_count;
        size_t txn_size;
        uint8_t *frames_start;
        uint8_t *commit_ptr;
        size_t frame_idx;
        uint32_t running_crc;
        uint32_t running_crc_field;
        sdb_wal_commit_rec rec;
        uint64_t txn_id;
        uint64_t new_next_page_id;
        uint64_t new_freelist;
        uint32_t declared_frame_count;
        uint8_t choice;

        frame_count = 1U + ((size_t)reader_u8(r) % SDB_FUZZ_WAL_MAX_FRAMES);
        txn_size = frame_count * frame_size + SDB_WAL_COMMIT_SIZE;
        if (offset + txn_size > cap) {
            break;
        }

        /* --- txn_id: monotone / gap / off-by-one / random. --- */
        choice = reader_u8(r);
        switch (choice & 0x3U) {
        case 0U:
            txn_id = expected_next;
            break;
        case 1U:
            txn_id = expected_next + 1U;
            break;
        case 2U:
            txn_id = (expected_next > 1U) ? expected_next - 1U : expected_next;
            break;
        default:
            txn_id = (uint64_t)reader_u8(r) + 1U;
            break;
        }

        /* --- next_page_id: grow / same / shrink / random small. --- */
        choice = reader_u8(r);
        switch (choice & 0x3U) {
        case 0U:
            new_next_page_id =
                running_next_page_id + ((uint64_t)reader_u8(r) % 8U) + 1U;
            break;
        case 1U:
            new_next_page_id = running_next_page_id;
            break;
        case 2U:
            new_next_page_id = (running_next_page_id > 1U)
                ? running_next_page_id - 1U
                : running_next_page_id;
            break;
        default:
            new_next_page_id = (uint64_t)reader_u8(r);
            break;
        }
        if (new_next_page_id > SDB_FUZZ_WAL_PAGE_ID_CEIL) {
            new_next_page_id = SDB_FUZZ_WAL_PAGE_ID_CEIL;
        }
        if (new_next_page_id < 2U) {
            new_next_page_id = 2U;
        }

        /* --- freelist: 0 / in-range / out-of-range / random. --- */
        choice = reader_u8(r);
        switch (choice & 0x3U) {
        case 0U:
            new_freelist = 0U;
            break;
        case 1U:
            new_freelist = (new_next_page_id > 1U) ? new_next_page_id - 1U : 0U;
            break;
        case 2U:
            new_freelist = new_next_page_id;
            break;
        default:
            new_freelist = reader_u64(r);
            break;
        }

        /*
         * --- Frames: 24-byte self-describing header + page body. A valid
         *     page (page_id < new_next_page_id, page_lsn == txn_id) lets the
         *     deterministic branch apply; otherwise validate rejects. ---
         */
        frames_start = out + offset;
        for (frame_idx = 0U; frame_idx < frame_count; ++frame_idx) {
            uint8_t *frame = frames_start + frame_idx * frame_size;
            uint64_t page_id = 1U + ((uint64_t)reader_u8(r) % 6U);
            uint8_t page_choice;

            sdb_write_u64_le(frame, page_id);
            sdb_write_u64_le(
                frame + SDB_WAL_FRAME_V4_TXN_ID_OFFSET, txn_id
            );
            sdb_write_u32_le(
                frame + SDB_WAL_FRAME_V4_FRAME_INDEX_OFFSET,
                (uint32_t)frame_idx
            );
            sdb_write_u32_le(
                frame + SDB_WAL_FRAME_V4_FRAME_COUNT_OFFSET,
                (uint32_t)frame_count
            );
            page_choice = reader_u8(r);
            if ((page_choice & 0x3U) == 0U) {
                (void)sdb_page_encode(
                    frame + SDB_WAL_RECORD_HEADER_SIZE_V4,
                    page_size,
                    (uint16_t)SDB_PAGE_TYPE_DATA,
                    page_id,
                    txn_id,
                    NULL, 0U
                );
            } else {
                reader_fill(
                    r, frame + SDB_WAL_RECORD_HEADER_SIZE_V4, page_size
                );
            }
        }

        /*
         * --- v4 frame-meta corruption (applied BEFORE the running-CRC so a
         *     torn frame either fails the frame_count-bound / declared-count
         *     match at the boundary or reaches validate_frames and is
         *     rejected as SDB_E_CORRUPT). ---
         */
        choice = reader_u8(r);
        switch (choice & 0x7U) {
        case 0U:
            /*
             * First frame's frame_index != 0: deterministic branch reads a
             * bad frame_count / validate rejects on index mismatch.
             */
            sdb_write_u32_le(
                frames_start + SDB_WAL_FRAME_V4_FRAME_INDEX_OFFSET, 7U
            );
            break;
        case 1U:
            /*
             * First frame's embedded frame_count huge: declared_count
             * overflows the (remaining/frame_size) bound -> torn.
             */
            sdb_write_u32_le(
                frames_start + SDB_WAL_FRAME_V4_FRAME_COUNT_OFFSET,
                0xFFFFFFFFU
            );
            break;
        case 2U:
            /* First frame's embedded frame_count zero -> torn. */
            sdb_write_u32_le(
                frames_start + SDB_WAL_FRAME_V4_FRAME_COUNT_OFFSET, 0U
            );
            break;
        case 3U:
            /* A middle/last frame names the wrong txn_id: validate rejects. */
            sdb_write_u64_le(
                frames_start + (frame_count - 1U) * frame_size
                    + SDB_WAL_FRAME_V4_TXN_ID_OFFSET,
                txn_id ^ 0xFFU
            );
            break;
        default:
            break; /* leave the self-describing meta intact */
        }

        /*
         * --- Commit record: fuzz-selected running-CRC and declared
         *     frame_count so a mismatch flags a torn frame. ---
         */
        running_crc = sdb_crc32(frames_start, frame_count * frame_size);
        choice = reader_u8(r);
        running_crc_field = ((choice & 0x7U) == 0U)
            ? running_crc ^ 0xDEADBEEFU
            : running_crc;

        choice = reader_u8(r);
        declared_frame_count = (uint32_t)frame_count;
        switch (choice & 0x7U) {
        case 0U:
            declared_frame_count = (uint32_t)frame_count + 1U;
            break;
        case 1U:
            declared_frame_count = (uint32_t)frame_count - 1U;
            break;
        case 2U:
            declared_frame_count = 0U;
            break;
        default:
            break;
        }

        rec.txn_id = txn_id;
        rec.frame_count = declared_frame_count;
        rec.next_page_id = new_next_page_id;
        rec.freelist_page = new_freelist;

        commit_ptr = frames_start + frame_count * frame_size;
        sdb_wal_encode_commit_rec(commit_ptr, &rec, running_crc_field);

        choice = reader_u8(r);
        if ((choice & 0x7U) == 0U) {
            commit_ptr[SDB_FUZZ_V3_COMMIT_CHECKSUM_OFFSET] ^= 0x55U;
        }
        if ((choice & 0x70U) == 0U) {
            commit_ptr[0] ^= 0xAAU;
        }

        if (!captured_first) {
            first_txn_id = txn_id;
            first_page_count = (uint32_t)frame_count;
            captured_first = true;
        }

        offset += txn_size;
        running_next_page_id = new_next_page_id;
        expected_next = txn_id + 1U;
    }

    body_len = (offset > SDB_WAL_HEADER_SIZE)
        ? (offset - SDB_WAL_HEADER_SIZE)
        : 0U;

    /*
     * Torn-tail simulation: lop trailing K bytes off the final txn so the
     * v4 boundary lands on a short commit-record or a cut-mid-frame.
     */
    if (body_len > 0U && (reader_u8(r) & 0x3U) == 0U) {
        cut = (size_t)reader_u8(r) | ((size_t)reader_u8(r) << 8);
        cut %= (body_len + 1U);
        offset -= cut;
    }

    /*
     * Header identifies the first txn and seals the identity fields. Written
     * last because it names the first txn's id / page_count.
     */
    build_v4_header(
        out,
        first_txn_id,
        first_page_count,
        hdr_page_size,
        hdr_file_id,
        g_sb.next_page_id,
        0U,
        hdr_valid_crc
    );

    return offset;
}

static void run_structured_v4(fuzz_reader *r)
{
    /* ~97 KB worst-case: header + 6 * (4 * 4120 + 44). */
    static uint8_t image[
        SDB_WAL_HEADER_SIZE
        + SDB_FUZZ_WAL_MAX_TXNS * (
            SDB_FUZZ_WAL_MAX_FRAMES * (SDB_WAL_RECORD_HEADER_SIZE_V4
                + (size_t)SDB_FUZZ_WAL_PAGE_SIZE)
            + SDB_WAL_COMMIT_SIZE)
    ];
    size_t image_size = build_v4_image(r, image, sizeof(image));
    if (image_size == 0U) {
        return;
    }
    run_recover_all(image, image_size);
}

/* ---------- libFuzzer entry point ------------------------------------- */

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    fuzz_reader reader;

    if (size > SDB_FUZZ_WAL_MAX_INPUT) {
        return 0;
    }
    if (ensure_initialized() != 0) {
        return 0;
    }

    /*
     * Path A: raw fuzz bytes → single-txn v1/v2 recovery.
     * Preserves the coverage of the original harness so old crashes stay
     * reachable and the corpus keeps meaning.
     */
    run_recover_single(data, size);

    /*
     * Path B: raw fuzz bytes → multi-txn v3 recovery. The v3 scanner is
     * intentionally lenient about the reserved 64-byte header slot, so
     * uninterpreted input directly exercises its torn-tail /
     * corrupt-frame / commit-CRC branches.
     */
    run_recover_all(data, size);

    /*
     * Path C: build a structured v3 multi-txn image from the same fuzz
     * bytes and hand that to recover_all. Guarantees non-trivial
     * coverage of the accept path (contiguous txn_ids, monotone
     * next_page_id, matching running/commit CRCs) while still letting
     * every field flip into an invalid state.
     */
    reader.buf = data;
    reader.len = size;
    reader.pos = 0U;
    run_structured_v3(&reader);

    /*
     * Path D: build a structured v4 self-describing multi-txn image and hand
     * it to recover_all. Guarantees coverage of the deterministic v4 boundary
     * (frame_count-driven commit offset), the identity gate (valid CRC with a
     * foreign page_size / file_id), the probe-v4-on-corrupt-header branch
     * (torn header sector but intact self-describing frames), and every torn
     * variant (bad frame_index / embedded frame_count, cut tail). A fresh
     * reader starting at pos 0 lets libFuzzer drive both builders from the
     * same corpus while their differing consume shapes keep them divergent.
     */
    reader.buf = data;
    reader.len = size;
    reader.pos = 0U;
    run_structured_v4(&reader);

    return 0;
}
