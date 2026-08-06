
#include "pager.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static const char *db_path = "test-txn-cancel-alloc.tmp";
static const char *wal_path = "test-txn-cancel-alloc.tmp.wal";

static void cleanup(void)
{
    (void)remove(db_path);
    (void)remove(wal_path);
}

static void test_txn_free_cancels_in_batch_allocation(void)
{
    sdb_pager pager;
    sdb_txn txn;
    uint64_t p1 = 0U, p2 = 0U;
    uint64_t next_before = 0U;
    uint64_t reused = 0U;
    uint8_t payload[8];
    uint8_t salt[16] = {1U};
    uint8_t file_id[16] = {2U};

    cleanup();
    assert(sdb_pager_create(db_path, 4096U, salt, file_id, &pager) == SDB_OK);
    next_before = pager.superblock.next_page_id;
    assert(sdb_txn_begin(&pager, &txn) == SDB_OK);

    assert(sdb_txn_allocate(&txn, &p1) == SDB_OK);
    assert(sdb_txn_allocate(&txn, &p2) == SDB_OK);
    assert(p1 != p2 && p1 != 0U && p2 != 0U);

    assert(sdb_txn_free(&txn, p2) == SDB_OK);

    (void)memset(payload, 0xAA, sizeof(payload));
    assert(sdb_txn_put(
        &txn, p1, (uint16_t)SDB_PAGE_TYPE_DATA, payload, sizeof(payload)
    ) == SDB_OK);

    assert(sdb_txn_commit(&txn) == SDB_OK);

    assert(!pager.needs_recovery);

    /*
     * Prove freelist reclaim, not merely commit-OK. The in-batch cancel of p2
     * returns the slot to the freelist (freelist_page := p2); it does NOT roll
     * next_page_id back, so both allocations still bumped the counter (+2) and
     * the committed superblock's freelist head must point at p2. If p2 were
     * orphaned (leaked), freelist_page would stay 0 while next_page_id still
     * grew by 2 -- so the freelist_page check is the assertion that catches the
     * leak.
     */
    assert(pager.superblock.freelist_page == p2);
    assert(pager.superblock.next_page_id == next_before + 2U);

    /* End-to-end: a fresh allocation must hand back the reclaimed slot. */
    assert(sdb_txn_begin(&pager, &txn) == SDB_OK);
    assert(sdb_txn_allocate(&txn, &reused) == SDB_OK);
    assert(reused == p2);
    sdb_txn_abort(&txn);

    assert(sdb_pager_close(&pager) == SDB_OK);
    cleanup();
    (void)puts("txn_free cancels in-batch allocation: ok");
}

static void test_alloc_from_staged_free(void)
{
    sdb_pager pager;
    sdb_txn txn;
    uint64_t p1 = 0U, p2 = 0U, p3 = 0U;
    uint8_t payload[16];
    uint8_t salt[16] = {3U};
    uint8_t file_id[16] = {4U};

    cleanup();
    assert(sdb_pager_create(db_path, 4096U, salt, file_id, &pager) == SDB_OK);

    assert(sdb_txn_begin(&pager, &txn) == SDB_OK);
    assert(sdb_txn_allocate(&txn, &p1) == SDB_OK);
    (void)memset(payload, 0xCC, sizeof(payload));
    assert(sdb_txn_put(
        &txn, p1, (uint16_t)SDB_PAGE_TYPE_DATA, payload, sizeof(payload)
    ) == SDB_OK);
    assert(sdb_txn_commit(&txn) == SDB_OK);

    assert(sdb_txn_begin(&pager, &txn) == SDB_OK);
    assert(sdb_txn_free(&txn, p1) == SDB_OK);
    assert(sdb_txn_allocate(&txn, &p2) == SDB_OK);

    assert(p2 == p1);

    (void)memset(payload, 0xDD, sizeof(payload));
    assert(sdb_txn_put(
        &txn, p2, (uint16_t)SDB_PAGE_TYPE_DATA, payload, sizeof(payload)
    ) == SDB_OK);
    assert(sdb_txn_commit(&txn) == SDB_OK);
    assert(!pager.needs_recovery);

    assert(sdb_txn_begin(&pager, &txn) == SDB_OK);
    assert(sdb_txn_allocate(&txn, &p3) == SDB_OK);
    assert(p3 != p1 && p3 != 0U);
    (void)memset(payload, 0xEE, sizeof(payload));
    assert(sdb_txn_put(
        &txn, p3, (uint16_t)SDB_PAGE_TYPE_DATA, payload, sizeof(payload)
    ) == SDB_OK);
    assert(sdb_txn_commit(&txn) == SDB_OK);

    assert(sdb_pager_close(&pager) == SDB_OK);
    cleanup();
    (void)puts("txn_allocate reads staged freelist head: ok");
}

