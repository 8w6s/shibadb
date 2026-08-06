/*
 * Churn reclaim gate. A delete followed by a re-insert of the same keys must
 * NOT accumulate dead rows without bound: an object delete has to remove the
 * object's chunk rows, not just its metadata row. Before the fix, sdb_object_delete
 * deleted only the metadata key and orphaned every chunk row, so a
 * delete+reinsert churn (session/cache expiry, queues) grew stale_entry_count
 * (and the data file) linearly forever, reclaimable only by compact().
 *
 * This gate churns the same key set many times and asserts the dead-row count
 * stays bounded relative to the live set — i.e. delete actually reclaims. It is
 * a regression test for that fix and the required gate for any future change to
 * the reclaim/generation scheme.
 */
#include "shibadb_engine.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#define KEYS 3000U
#define ROUNDS 10U

static const char *db_path = "test-churn-reclaim.tmp";
static const char *wal_path = "test-churn-reclaim.tmp.wal";
static const char *lock_path = "test-churn-reclaim.tmp.lock";
static const uint8_t namespace_name[] = "ch";
static const uint8_t password[] = "churn-reclaim-password";

static void encode_u32(uint8_t out[4], uint32_t v)
{
    out[0] = (uint8_t)v;
    out[1] = (uint8_t)(v >> 8U);
    out[2] = (uint8_t)(v >> 16U);
    out[3] = (uint8_t)(v >> 24U);
}

int main(void)
{
    sdb_database_options options;
    sdb_database *database = NULL;
    sdb_transaction *txn = NULL;
    sdb_verify_result verify_result;
    unsigned round;
    uint32_t i;
    uint64_t stale_after_first = 0U;
    uint64_t pages_after_first = 0U;

    (void)remove(db_path);
    (void)remove(wal_path);
    (void)remove(lock_path);
    sdb_database_options_init(&options);
    options.kdf_iterations = SDB_MIN_KDF_ITERATIONS;
    options.password = password;
    options.password_size = sizeof(password) - 1U;
    assert(sdb_database_create(db_path, &options, &database) == SDB_OK);

    for (round = 0U; round < ROUNDS; ++round) {
        /* delete every key */
        assert(sdb_transaction_begin(database, &txn) == SDB_OK);
        for (i = 0U; i < KEYS; ++i) {
            uint8_t key[4];
            encode_u32(key, i);
            (void)sdb_transaction_kv_delete(
                txn, namespace_name, sizeof(namespace_name), key, sizeof(key)
            );
        }
        assert(sdb_transaction_commit(txn) == SDB_OK);
        assert(sdb_transaction_close(txn) == SDB_OK);
        /* re-insert every key */
        assert(sdb_transaction_begin(database, &txn) == SDB_OK);
        for (i = 0U; i < KEYS; ++i) {
            uint8_t key[4];
            uint8_t value[4];
            encode_u32(key, i);
            encode_u32(value, round * 131U + i);
            assert(sdb_transaction_kv_put(
                txn, namespace_name, sizeof(namespace_name),
                key, sizeof(key), value, sizeof(value)
            ) == SDB_OK);
        }
        assert(sdb_transaction_commit(txn) == SDB_OK);
        assert(sdb_transaction_close(txn) == SDB_OK);

        assert(sdb_database_verify(database, &verify_result) == SDB_OK);
        /* Live set is exactly KEYS chunks every round. */
        assert(verify_result.live_chunk_count == KEYS);
        if (round == 0U) {
            stale_after_first = verify_result.stale_entry_count;
            pages_after_first = verify_result.allocated_page_count;
        }
        /*
         * The gate: dead rows must stay bounded, not grow with the number of
         * churn rounds. Before the chunk-delete fix this grew by KEYS every
         * round (10*KEYS by the end); with reclaim working it stays near the
         * first round's level. Allow generous slack (a little transient dead
         * state between commit and the next reclaim is fine) but reject linear
         * growth: cap at 2*KEYS regardless of round count.
         */
        assert(verify_result.stale_entry_count <= 2U * (uint64_t)KEYS);
        assert(verify_result.allocated_page_count <= pages_after_first + 16U);
    }

    /*
     * Overwrite churn: re-put the SAME keys with new values, no delete. An
     * in-place update must reclaim the previous version's chunk rows too —
     * object_put assigns a fresh (monotonic) generation, so without cleanup the
     * old-generation chunks orphan exactly like the delete case did. Dead rows
     * must stay bounded here as well.
     */
    for (round = 0U; round < ROUNDS; ++round) {
        assert(sdb_transaction_begin(database, &txn) == SDB_OK);
        for (i = 0U; i < KEYS; ++i) {
            uint8_t key[4];
            uint8_t value[4];
            encode_u32(key, i);
            encode_u32(value, 0x9000U + round * 17U + i);
            assert(sdb_transaction_kv_put(
                txn, namespace_name, sizeof(namespace_name),
                key, sizeof(key), value, sizeof(value)
            ) == SDB_OK);
        }
        assert(sdb_transaction_commit(txn) == SDB_OK);
        assert(sdb_transaction_close(txn) == SDB_OK);
        assert(sdb_database_verify(database, &verify_result) == SDB_OK);
        assert(verify_result.live_chunk_count == KEYS);
        assert(verify_result.stale_entry_count <= 2U * (uint64_t)KEYS);
        assert(verify_result.allocated_page_count <= pages_after_first + 16U);
    }

    (void)stale_after_first;
    assert(sdb_database_close(database) == SDB_OK);
    (void)remove(db_path);
    (void)remove(wal_path);
    (void)remove(lock_path);
    (void)printf(
        "churn reclaim: ok (%u rounds x %u keys, dead rows stay bounded)\n",
        ROUNDS, KEYS
    );
    return 0;
}
