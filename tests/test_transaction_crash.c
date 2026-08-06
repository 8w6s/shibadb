#include "pager.h"
#include "wal.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static const char *test_path = "test-transaction-crash.tmp";
static const char *wal_path = "test-transaction-crash.tmp.wal";
static const uint8_t old_value[] = "old";
static const uint8_t new_value[] = "new";

static void prepare(
    sdb_pager *pager, uint64_t *first_out, uint64_t *second_out
)
{
    uint8_t salt[16] = {7U};
    uint8_t file_id[16] = {8U};
    (void)remove(test_path);
    (void)remove(wal_path);
    assert(sdb_pager_create(
        test_path, 4096U, salt, file_id, pager
    ) == SDB_OK);
    assert(sdb_pager_allocate(pager, first_out) == SDB_OK);
    assert(sdb_pager_allocate(pager, second_out) == SDB_OK);
    assert(sdb_pager_write(
        pager, *first_out, (uint16_t)SDB_PAGE_TYPE_DATA, 0U,
        old_value, sizeof(old_value)
    ) == SDB_OK);
    assert(sdb_pager_write(
        pager, *second_out, (uint16_t)SDB_PAGE_TYPE_DATA, 0U,
        old_value, sizeof(old_value)
    ) == SDB_OK);
}

static bool is_new(sdb_pager *pager, uint64_t page_id)
{
    uint8_t page[4096];
    sdb_page_view view;
    assert(sdb_pager_read(
        pager, page_id, page, sizeof(page), &view
    ) == SDB_OK);
    assert(view.payload_size == sizeof(old_value));
    if (memcmp(view.payload, new_value, sizeof(new_value)) == 0) {
        return true;
    }
    assert(memcmp(view.payload, old_value, sizeof(old_value)) == 0);
    return false;
}

static sdb_status stage_and_commit(
    sdb_pager *pager, uint64_t first, uint64_t second
)
{
    sdb_txn txn;
    uint64_t reserved;
    assert(sdb_txn_begin(pager, &txn) == SDB_OK);
    assert(sdb_txn_allocate(&txn, &reserved) == SDB_OK);
    assert(reserved == pager->superblock.next_page_id);
    assert(sdb_txn_put(
        &txn, first, (uint16_t)SDB_PAGE_TYPE_DATA,
        new_value, sizeof(new_value)
    ) == SDB_OK);
    assert(sdb_txn_put(
        &txn, second, (uint16_t)SDB_PAGE_TYPE_DATA,
        new_value, sizeof(new_value)
    ) == SDB_OK);
    assert(sdb_txn_put(
        &txn, reserved, (uint16_t)SDB_PAGE_TYPE_DATA,
        new_value, sizeof(new_value)
    ) == SDB_OK);
    return sdb_txn_commit(&txn);
}

static void verify_atomic_reopen(uint64_t first, uint64_t second)
{
    sdb_pager pager;
    bool first_new;
    bool second_new;
    const uint64_t reserved = second + 1U;
    assert(sdb_pager_open(test_path, &pager) == SDB_OK);
    first_new = is_new(&pager, first);
    second_new = is_new(&pager, second);
    assert(first_new == second_new);
    if (first_new) {
        assert(pager.superblock.next_page_id == reserved + 1U);
        assert(is_new(&pager, reserved));
        /*
         * Durable outcome: the single staged txn (txn_id 1, the only WAL-mode
         * commit — prepare's writes go through the direct-write path and never
         * bump the LSN) is the recovered checkpoint. Both the durable
         * superblock LSN and the reopened in-memory LSN must be exactly 1.
         */
        assert(pager.superblock.checkpoint_lsn == 1U);
        assert(pager.current_lsn == 1U);
    } else {
        assert(pager.superblock.next_page_id == reserved);
        /*
         * Torn/dropped outcome: the txn was dropped WHOLE, so the durable
         * prefix (nothing but the pre-txn state) is kept and the LSN never
         * advanced past 0. A partial-replay bug that advanced the LSN while
         * dropping the data (or vice versa) trips here.
         */
        assert(pager.superblock.checkpoint_lsn == 0U);
        assert(pager.current_lsn == 0U);
    }
    assert(sdb_pager_close(&pager) == SDB_OK);
}

/*
 * Strict-drop verifier for the WAL write-fault boundary (op 0). A faulted
 * write never issues its pwrite, so ZERO bytes reach the WAL: the txn cannot
 * be durable-anyway (unlike the fsync boundary, where the OS page cache may
 * retain the bytes). We therefore demand the exact dropped state rather than
 * the both-or-neither of verify_atomic_reopen: both pre-txn pages keep their
 * OLD value, the reserved page was never allocated, and the LSN is untouched.
 */
static void verify_dropped_reopen(uint64_t first, uint64_t second)
{
    sdb_pager pager;
    const uint64_t reserved = second + 1U;
    assert(sdb_pager_open(test_path, &pager) == SDB_OK);
    /* is_new asserts the payload is old-or-new and returns true only for new. */
    assert(!is_new(&pager, first));
    assert(!is_new(&pager, second));
    assert(pager.superblock.next_page_id == reserved);
    assert(pager.superblock.checkpoint_lsn == 0U);
    assert(pager.current_lsn == 0U);
    assert(sdb_pager_close(&pager) == SDB_OK);
}

