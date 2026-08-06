/*
 * R4 group commit — deterministic batch durability / crash matrix, driven at
 * the pager layer so the coalescing and its failure boundaries are fully under
 * test control (no thread-timing races). Covers:
 *
 *   1. coalesced_batch_durable   — N deferred txns flushed by ONE leader fsync
 *                                  all survive a close/reopen.
 *   2. torn_batch_drops_tail     — a write fault mid-batch (before any fsync)
 *                                  leaves the intact prefix recoverable and
 *                                  drops the torn txn WHOLE — never half a txn.
 *   3. flush_failure_poisons      — a leader fsync failure propagates an error
 *                                  to the commit and never falsely reports OK;
 *                                  a batch that WAS durable earlier is intact.
 */
#include "file.h"
#include "pager.h"
#include "wal.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static const char *pager_path = "test-group-commit-batch.tmp";
static const char *wal_path = "test-group-commit-batch.tmp.wal";

static void cleanup(void)
{
    (void)remove(pager_path);
    (void)remove(wal_path);
}

/*
 * Stage one deferred txn (single data page) and return its assigned txn id
 * WITHOUT making it durable — it lands in the group-commit queue.
 */
static uint64_t stage_txn(sdb_pager *pager, uint8_t marker)
{
    sdb_txn txn;
    uint64_t page_id = 0U;
    uint64_t txn_id = 0U;
    uint8_t payload[16];
    (void)memset(payload, marker, sizeof(payload));
    assert(sdb_txn_begin(pager, &txn) == SDB_OK);
    assert(sdb_txn_allocate(&txn, &page_id) == SDB_OK);
    assert(sdb_txn_put(
        &txn, page_id, (uint16_t)SDB_PAGE_TYPE_DATA, payload, sizeof(payload)
    ) == SDB_OK);
    assert(sdb_txn_commit(&txn) == SDB_OK);
    assert(sdb_pager_take_pending(pager, &txn_id));
    assert(txn_id != 0U);
    return txn_id;
}

static void test_coalesced_batch_durable(void)
{
    sdb_pager pager;
    uint8_t salt[16] = {1U};
    uint8_t file_id[16] = {2U};
    uint64_t t1;
    uint64_t t2;
    uint64_t t3;

    cleanup();
    assert(sdb_pager_create(pager_path, 4096U, salt, file_id, &pager) == SDB_OK);
    sdb_pager_set_defer_commit(&pager, true);
    t1 = stage_txn(&pager, 0xA1U);
    t2 = stage_txn(&pager, 0xB2U);
    t3 = stage_txn(&pager, 0xC3U);
    assert(t2 == t1 + 1U && t3 == t2 + 1U);
    /*
     * PREPARE pwrites each txn's bytes with NO fsync, so after staging three
     * txns the WAL fd has not been synced even once.
     */
    assert(sdb_file_sync_count_for_testing(&pager.wal_file) == 0U);
    /*
     * One leader, ONE fsync, whole queue durable — the coalescing proof:
     * three txns cost a single fsync, not three.
     */
    assert(sdb_pager_commit_durable(&pager, t3) == SDB_OK);
    assert(sdb_file_sync_count_for_testing(&pager.wal_file) == 1U);
    sdb_pager_set_defer_commit(&pager, false);
    assert(pager.durable_lsn >= t3);
    assert(sdb_pager_close(&pager) == SDB_OK);

    /* Reopen: recovery replays all three coalesced txns. */
    assert(sdb_pager_open(pager_path, &pager) == SDB_OK);
    assert(pager.superblock.checkpoint_lsn == t3);
    assert(sdb_pager_close(&pager) == SDB_OK);
    (void)puts("  coalesced_batch_durable: ok");
}