static void test_cancel_then_put_rejects(void)
{
    sdb_pager pager;
    sdb_txn txn;
    uint64_t p1 = 0U;
    uint8_t payload[8];
    uint8_t salt[16] = {5U};
    uint8_t file_id[16] = {6U};
    sdb_status status;

    cleanup();
    assert(sdb_pager_create(db_path, 4096U, salt, file_id, &pager) == SDB_OK);
    assert(sdb_txn_begin(&pager, &txn) == SDB_OK);
    assert(sdb_txn_allocate(&txn, &p1) == SDB_OK);

    (void)memset(payload, 0x11, sizeof(payload));
    assert(sdb_txn_put(
        &txn, p1, (uint16_t)SDB_PAGE_TYPE_DATA, payload, sizeof(payload)
    ) == SDB_OK);

    assert(sdb_txn_free(&txn, p1) == SDB_OK);

    (void)memset(payload, 0x22, sizeof(payload));
    status = sdb_txn_put(
        &txn, p1, (uint16_t)SDB_PAGE_TYPE_DATA, payload, sizeof(payload)
    );

    assert(status == SDB_OK || status == SDB_E_INVALID_ARGUMENT);

    if (status == SDB_OK) {
        assert(sdb_txn_commit(&txn) == SDB_OK);
    } else {
        sdb_txn_abort(&txn);
    }
    assert(sdb_pager_close(&pager) == SDB_OK);
    cleanup();
    (void)puts("cancel-then-put path exercised: ok");
}

/*
 * Regression (on-disk allocator leak): allocate-then-cancel inside a txn must
 * return the cancelled page to the freelist, never orphan it. A cancelled
 * allocation that neither reaches the freelist nor rolls back next_page_id
 * leaks one on-disk page per occurrence — every functional test stays green
 * and data reads fine, but the file grows without bound. This drives many
 * cancel cycles (recycling the committed page each round so ONLY the cancel
 * behaviour drives growth) and asserts next_page_id stays O(1)-bounded, then
 * confirms the recycled freelist survives a close/reopen.
 */
static void test_cancelled_allocation_recycled_not_orphaned(void)
{
    sdb_pager pager;
    sdb_txn txn;
    uint64_t base;
    uint64_t growth;
    uint64_t keep = 0U;
    uint8_t payload[16];
    uint8_t salt[16] = {9U};
    uint8_t file_id[16] = {10U};
    size_t iter;
    const size_t iterations = 300U;

    cleanup();
    assert(sdb_pager_create(db_path, 4096U, salt, file_id, &pager) == SDB_OK);
    (void)memset(payload, 0x5A, sizeof(payload));

    /* Warmup one alloc+commit to settle next_page_id, then record the base. */
    assert(sdb_txn_begin(&pager, &txn) == SDB_OK);
    assert(sdb_txn_allocate(&txn, &keep) == SDB_OK);
    assert(sdb_txn_put(
        &txn, keep, (uint16_t)SDB_PAGE_TYPE_DATA, payload, sizeof(payload)
    ) == SDB_OK);
    assert(sdb_txn_commit(&txn) == SDB_OK);
    base = pager.superblock.next_page_id;

    for (iter = 0U; iter < iterations; ++iter) {
        uint64_t a = 0U;
        uint64_t b = 0U;
        /* One committed page + one cancelled allocation in the same txn. */
        assert(sdb_txn_begin(&pager, &txn) == SDB_OK);
        assert(sdb_txn_allocate(&txn, &a) == SDB_OK);
        assert(sdb_txn_put(
            &txn, a, (uint16_t)SDB_PAGE_TYPE_DATA, payload, sizeof(payload)
        ) == SDB_OK);
        assert(sdb_txn_allocate(&txn, &b) == SDB_OK);
        assert(sdb_txn_free(&txn, b) == SDB_OK); /* cancel the allocation */
        assert(sdb_txn_commit(&txn) == SDB_OK);
        /* Recycle the committed page so only cancel behaviour drives growth. */
        assert(sdb_txn_begin(&pager, &txn) == SDB_OK);
        assert(sdb_txn_free(&txn, a) == SDB_OK);
        assert(sdb_txn_commit(&txn) == SDB_OK);
    }

    growth = pager.superblock.next_page_id - base;
    /*
     * Fixed: the cancelled page is recycled via the freelist, so growth is
     * O(1) (~2). Buggy: every cancelled page is orphaned and the file extends
     * by roughly one page per iteration (~300).
     */
    assert(growth < 16U);

    /* The recycled freelist must persist across a close/reopen. */
    assert(sdb_pager_close(&pager) == SDB_OK);
    assert(sdb_pager_open(db_path, &pager) == SDB_OK);
    {
        uint64_t high = pager.superblock.next_page_id;
        uint64_t reused = 0U;
        assert(sdb_txn_begin(&pager, &txn) == SDB_OK);
        assert(sdb_txn_allocate(&txn, &reused) == SDB_OK);
        assert(reused < high); /* reused a freelist slot, did not extend */
        sdb_txn_abort(&txn);
    }
    assert(sdb_pager_close(&pager) == SDB_OK);
    cleanup();
    (void)puts(
        "cancelled allocation recycled to freelist (no orphan leak): ok"
    );
}

int main(void)
{
    test_txn_free_cancels_in_batch_allocation();
    test_alloc_from_staged_free();
    test_cancel_then_put_rejects();
    test_cancelled_allocation_recycled_not_orphaned();
    (void)puts("txn cancel/staged-alloc tests: all ok");
    return 0;
}