/*
 * Existing single-txn boundary matrices. Kept as-is so a regression in the
 * append_txn single-fsync path (R2a) or in the pager->file crash path still
 * trips the assert. The multi-txn matrix below layers ON TOP of them: any
 * commit boundary that keeps atomicity for one txn but silently drops a PRIOR
 * durable txn would only show up in the multi-txn scenarios.
 */
/*
 * R2a collapses the WAL append to exactly TWO injectable I/O ops: op 0 is the
 * single [header?][frames][commit-record] write (one pwrite — io_limit is the
 * SIZE_MAX default), op 1 is the single fsync. A boundary of 2+ arms nothing
 * (the append already returned), so the loop is bounded at 2 to keep every
 * iteration a REAL fault, and faults_fired confirms both ops actually faulted
 * (a regression that adds/removes/relocates an op trips the per-iteration
 * commit-must-fail assert or the faults_fired total).
 */
#define SDB_WAL_COMMIT_IO_OPS ((size_t)2)

static void test_legacy_single_txn_wal_boundaries(void)
{
    size_t boundary;
    size_t faults_fired = 0U;
    for (boundary = 0U; boundary < SDB_WAL_COMMIT_IO_OPS; ++boundary) {
        sdb_pager pager;
        uint64_t first;
        uint64_t second;
        sdb_status commit_status;
        prepare(&pager, &first, &second);
        sdb_wal_fail_after_for_testing(boundary);
        commit_status = stage_and_commit(&pager, first, second);
        sdb_wal_clear_failure_for_testing();
        /* Both reachable ops are genuine faults ⇒ commit must report failure. */
        assert(commit_status != SDB_OK);
        ++faults_fired;
        assert(sdb_pager_close(&pager) == SDB_OK);
        if (boundary == 0U) {
            /* Write fault: pwrite never ran, so the txn is dropped WHOLE. */
            verify_dropped_reopen(first, second);
        } else {
            /* fsync fault: durable-anyway (page cache) OR dropped — atomic. */
            verify_atomic_reopen(first, second);
        }
    }
    assert(faults_fired == SDB_WAL_COMMIT_IO_OPS);
    (void)puts("transaction crash: legacy single-txn WAL boundaries ok");
}

/*
 * The WAL-mode commit touches the DATA file at exactly TWO injectable ops, and
 * both fire only AFTER the WAL append is already durable on its own fd (never
 * faulted here): op 0 is sdb_file_size, op 1 is sdb_file_resize (grow to fit
 * the txn's new next_page_id). Because the WAL holds the committed txn
 * regardless of which data-file op faults, reopen always replays it
 * (durable-anyway), so every iteration lands the NEW branch. Boundaries 2+
 * arm nothing, so the loop is bounded at 2.
 */
#define SDB_FILE_COMMIT_IO_OPS ((size_t)2)

static void test_legacy_single_txn_file_boundaries(void)
{
    size_t boundary;
    size_t faults_fired = 0U;
    for (boundary = 0U; boundary < SDB_FILE_COMMIT_IO_OPS; ++boundary) {
        sdb_pager pager;
        uint64_t first;
        uint64_t second;
        sdb_status commit_status;
        prepare(&pager, &first, &second);
        sdb_file_fail_after_for_testing(&pager.file, boundary);
        commit_status = stage_and_commit(&pager, first, second);
        sdb_file_clear_failure_for_testing(&pager.file);
        /* Both reachable data-file ops are genuine faults ⇒ commit fails. */
        assert(commit_status != SDB_OK);
        ++faults_fired;
        assert(sdb_pager_close(&pager) == SDB_OK);
        verify_atomic_reopen(first, second);
    }
    assert(faults_fired == SDB_FILE_COMMIT_IO_OPS);
    (void)puts("transaction crash: legacy single-txn file boundaries ok");
}

/* ---------------- multi-txn crash matrix ---------------- */

/*
 * Distinct per-txn payloads for the multi-txn matrix. Each is exactly the
 * same size (16 bytes including the trailing NUL for readability) so is_v*
 * checks below can rely on view.payload_size and a memcmp.
 */
static const uint8_t v_txn1_p1[] = "TXN1-P1-VALUE!!";
static const uint8_t v_txn2_p2[] = "TXN2-P2-VALUE!!";
static const uint8_t v_txn3_p3[] = "TXN3-P3-VALUE!!";
static const uint8_t v_txn4_p1[] = "TXN4-P1-VALUE!!";

/*
 * Fresh pager with three pre-allocated pages. Each allocate goes through the
 * direct-write path (data file + superblock update), leaving the WAL empty
 * so the multi-txn matrix starts from a clean slate. Callers subsequently
 * commit txns via the WAL-mode path; those commits stay in the WAL until
 * either open recovery or an explicit checkpoint drains them.
 */
static void prepare_three_pages(
    sdb_pager *pager, uint64_t *p1, uint64_t *p2, uint64_t *p3
)
{
    uint8_t salt[16] = {0x11U};
    uint8_t file_id[16] = {0x22U};
    (void)remove(test_path);
    (void)remove(wal_path);
    assert(
        sdb_pager_create(test_path, 4096U, salt, file_id, pager) == SDB_OK
    );
    assert(sdb_pager_allocate(pager, p1) == SDB_OK);
    assert(sdb_pager_allocate(pager, p2) == SDB_OK);
    assert(sdb_pager_allocate(pager, p3) == SDB_OK);
}