static void test_torn_batch_drops_tail(void)
{
    sdb_pager pager;
    uint8_t salt[16] = {3U};
    uint8_t file_id[16] = {4U};
    uint64_t t1;
    sdb_status t2_status;
    sdb_txn txn;
    uint64_t page_id = 0U;
    uint8_t payload[16];

    cleanup();
    assert(sdb_pager_create(pager_path, 4096U, salt, file_id, &pager) == SDB_OK);
    sdb_pager_set_defer_commit(&pager, true);

    /*
     * t1 stages cleanly: its bytes are pwritten during PREPARE (per-txn, under
     * the engine mutex) and written_lsn advances to t1. No fsync yet — that is
     * the leader's job in sdb_pager_commit_durable.
     */
    t1 = stage_txn(&pager, 0x51U);
    assert(pager.wal_file_open);
    assert(pager.written_lsn == t1);

    /*
     * Arm a write fault so the NEXT WAL file op fails. In this architecture the
     * torn boundary is the tail txn's PREPARE pwrite, NOT a leader write (the
     * leader only fsyncs). t2's commit encodes fine but its pwrite fails, so
     * sdb_txn_commit reports the error, written_lsn never advances past t1, and
     * needs_recovery is set — the tail txn is rejected WHOLE, never half.
     */
    (void)memset(payload, 0x62U, sizeof(payload));
    assert(sdb_txn_begin(&pager, &txn) == SDB_OK);
    assert(sdb_txn_allocate(&txn, &page_id) == SDB_OK);
    assert(sdb_txn_put(
        &txn, page_id, (uint16_t)SDB_PAGE_TYPE_DATA, payload, sizeof(payload)
    ) == SDB_OK);
    /*
     * Arm just before commit so the fault lands on the PREPARE pwrite (the
     * first WAL file op in the commit path), not on any earlier bookkeeping.
     */
    sdb_file_fail_after_for_testing(&pager.wal_file, 0U);
    t2_status = sdb_txn_commit(&txn);
    sdb_file_clear_failure_for_testing(&pager.wal_file);
    assert(t2_status != SDB_OK);
    /* The failed tail txn did not become durable-able. */
    assert(pager.written_lsn == t1);
    assert(pager.needs_recovery);

    /*
     * t1's bytes are on disk and durable-able; the leader fsync commits exactly
     * the intact prefix.
     */
    assert(sdb_pager_commit_durable(&pager, t1) == SDB_OK);
    assert(pager.durable_lsn == t1);
    sdb_pager_set_defer_commit(&pager, false);
    assert(sdb_pager_close(&pager) == SDB_OK);

    /*
     * Reopen: t1's frames+commit-record replay; the torn tail leaves nothing
     * recoverable past it. No half-applied txn: checkpoint_lsn stops at t1.
     */
    assert(sdb_pager_open(pager_path, &pager) == SDB_OK);
    assert(pager.superblock.checkpoint_lsn == t1);
    assert(sdb_pager_close(&pager) == SDB_OK);
    (void)puts("  torn_batch_drops_tail: ok");
}

static void test_flush_failure_poisons(void)
{
    sdb_pager pager;
    uint8_t salt[16] = {5U};
    uint8_t file_id[16] = {6U};
    uint64_t durable_txn;
    uint64_t doomed_txn;
    sdb_status status;

    cleanup();
    assert(sdb_pager_create(pager_path, 4096U, salt, file_id, &pager) == SDB_OK);

    /* Batch A — a normal synchronous commit that IS made durable. */
    {
        sdb_txn txn;
        uint64_t page_id = 0U;
        uint8_t payload[16];
        (void)memset(payload, 0x77U, sizeof(payload));
        assert(sdb_txn_begin(&pager, &txn) == SDB_OK);
        assert(sdb_txn_allocate(&txn, &page_id) == SDB_OK);
        assert(sdb_txn_put(
            &txn, page_id, (uint16_t)SDB_PAGE_TYPE_DATA,
            payload, sizeof(payload)
        ) == SDB_OK);
        assert(sdb_txn_commit(&txn) == SDB_OK);
    }
    durable_txn = pager.current_lsn;
    assert(durable_txn >= 1U);

    /* Batch B — staged, then the leader's fsync is forced to fail. */
    sdb_pager_set_defer_commit(&pager, true);
    doomed_txn = stage_txn(&pager, 0x99U);
    sdb_pager_group_commit_fail_next_flush_for_testing();
    status = sdb_pager_commit_durable(&pager, doomed_txn);
    sdb_pager_group_commit_clear_failure_for_testing();
    sdb_pager_set_defer_commit(&pager, false);
    /* The leader failure is reported — never a false SDB_OK. */
    assert(status != SDB_OK);
    /* Sticky poison: any later durability attempt keeps failing. */
    assert(sdb_pager_commit_durable(&pager, doomed_txn) != SDB_OK);
    pager.needs_recovery = true;
    assert(sdb_pager_close(&pager) == SDB_OK);

    /* Reopen: the earlier durable batch is never lost. */
    assert(sdb_pager_open(pager_path, &pager) == SDB_OK);
    assert(pager.superblock.checkpoint_lsn >= durable_txn);
    assert(sdb_pager_close(&pager) == SDB_OK);
    (void)puts("  flush_failure_poisons: ok");
}

int main(void)
{
    test_coalesced_batch_durable();
    test_torn_batch_drops_tail();
    test_flush_failure_poisons();
    cleanup();
    (void)puts("group commit batch tests: ok");
    return 0;
}
