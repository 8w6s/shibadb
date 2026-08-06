#include "pager.h"
#include "wal.h"
#include "page.h"
#include "file.h"
#include "shibadb.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static const char *test_path = "test-wal-recover-forge.tmp";
static const char *wal_path = "test-wal-recover-forge.tmp.wal";
static const uint8_t password[] = "correct horse battery staple";
static const uint8_t original_payload[] = "ORIGINAL-DATA-16";
static const uint8_t forged_payload[] = "HACKED-BY-ATTKR!";
static const uint8_t updated_payload[] = "UPDATED-LEGIT-16";

static void cleanup(void)
{
    (void)remove(test_path);
    (void)remove(wal_path);
}

/*
 * BUG#1: a keyless attacker who can write the .wal sidecar forges a txn whose
 * frame is a plaintext DATA page (outer type != ENCRYPTED). Recovery only
 * AEAD-verifies frames whose outer type IS ENCRYPTED; a DATA/FREE frame slips
 * past the range/unique/CRC32/page_lsn gates (all keyless/public) and is
 * applied verbatim to the data file. The read path then rejects the resulting
 * non-ENCRYPTED page (pager.c: encrypted DB must see type ENCRYPTED) -> the
 * committed page is permanently unreadable: data loss / DoS without the key.
 *
 * The fix mirrors the read-path guard at the recovery/parse boundary: for an
 * encrypted DB (data_key != NULL) recovery must REJECT any frame whose outer
 * type is not ENCRYPTED, before applying it.
 */
static void test_recovery_rejects_forged_nonencrypted_frame(void)
{
    sdb_pager pager;
    uint8_t salt[SDB_SALT_SIZE];
    uint8_t file_id[SDB_FILE_ID_SIZE];
    uint64_t p1;
    sdb_superblock_v1 snap;
    sdb_status open_status;

    memset(salt, 0x33, sizeof(salt));
    memset(file_id, 0x44, sizeof(file_id));
    cleanup();

    assert(sdb_pager_create_encrypted(
        test_path, 4096U, salt, file_id,
        password, sizeof(password) - 1U,
        SDB_MIN_KDF_ITERATIONS, &pager
    ) == SDB_OK);
    assert(pager.encryption_enabled);
    assert(sdb_pager_allocate(&pager, &p1) == SDB_OK);
    assert(sdb_pager_write(
        &pager, p1, (uint16_t)SDB_PAGE_TYPE_DATA, 0U,
        original_payload, sizeof(original_payload)
    ) == SDB_OK);
    /*
     * The attacker only reads PUBLIC superblock fields (file_id, page_size,
     * next_page_id, freelist_page, checkpoint_lsn). It NEVER touches data_key.
     * A snapshot of the in-memory superblock stands in for what the attacker
     * would read straight off disk.
     */
    snap = pager.superblock;
    assert(sdb_pager_close(&pager) == SDB_OK);

    /* --- forge a v5 WAL with a single plaintext DATA frame (no key used) --- */
    {
        sdb_superblock_v1 forged_sb;
        uint8_t forged_page[4096];
        sdb_wal_page wp;
        sdb_file wal;
        uint64_t new_off = 0U;
        const uint64_t txn_id = snap.checkpoint_lsn + 1U;

        memset(&forged_sb, 0, sizeof(forged_sb));
        forged_sb.page_size = snap.page_size;
        forged_sb.next_page_id = snap.next_page_id;
        forged_sb.freelist_page = snap.freelist_page;
        memcpy(forged_sb.file_id, snap.file_id, SDB_FILE_ID_SIZE);

        /* Keyless: sdb_page_encode stamps a public CRC32, no AEAD. */
        assert(sdb_page_encode(
            forged_page, (size_t)snap.page_size,
            (uint16_t)SDB_PAGE_TYPE_DATA, p1, txn_id,
            forged_payload, sizeof(forged_payload)
        ) == SDB_OK);
        wp.page_id = p1;
        wp.bytes = forged_page;

        (void)remove(wal_path);
        assert(sdb_file_create_new(wal_path, &wal) == SDB_OK);
        /*
         * append_offset == SDB_WAL_HEADER_SIZE stamps the identity header and
         * writes header+frame+commit as one valid v5 WAL. All keyless.
         */
        assert(sdb_wal_append_txn(
            &wal, SDB_WAL_HEADER_SIZE, &forged_sb, txn_id, &wp, 1U, &new_off
        ) == SDB_OK);
        assert(sdb_file_close(&wal) == SDB_OK);
    }

    /* --- reopen: recovery must reject the forged frame --- */
    open_status = sdb_pager_open_encrypted(
        test_path, password, sizeof(password) - 1U, &pager
    );
    (void)fprintf(
        stderr,
        "[forge] reopen after forged DATA frame: status=%d (%s)\n",
        (int)open_status, sdb_status_string(open_status)
    );
    if (open_status == SDB_OK) {
        /*
         * PRE-FIX behaviour: recovery applied the forged frame. Demonstrate the
         * damage before failing: the once-ENCRYPTED committed page now reads
         * back as a non-ENCRYPTED page and is unreadable.
         */
        uint8_t page[4096];
        sdb_page_view view;
        sdb_status read_status = sdb_pager_read(
            &pager, p1, page, sizeof(page), &view
        );
        (void)fprintf(
            stderr,
            "[forge] forged frame WAS applied; read p1 status=%d (%s)\n",
            (int)read_status, sdb_status_string(read_status)
        );
        (void)sdb_pager_close(&pager);
    }
    /*
     * Correct (post-fix) behaviour: recovery refuses to apply the forged
     * frame and surfaces corruption.
     */
    assert(open_status == SDB_E_CORRUPT);

    /* --- the original committed page must be intact --- */
    (void)remove(wal_path);
    assert(sdb_pager_open_encrypted(
        test_path, password, sizeof(password) - 1U, &pager
    ) == SDB_OK);
    {
        uint8_t page[4096];
        sdb_page_view view;
        assert(sdb_pager_read(
            &pager, p1, page, sizeof(page), &view
        ) == SDB_OK);
        assert(view.payload_size == sizeof(original_payload));
        assert(memcmp(
            view.payload, original_payload, sizeof(original_payload)
        ) == 0);
    }
    assert(sdb_pager_close(&pager) == SDB_OK);
    cleanup();
    (void)puts("wal recovery: forged non-ENCRYPTED frame rejected ok");
}