/*
 * Commit `value` to `page_id` inside its own single-page WAL-mode txn. Used
 * to lay down durable txns before the injected crash. Aborts on failure so
 * the matrix's "prior txns durable" precondition is never quietly untrue.
 */
static void commit_single_page_txn(
    sdb_pager *pager,
    uint64_t page_id,
    const uint8_t *value,
    size_t value_size
)
{
    sdb_txn txn;
    assert(sdb_txn_begin(pager, &txn) == SDB_OK);
    assert(sdb_txn_put(
        &txn, page_id, (uint16_t)SDB_PAGE_TYPE_DATA, value, value_size
    ) == SDB_OK);
    assert(sdb_txn_commit(&txn) == SDB_OK);
}

/*
 * Read the current bytes at `page_id` and decide "does it match `value`?".
 * `payload_size==0` means the page still holds its fresh-allocated state
 * (sdb_pager_write with NULL payload) — i.e. no txn has touched it yet.
 */
static bool page_has_value(
    sdb_pager *pager,
    uint64_t page_id,
    const uint8_t *value,
    size_t value_size
)
{
    uint8_t page[4096];
    sdb_page_view view;
    assert(
        sdb_pager_read(pager, page_id, page, sizeof(page), &view) == SDB_OK
    );
    if (view.payload_size != value_size) {
        return false;
    }
    return memcmp(view.payload, value, value_size) == 0;
}

static bool page_is_empty(sdb_pager *pager, uint64_t page_id)
{
    uint8_t page[4096];
    sdb_page_view view;
    assert(
        sdb_pager_read(pager, page_id, page, sizeof(page), &view) == SDB_OK
    );
    return view.payload_size == 0U;
}

/*
 * (1) Multi-txn WAL commit-boundary matrix.
 *
 * Precondition per iteration: fresh pager with three allocated pages; two
 * successful WAL-mode txns (txn1 writes p1, txn2 writes p2) are already
 * durable on disk (one fsync each under R2a). We then arm a per-boundary
 * failure
 * and attempt txn3 — a MULTI-PAGE txn that both rewrites p1 (v_txn4_p1)
 * AND writes p3 (v_txn3_p3). Post-reopen we require:
 *
 *   - p2 == v_txn2_p2 (ALWAYS — prior durable txn, untouched by txn3).
 *   - Atomicity of txn3: EITHER both p1 and p3 hold their new values,
 *     OR p1 keeps v_txn1_p1 AND p3 is fresh-allocated (payload_size=0).
 *     A "half torn" outcome (one of the two updated, the other not) is
 *     the exact durability bug this matrix hunts for.
 *   - next_page_id unchanged: txn3 does not allocate.
 *
 * We deliberately do NOT couple the atomicity check to sdb_txn_commit's
 * return value. The lone fsync (boundary 1) can leave the frames + commit-rec
 * physically on disk via the OS page cache even though the fsync errored — a
 * soft crash injection cannot simulate a hard power loss. The "durable-
 * anyway" outcome is legitimate; only atomicity is the invariant.
 *
 * Boundaries fired via sdb_wal_fail_after_for_testing (R2a collapses the two
 * commit fsyncs into one contiguous write + one fsync, so only two I/O ops
 * remain per append):
 *   0 — the single WAL write ([header?] frames + commit-record) fails
 *   1 — the single fsync fails
 *   2..5 — no injection reachable; txn3 fully durable
 *
 * A lost txn1/txn2 → page_has_value(p2) trips. A "half torn" txn3 →
 * the XOR trips. A partial-replay bug that drops txn3's frames while
 * keeping the commit-rec would also show up as "half torn" here.
 */
static void test_multi_txn_wal_commit_boundaries(void)
{
    size_t boundary;
    for (boundary = 0U; boundary < 6U; ++boundary) {
        sdb_pager pager;
        uint64_t p1;
        uint64_t p2;
        uint64_t p3;
        sdb_txn txn3;
        bool p1_updated;
        bool p3_updated;
        prepare_three_pages(&pager, &p1, &p2, &p3);

        commit_single_page_txn(&pager, p1, v_txn1_p1, sizeof(v_txn1_p1));
        commit_single_page_txn(&pager, p2, v_txn2_p2, sizeof(v_txn2_p2));
        /* txn1 and txn2 are durable in the WAL now (one fsync each, R2a). */

        sdb_wal_fail_after_for_testing(boundary);
        assert(sdb_txn_begin(&pager, &txn3) == SDB_OK);
        /* Multi-page txn: BOTH writes must land or NEITHER must. */
        assert(sdb_txn_put(
            &txn3, p1, (uint16_t)SDB_PAGE_TYPE_DATA,
            v_txn4_p1, sizeof(v_txn4_p1)
        ) == SDB_OK);
        assert(sdb_txn_put(
            &txn3, p3, (uint16_t)SDB_PAGE_TYPE_DATA,
            v_txn3_p3, sizeof(v_txn3_p3)
        ) == SDB_OK);
        (void)sdb_txn_commit(&txn3);
        sdb_wal_clear_failure_for_testing();
        assert(sdb_pager_close(&pager) == SDB_OK);

        assert(sdb_pager_open(test_path, &pager) == SDB_OK);
        /* Prior durable txn on p2 must survive unconditionally. */
        assert(page_has_value(&pager, p2, v_txn2_p2, sizeof(v_txn2_p2)));
        p1_updated = page_has_value(
            &pager, p1, v_txn4_p1, sizeof(v_txn4_p1)
        );
        p3_updated = page_has_value(
            &pager, p3, v_txn3_p3, sizeof(v_txn3_p3)
        );
        /* Atomicity: both-or-neither for txn3's two-page write set. */
        assert(p1_updated == p3_updated);
        if (!p1_updated) {
            /* Torn: prior state on p1 stays, p3 stays fresh-allocated. */
            assert(page_has_value(
                &pager, p1, v_txn1_p1, sizeof(v_txn1_p1)
            ));
            assert(page_is_empty(&pager, p3));
        }
        /* No allocations happened; next_page_id must not have shifted. */
        assert(pager.superblock.next_page_id == p3 + 1U);
        assert(sdb_pager_close(&pager) == SDB_OK);
    }
    (void)remove(wal_path);
    (void)remove(test_path);
    (void)puts(
        "transaction crash: multi-txn WAL commit boundaries ok"
    );
}

/*
 * (1b) Genuine PARTIAL write of a MULTI-frame txn.
 *
 * The default io_limit (SIZE_MAX) turns every append into a single pwrite, so
 * the write boundary is all-or-nothing — it can never leave a half-written
 * frame region on disk. Here we shrink the WAL fd's per-pwrite chunk so a
 * two-frame append is split across several pwrites, then fault AFTER the first
 * chunk (sdb_wal_fail_after_for_testing(1)). That leaves the frame region
 * TORN: the leading bytes are durable, the tail is missing.
 *
 * Precondition: one prior single-page txn (txn1 → p2) is durable in the WAL.
 * The torn txn (p1 + p3) must be dropped WHOLE by the running-CRC gate, and
 * the durable prefix (txn1) must survive untouched with a coherent LSN. A
 * recovery that honoured the torn frames — replaying one page while dropping
 * the other, or advancing the LSN past the torn txn — trips the asserts.
 */
static void test_multi_frame_partial_write(void)
{
    sdb_pager pager;
    uint64_t p1;
    uint64_t p2;
    uint64_t p3;
    sdb_txn txn;
    sdb_status commit_status;
    prepare_three_pages(&pager, &p1, &p2, &p3);

    /* Durable prefix: txn1 writes p2 (one page, one clean single-fsync). */
    commit_single_page_txn(&pager, p2, v_txn2_p2, sizeof(v_txn2_p2));
    /*
     * The WAL fd is lazily opened by the first commit; it is now live. Shrink
     * its per-pwrite chunk so the next (two-frame) append fragments into
     * multiple pwrites. io_limit survives append_txn's re-arm (which only
     * resets the fail counter, not the chunk size).
     */
    assert(pager.wal_file_open);
    sdb_file_set_io_limit_for_testing(&pager.wal_file, 512U);

    /* txn2: two frames (p1 + p3). Fault after the first pwrite chunk. */
    sdb_wal_fail_after_for_testing(1U);
    assert(sdb_txn_begin(&pager, &txn) == SDB_OK);
    assert(sdb_txn_put(
        &txn, p1, (uint16_t)SDB_PAGE_TYPE_DATA, v_txn4_p1, sizeof(v_txn4_p1)
    ) == SDB_OK);
    assert(sdb_txn_put(
        &txn, p3, (uint16_t)SDB_PAGE_TYPE_DATA, v_txn3_p3, sizeof(v_txn3_p3)
    ) == SDB_OK);
    commit_status = sdb_txn_commit(&txn);
    sdb_wal_clear_failure_for_testing();
    /* Mid-write fault ⇒ commit must report failure. */
    assert(commit_status != SDB_OK);
    assert(sdb_pager_close(&pager) == SDB_OK);

    assert(sdb_pager_open(test_path, &pager) == SDB_OK);
    /* Durable prefix survives. */
    assert(page_has_value(&pager, p2, v_txn2_p2, sizeof(v_txn2_p2)));
    /* Torn multi-frame txn dropped WHOLE: neither of its pages was applied. */
    assert(!page_has_value(&pager, p1, v_txn4_p1, sizeof(v_txn4_p1)));
    assert(!page_has_value(&pager, p3, v_txn3_p3, sizeof(v_txn3_p3)));
    assert(page_is_empty(&pager, p1));
    assert(page_is_empty(&pager, p3));
    /* LSN reflects ONLY the durable prefix (txn1). */
    assert(pager.superblock.checkpoint_lsn == 1U);
    assert(pager.current_lsn == 1U);
    assert(pager.superblock.next_page_id == p3 + 1U);
    assert(sdb_pager_close(&pager) == SDB_OK);

    (void)remove(wal_path);
    (void)remove(test_path);
    (void)puts(
        "transaction crash: multi-frame partial write dropped whole ok"
    );
}