/*
 * Positive control: a genuine ENCRYPTED frame produced by the engine's own
 * WAL-mode commit must still recover and apply normally. Guards against a fix
 * that over-rejects and breaks legitimate encrypted recovery.
 */
static void test_recovery_applies_valid_encrypted_frame(void)
{
    sdb_pager pager;
    uint8_t salt[SDB_SALT_SIZE];
    uint8_t file_id[SDB_FILE_ID_SIZE];
    uint64_t p1;

    memset(salt, 0x55, sizeof(salt));
    memset(file_id, 0x66, sizeof(file_id));
    cleanup();

    assert(sdb_pager_create_encrypted(
        test_path, 4096U, salt, file_id,
        password, sizeof(password) - 1U,
        SDB_MIN_KDF_ITERATIONS, &pager
    ) == SDB_OK);
    assert(sdb_pager_allocate(&pager, &p1) == SDB_OK);
    assert(sdb_pager_write(
        &pager, p1, (uint16_t)SDB_PAGE_TYPE_DATA, 0U,
        original_payload, sizeof(original_payload)
    ) == SDB_OK);
    /*
     * WAL-mode commit: encodes an ENCRYPTED frame, left in the WAL (close does
     * not checkpoint).
     */
    {
        sdb_txn txn;
        assert(sdb_txn_begin(&pager, &txn) == SDB_OK);
        assert(sdb_txn_put(
            &txn, p1, (uint16_t)SDB_PAGE_TYPE_DATA,
            updated_payload, sizeof(updated_payload)
        ) == SDB_OK);
        assert(sdb_txn_commit(&txn) == SDB_OK);
    }
    assert(sdb_pager_close(&pager) == SDB_OK);

    assert(sdb_pager_open_encrypted(
        test_path, password, sizeof(password) - 1U, &pager
    ) == SDB_OK);
    {
        uint8_t page[4096];
        sdb_page_view view;
        assert(sdb_pager_read(
            &pager, p1, page, sizeof(page), &view
        ) == SDB_OK);
        assert(view.payload_size == sizeof(updated_payload));
        assert(memcmp(
            view.payload, updated_payload, sizeof(updated_payload)
        ) == 0);
    }
    assert(sdb_pager_close(&pager) == SDB_OK);
    cleanup();
    (void)puts("wal recovery: valid ENCRYPTED frame applied ok");
}

int main(void)
{
    test_recovery_rejects_forged_nonencrypted_frame();
    test_recovery_applies_valid_encrypted_frame();
    (void)puts("all wal_recover_forge tests passed");
    return 0;
}