/*
 * (2) Multi-txn uncheckpointed crash recovery.
 *
 * Three WAL-mode txns (txn1..txn3) are all durably committed, no checkpoint
 * has been driven, THEN the pager is closed abruptly (no explicit
 * checkpoint call, just close). The WAL still holds all three txns. On
 * reopen the recover-all path must replay ALL THREE — a partial replay
 * would drop a committed txn, which is the "lost commit" bug.
 *
 * Also asserts that txn4 (rewriting p1) chains on top: after reopen p1 must
 * hold v_txn4_p1 (newest wins across two durable rewrites of the same page).
 */
static void test_multi_txn_uncheckpointed_crash_recovery(void)
{
    sdb_pager pager;
    uint64_t p1;
    uint64_t p2;
    uint64_t p3;
    prepare_three_pages(&pager, &p1, &p2, &p3);

    commit_single_page_txn(&pager, p1, v_txn1_p1, sizeof(v_txn1_p1));
    commit_single_page_txn(&pager, p2, v_txn2_p2, sizeof(v_txn2_p2));
    commit_single_page_txn(&pager, p3, v_txn3_p3, sizeof(v_txn3_p3));
    /* Rewrite p1 with a fresh value to exercise newest-wins replay. */
    commit_single_page_txn(&pager, p1, v_txn4_p1, sizeof(v_txn4_p1));

    /* No checkpoint. Close abruptly — recovery MUST replay all 4 txns. */
    assert(sdb_pager_close(&pager) == SDB_OK);

    assert(sdb_pager_open(test_path, &pager) == SDB_OK);
    assert(page_has_value(&pager, p1, v_txn4_p1, sizeof(v_txn4_p1)));
    assert(page_has_value(&pager, p2, v_txn2_p2, sizeof(v_txn2_p2)));
    assert(page_has_value(&pager, p3, v_txn3_p3, sizeof(v_txn3_p3)));
    /* Checkpoint LSN reflects all four committed txns. */
    assert(pager.superblock.checkpoint_lsn == 4U);
    assert(pager.current_lsn == 4U);
    assert(sdb_pager_close(&pager) == SDB_OK);

    (void)remove(wal_path);
    (void)remove(test_path);
    (void)puts(
        "transaction crash: multi-txn uncheckpointed recovery ok"
    );
}

/*
 * (2b) R2a single-fsync fault path: an injected failure at either of the two
 * remaining I/O ops (op 0 = the one contiguous frames+commit write, op 1 =
 * the one fsync) MUST make sdb_txn_commit return an error, MUST NOT commit
 * the txn as far as recovery is concerned, and MUST leave a prior durable
 * txn untouched. Single-page txn ⇒ the outcome is strictly all-or-nothing:
 * p2 is either its new value (only possible for the fsync boundary via the
 * OS page cache, a legitimate durable-anyway result) or its fresh-allocated
 * empty state (write torn / fsync-not-durable ⇒ dropped). A "commit returned
 * OK but the write/fsync was faulted" outcome would be the durability bug.
 */
static void test_single_fsync_fault_path(void)
{
    size_t boundary;
    for (boundary = 0U; boundary < 2U; ++boundary) {
        sdb_pager pager;
        uint64_t p1;
        uint64_t p2;
        uint64_t p3;
        sdb_txn txn;
        sdb_status commit_status;
        bool p2_committed;
        prepare_three_pages(&pager, &p1, &p2, &p3);
        commit_single_page_txn(&pager, p1, v_txn1_p1, sizeof(v_txn1_p1));

        sdb_wal_fail_after_for_testing(boundary);
        assert(sdb_txn_begin(&pager, &txn) == SDB_OK);
        assert(sdb_txn_put(
            &txn, p2, (uint16_t)SDB_PAGE_TYPE_DATA,
            v_txn2_p2, sizeof(v_txn2_p2)
        ) == SDB_OK);
        commit_status = sdb_txn_commit(&txn);
        sdb_wal_clear_failure_for_testing();
        /* Both remaining ops fire the injection ⇒ commit must report error. */
        assert(commit_status != SDB_OK);
        assert(sdb_pager_close(&pager) == SDB_OK);

        assert(sdb_pager_open(test_path, &pager) == SDB_OK);
        /* Prior durable txn on p1 survives regardless of the fault. */
        assert(page_has_value(&pager, p1, v_txn1_p1, sizeof(v_txn1_p1)));
        p2_committed = page_has_value(&pager, p2, v_txn2_p2, sizeof(v_txn2_p2));
        if (!p2_committed) {
            /* Dropped: p2 stays fresh-allocated (no half-written frame). */
            assert(page_is_empty(&pager, p2));
            assert(pager.superblock.checkpoint_lsn == 1U);
            assert(pager.current_lsn == 1U);
        } else {
            /* Durable-anyway (fsync boundary only): consistent LSN. */
            assert(pager.superblock.checkpoint_lsn == 2U);
        }
        /* p3 never touched. */
        assert(page_is_empty(&pager, p3));
        assert(pager.superblock.next_page_id == p3 + 1U);
        assert(sdb_pager_close(&pager) == SDB_OK);
    }
    (void)remove(wal_path);
    (void)remove(test_path);
    (void)puts("transaction crash: single-fsync fault path ok");
}

/*
 * (2c) Persistent-fd lifecycle across a checkpoint (R2a). The pager holds one
 * WAL fd for its whole life instead of open()+close() per commit. This drives:
 *   commit txn1 → checkpoint (truncates the WAL THROUGH the live fd, resetting
 *   the append offset to SDB_WAL_HEADER_SIZE) → commit txn2 (must re-stamp the
 *   identity header at the head of the same live fd and land txn2 cleanly).
 * Then close+reopen and confirm both values survive with a coherent LSN. A
 * stale offset or a fd left pointing past the checkpointed frames would drop
 * txn2 on reopen; a missing header re-stamp would fail recover_all's gate.
 */
static void test_persistent_fd_checkpoint_reuse(void)
{
    sdb_pager pager;
    uint64_t p1;
    uint64_t p2;
    uint64_t p3;
    prepare_three_pages(&pager, &p1, &p2, &p3);

    commit_single_page_txn(&pager, p1, v_txn1_p1, sizeof(v_txn1_p1));
    /* Drain the WAL through the live fd; append offset resets to the header. */
    assert(sdb_pager_checkpoint(&pager) == SDB_OK);
    assert(pager.wal_tail == (uint64_t)SDB_WAL_HEADER_SIZE);
    assert(pager.wal_index.size == 0U);
    assert(pager.superblock.checkpoint_lsn == 1U);

    /* Reuse the SAME persistent fd: txn2 must re-stamp the header + append. */
    commit_single_page_txn(&pager, p2, v_txn2_p2, sizeof(v_txn2_p2));
    assert(pager.current_lsn == 2U);
    /* A third commit exercises the append-past-header branch on the live fd. */
    commit_single_page_txn(&pager, p3, v_txn3_p3, sizeof(v_txn3_p3));
    assert(pager.current_lsn == 3U);
    assert(sdb_pager_close(&pager) == SDB_OK);

    assert(sdb_pager_open(test_path, &pager) == SDB_OK);
    /* p1 came from the checkpointed data file; p2/p3 from the reused WAL. */
    assert(page_has_value(&pager, p1, v_txn1_p1, sizeof(v_txn1_p1)));
    assert(page_has_value(&pager, p2, v_txn2_p2, sizeof(v_txn2_p2)));
    assert(page_has_value(&pager, p3, v_txn3_p3, sizeof(v_txn3_p3)));
    assert(pager.superblock.checkpoint_lsn == 3U);
    assert(pager.current_lsn == 3U);
    assert(sdb_pager_close(&pager) == SDB_OK);

    (void)remove(wal_path);
    (void)remove(test_path);
    (void)puts("transaction crash: persistent-fd checkpoint reuse ok");
}

/*
 * (2d) Persistent-fd reopen loop (R2a). Open → commit → close, repeatedly, on
 * the same file. Each open lazily reopens the WAL fd on its first commit and
 * each close must release it; a leaked or half-closed fd would eventually
 * surface as an open/commit failure or a lost txn. Every reopen must still see
 * every previously committed page.
 */
static void test_persistent_fd_reopen_loop(void)
{
    sdb_pager pager;
    uint64_t p1;
    uint64_t p2;
    uint64_t p3;
    size_t round;
    const uint8_t *values[3];
    prepare_three_pages(&pager, &p1, &p2, &p3);
    assert(sdb_pager_close(&pager) == SDB_OK);

    values[0] = v_txn1_p1;
    values[1] = v_txn2_p2;
    values[2] = v_txn3_p3;
    for (round = 0U; round < 3U; ++round) {
        const uint64_t page = p1 + (uint64_t)round;
        assert(sdb_pager_open(test_path, &pager) == SDB_OK);
        /* Prior rounds' commits must be visible after each reopen. */
        {
            size_t seen;
            for (seen = 0U; seen < round; ++seen) {
                assert(page_has_value(
                    &pager, p1 + (uint64_t)seen, values[seen],
                    sizeof(v_txn1_p1)
                ));
            }
        }
        commit_single_page_txn(&pager, page, values[round], sizeof(v_txn1_p1));
        assert(sdb_pager_close(&pager) == SDB_OK);
    }

    assert(sdb_pager_open(test_path, &pager) == SDB_OK);
    assert(page_has_value(&pager, p1, v_txn1_p1, sizeof(v_txn1_p1)));
    assert(page_has_value(&pager, p2, v_txn2_p2, sizeof(v_txn2_p2)));
    assert(page_has_value(&pager, p3, v_txn3_p3, sizeof(v_txn3_p3)));
    assert(sdb_pager_close(&pager) == SDB_OK);

    (void)remove(wal_path);
    (void)remove(test_path);
    (void)puts("transaction crash: persistent-fd reopen loop ok");
}

#if SDB_TESTING
/*
 * (3) Multi-txn checkpoint-boundary matrix.
 *
 * Precondition per iteration: two WAL-mode txns (txn1 writes p1, txn2
 * writes p2) already durable in the WAL, WAL_index populated, data file
 * still holding the fresh-allocated empty pages. We drive
 * sdb_pager_checkpoint with a per-stage failure injected, close, reopen,
 * and require BOTH txns to be present regardless of which stage crashed:
 *
 *   BEFORE_DATA_SYNC   — some page bytes may have been written to the
 *                        data file but not fsynced; WAL and superblock
 *                        both still point at the OLD checkpoint_lsn. On
 *                        reopen the WAL replays txn1..txn2, overwriting
 *                        any partial data-file bytes.
 *   BEFORE_SUPERBLOCK  — data-file fsynced but superblock still points at
 *                        the old checkpoint_lsn. Reopen re-scans the WAL
 *                        from the old LSN and re-applies (idempotent).
 *   BEFORE_WAL_CLEAR   — data-file + superblock durable at the new LSN,
 *                        WAL still populated. Reopen scans WAL from the
 *                        new LSN, finds no eligible txn, and the open
 *                        path's sdb_wal_clear then truncates the WAL.
 *
 * A lost txn at any stage → page_has_value assert trips. Double-application
 * would show up as a wrong page_lsn (recovery goes idempotent by writing
 * the frame's page_lsn into the page — a re-applied frame carries the same
 * lsn so this reduces to "final bytes match").
 */
static void run_checkpoint_stage_case(
    sdb_checkpoint_fail_stage stage, const char *stage_name
)
{
    sdb_pager pager;
    uint64_t p1;
    uint64_t p2;
    uint64_t p3;
    sdb_status checkpoint_status;
    prepare_three_pages(&pager, &p1, &p2, &p3);

    commit_single_page_txn(&pager, p1, v_txn1_p1, sizeof(v_txn1_p1));
    commit_single_page_txn(&pager, p2, v_txn2_p2, sizeof(v_txn2_p2));

    sdb_pager_checkpoint_fail_at_for_testing(stage);
    checkpoint_status = sdb_pager_checkpoint(&pager);
    sdb_pager_checkpoint_clear_failure_for_testing();
    assert(checkpoint_status == SDB_E_IO);
    /*
     * A pre-superblock failure (BEFORE_DATA_SYNC / BEFORE_SUPERBLOCK) flags
     * needs_recovery; a post-superblock failure (BEFORE_WAL_CLEAR) leaves it
     * clear because the in-memory WAL state was reset to the checkpointed
     * position. Either way close still works (close doesn't guard on
     * needs_recovery — only txn ops do), and a no-op checkpoint-on-close is
     * safe because the wal_index is either untouched-then-replayed or empty.
     */
    assert(sdb_pager_close(&pager) == SDB_OK);

    assert(sdb_pager_open(test_path, &pager) == SDB_OK);
    assert(page_has_value(&pager, p1, v_txn1_p1, sizeof(v_txn1_p1)));
    assert(page_has_value(&pager, p2, v_txn2_p2, sizeof(v_txn2_p2)));
    /* p3 was never touched by any txn — must remain empty. */
    assert(page_is_empty(&pager, p3));
    /* Both durable txns are visible; checkpoint_lsn reflects them. */
    assert(pager.superblock.checkpoint_lsn == 2U);
    assert(pager.current_lsn == 2U);
    assert(sdb_pager_close(&pager) == SDB_OK);
    (void)printf(
        "transaction crash: checkpoint boundary %s ok\n", stage_name
    );
}

/*
 * (4) Regression: a commit AFTER a post-superblock checkpoint failure must
 * not be lost (adversarial finding CRITICAL #1).
 *
 * sdb_pager_checkpoint advances the on-disk superblock (checkpoint_lsn =
 * current_lsn) DURABLY, then clears the WAL. If the wal_clear step fails
 * (real EIO/ENOSPC) AFTER the superblock is durable, the checkpoint returns
 * an error. The bug: it used to leave wal_tail pointing past the already-
 * checkpointed frames while a caller (sdb_database_backup_unlocked) cleared
 * needs_recovery and kept accepting commits. The next commit got
 * txn_id = current_lsn+1 and was appended at that STALE tail. On
 * crash+reopen the durable superblock said checkpoint_lsn = N, recover_all
 * expected txn N+1 at the WAL head but found the old checkpointed frames
 * there, stopped at the torn tail, and never reached the new txn — silently
 * losing a committed transaction.
 *
 * The fix resets the in-memory WAL position (empty index, tail at header)
 * as soon as the superblock is durable, regardless of wal_clear's outcome,
 * so the next commit appends from the header and replays cleanly. This test
 * fires BEFORE_WAL_CLEAR (superblock durable, WAL left intact), mimics the
 * backup path's needs_recovery clear, commits one more txn, then crashes
 * (pager close never checkpoints) and asserts the new txn survives.
 */
static void test_commit_after_post_superblock_checkpoint_failure(void)
{
    sdb_pager pager;
    uint64_t p1;
    uint64_t p2;
    uint64_t p3;
    sdb_status checkpoint_status;
    prepare_three_pages(&pager, &p1, &p2, &p3);

    commit_single_page_txn(&pager, p1, v_txn1_p1, sizeof(v_txn1_p1));
    commit_single_page_txn(&pager, p2, v_txn2_p2, sizeof(v_txn2_p2));

    /*
     * Checkpoint fails AFTER the superblock is durably advanced but before
     * the WAL is cleared (models a real wal_clear EIO/ENOSPC).
     */
    sdb_pager_checkpoint_fail_at_for_testing(
        SDB_CHECKPOINT_FAIL_BEFORE_WAL_CLEAR
    );
    checkpoint_status = sdb_pager_checkpoint(&pager);
    sdb_pager_checkpoint_clear_failure_for_testing();
    assert(checkpoint_status == SDB_E_IO);

    /*
     * The superblock advanced to checkpoint_lsn=2 durably; the fix reset the
     * in-memory WAL position to the checkpointed state, leaving a self-
     * consistent state that keeps accepting commits (needs_recovery clear).
     * Pre-fix: wal_tail stayed at the end of the two checkpointed txns.
     */
    assert(!pager.needs_recovery);
    assert(pager.superblock.checkpoint_lsn == 2U);
    assert(pager.current_lsn == 2U);
    assert(pager.wal_tail == (uint64_t)SDB_WAL_HEADER_SIZE);
    assert(pager.wal_index.size == 0U);

    /*
     * Mirror sdb_database_backup_unlocked: clear needs_recovery after a
     * checkpoint failure so the source stays usable. (No-op post-fix; pre-
     * fix this is what re-enabled the losing commit path.)
     */
    pager.needs_recovery = false;

    /* Next commit lands txn_id = current_lsn+1 = 3 at wal_tail. */
    commit_single_page_txn(&pager, p3, v_txn3_p3, sizeof(v_txn3_p3));
    assert(pager.current_lsn == 3U);

    /*
     * Crash: pager close never checkpoints, so the WAL is the source of
     * truth for the next open.
     */
    assert(sdb_pager_close(&pager) == SDB_OK);

    assert(sdb_pager_open(test_path, &pager) == SDB_OK);
    /* The newly committed txn3 MUST NOT be lost. */
    assert(page_has_value(&pager, p3, v_txn3_p3, sizeof(v_txn3_p3)));
    /* The two prior txns were checkpointed into the data file. */
    assert(page_has_value(&pager, p1, v_txn1_p1, sizeof(v_txn1_p1)));
    assert(page_has_value(&pager, p2, v_txn2_p2, sizeof(v_txn2_p2)));
    assert(pager.superblock.checkpoint_lsn == 3U);
    assert(pager.current_lsn == 3U);
    assert(sdb_pager_close(&pager) == SDB_OK);

    (void)remove(wal_path);
    (void)remove(test_path);
    (void)puts(
        "transaction crash: commit after post-superblock checkpoint "
        "failure survives ok"
    );
}

static void test_multi_txn_checkpoint_boundaries(void)
{
    run_checkpoint_stage_case(
        SDB_CHECKPOINT_FAIL_BEFORE_DATA_SYNC, "BEFORE_DATA_SYNC"
    );
    run_checkpoint_stage_case(
        SDB_CHECKPOINT_FAIL_BEFORE_SUPERBLOCK, "BEFORE_SUPERBLOCK"
    );
    run_checkpoint_stage_case(
        SDB_CHECKPOINT_FAIL_BEFORE_WAL_CLEAR, "BEFORE_WAL_CLEAR"
    );

    /*
     * Clean checkpoint (no injection) — after this call the data file is
     * authoritative, the WAL is empty, and both txns are still readable.
     */
    {
        sdb_pager pager;
        uint64_t p1;
        uint64_t p2;
        uint64_t p3;
        prepare_three_pages(&pager, &p1, &p2, &p3);
        commit_single_page_txn(&pager, p1, v_txn1_p1, sizeof(v_txn1_p1));
        commit_single_page_txn(&pager, p2, v_txn2_p2, sizeof(v_txn2_p2));
        assert(sdb_pager_checkpoint(&pager) == SDB_OK);
        assert(pager.wal_tail == (uint64_t)SDB_WAL_HEADER_SIZE);
        assert(pager.wal_index.size == 0U);
        assert(pager.superblock.checkpoint_lsn == 2U);
        assert(page_has_value(&pager, p1, v_txn1_p1, sizeof(v_txn1_p1)));
        assert(page_has_value(&pager, p2, v_txn2_p2, sizeof(v_txn2_p2)));
        assert(page_is_empty(&pager, p3));
        assert(sdb_pager_close(&pager) == SDB_OK);
        /* Reopen: everything already in the data file. */
        assert(sdb_pager_open(test_path, &pager) == SDB_OK);
        assert(page_has_value(&pager, p1, v_txn1_p1, sizeof(v_txn1_p1)));
        assert(page_has_value(&pager, p2, v_txn2_p2, sizeof(v_txn2_p2)));
        assert(sdb_pager_close(&pager) == SDB_OK);
    }

    (void)remove(wal_path);
    (void)remove(test_path);
    (void)puts(
        "transaction crash: multi-txn checkpoint boundary matrix ok"
    );
}
#endif /* SDB_TESTING */

int main(void)
{
    test_legacy_single_txn_wal_boundaries();
    test_legacy_single_txn_file_boundaries();
    test_multi_txn_wal_commit_boundaries();
    test_multi_frame_partial_write();
    test_multi_txn_uncheckpointed_crash_recovery();
    test_single_fsync_fault_path();
    test_persistent_fd_checkpoint_reuse();
    test_persistent_fd_reopen_loop();
#if SDB_TESTING
    test_commit_after_post_superblock_checkpoint_failure();
    test_multi_txn_checkpoint_boundaries();
#endif

    (void)remove(wal_path);
    (void)remove(test_path);
    (void)puts("transaction crash tests: ok");
    return 0;
}
