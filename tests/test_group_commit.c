/*
 * R4 group commit — concurrent auto-commit stress. Many threads each drive a
 * long run of independent sdb_kv_put commits against ONE shared sdb_database
 * handle. The engine coalesces their fsyncs behind a single leader, so this is
 * the primary data-race gate (run under ThreadSanitizer) AND a durability
 * check: every commit that returns SDB_OK must survive a close/reopen, none
 * may be lost, and none may be applied twice (last-writer value must match).
 */
#include "shibadb_engine.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#endif

#define THREAD_COUNT 16U
#define RECORDS_PER_THREAD 96U

static const char *database_path = "test-group-commit.tmp";
static const char *wal_path = "test-group-commit.tmp.wal";
static const char *lock_path = "test-group-commit.tmp.lock";
static const char *compact_db_path = "test-group-commit-compact.tmp";
static const char *compact_wal_path = "test-group-commit-compact.tmp.wal";
static const char *compact_lock_path = "test-group-commit-compact.tmp.lock";
static const uint8_t namespace_name[] = "gc";
#define HOT_KEY_WRITES 128U

static void encode_u32(uint8_t output[4], uint32_t value)
{
    output[0] = (uint8_t)value;
    output[1] = (uint8_t)(value >> 8U);
    output[2] = (uint8_t)(value >> 16U);
    output[3] = (uint8_t)(value >> 24U);
}

typedef struct worker_context {
    sdb_database *database;
    uint32_t worker;
    bool failed;
    int fail_status;
    uint32_t fail_record;
#ifndef _WIN32
    atomic_uint *finished;
#endif
} worker_context;

/*
 * Each thread commits RECORDS_PER_THREAD independent keys via the auto-commit
 * API. Every put is its own txn — so THREAD_COUNT threads race to commit
 * concurrently and the group-commit leader/follower path is exercised hard.
 */
static void run_worker(worker_context *context)
{
    uint32_t record;
    for (record = 0U; record < RECORDS_PER_THREAD; ++record) {
        uint8_t key[8];
        uint8_t value[8];
        encode_u32(key, context->worker);
        encode_u32(key + 4U, record);
        encode_u32(value, record);
        encode_u32(value + 4U, context->worker);
        const sdb_status put_status = sdb_kv_put(
            context->database,
            namespace_name,
            sizeof(namespace_name),
            key,
            sizeof(key),
            value,
            sizeof(value)
        );
        if (put_status != SDB_OK) {
            context->failed = true;
            context->fail_status = (int)put_status;
            context->fail_record = record;
            return;
        }
    }
}

#ifdef _WIN32
static DWORD WINAPI worker_entry(LPVOID argument)
{
    run_worker((worker_context *)argument);
    return 0U;
}
#else
static void *worker_entry(void *argument)
{
    worker_context *context = (worker_context *)argument;
    run_worker(context);
    if (context->finished != NULL) {
        atomic_fetch_add_explicit(context->finished, 1U, memory_order_release);
    }
    return NULL;
}
#endif

static void verify_all_present(sdb_database *database)
{
    uint32_t worker;
    for (worker = 0U; worker < THREAD_COUNT; ++worker) {
        uint32_t record;
        for (record = 0U; record < RECORDS_PER_THREAD; ++record) {
            uint8_t key[8];
            uint8_t expected[8];
            uint8_t actual[8];
            size_t actual_size = 0U;
            encode_u32(key, worker);
            encode_u32(key + 4U, record);
            encode_u32(expected, record);
            encode_u32(expected + 4U, worker);
            assert(sdb_kv_get(
                database, namespace_name, sizeof(namespace_name),
                key, sizeof(key), actual, sizeof(actual), &actual_size
            ) == SDB_OK);
            assert(actual_size == sizeof(expected));
            assert(memcmp(actual, expected, sizeof(expected)) == 0);
        }
    }
}

/*
 * Race a stream of auto-commit puts against repeated compaction on the SAME
 * handle. Compaction closes + swaps the pager; without the commit_in_flight
 * guard it could do so while a group-commit leader is fsyncing off the engine
 * mutex — use-after-destroy of commit_mutex/wal_file. With the guard, compact
 * returns SDB_E_BUSY during that window instead. Under TSan this must show
 * zero data races; functionally every compact call is OK or BUSY (never a
 * crash or other error) and all committed data survives.
 */
static void test_compact_races_commit(void)
{
    sdb_database_options options;
    sdb_database *database = NULL;
    worker_context contexts[THREAD_COUNT];
    uint32_t index;
    size_t compact_ok = 0U;
    size_t compact_busy = 0U;
#ifdef _WIN32
    HANDLE threads[THREAD_COUNT];
#else
    pthread_t threads[THREAD_COUNT];
    atomic_uint finished;
    atomic_init(&finished, 0U);
#endif
    (void)remove(compact_db_path);
    (void)remove(compact_wal_path);
    (void)remove(compact_lock_path);
    sdb_database_options_init(&options);
    assert(sdb_database_create(compact_db_path, &options, &database) == SDB_OK);
    for (index = 0U; index < THREAD_COUNT; ++index) {
        contexts[index].database = database;
        contexts[index].worker = index;
        contexts[index].failed = false;
        contexts[index].fail_status = 0;
        contexts[index].fail_record = 0U;
#ifndef _WIN32
        contexts[index].finished = &finished;
#endif
#ifdef _WIN32
        threads[index] = CreateThread(
            NULL, 0U, worker_entry, &contexts[index], 0U, NULL
        );
        assert(threads[index] != NULL);
#else
        assert(pthread_create(
            &threads[index], NULL, worker_entry, &contexts[index]
        ) == 0);
#endif
    }
    for (;;) {
        sdb_compact_result result;
        sdb_status status;
        bool workers_done;
#ifdef _WIN32
        workers_done = (WaitForMultipleObjects(
            THREAD_COUNT, threads, TRUE, 0) == WAIT_OBJECT_0);
#else
        workers_done = (atomic_load_explicit(
            &finished, memory_order_acquire) >= THREAD_COUNT);
#endif
        status = sdb_database_compact(database, &options, &result);
        assert(status == SDB_OK || status == SDB_E_BUSY);
        if (status == SDB_OK) {
            ++compact_ok;
        } else {
            ++compact_busy;
        }
        /*
         * Stop as soon as the guard is proven exercised (a BUSY was observed)
         * or the workers have all finished. Breaking on the first BUSY is what
         * this test needs — it does not require draining the whole worker
         * lifetime — and it keeps the loop from busy-spinning compaction for
         * the entire (very long under ASan/TSan) run, which otherwise pushed
         * the test past its per-test timeout. Yield between attempts so the
         * committing workers get the CPU and a commit reliably lands in flight
         * while a compaction is trying, instead of this thread starving them.
         */
        if (workers_done || compact_busy > 0U) {
            break;
        }
#ifdef _WIN32
        (void)SwitchToThread();
#else
        sched_yield();
#endif
    }
    for (index = 0U; index < THREAD_COUNT; ++index) {
#ifdef _WIN32
        assert(WaitForSingleObject(threads[index], INFINITE) == WAIT_OBJECT_0);
        assert(CloseHandle(threads[index]));
#else
        assert(pthread_join(threads[index], NULL) == 0);
#endif
        assert(!contexts[index].failed);
    }
    /*
     * The race window MUST have been exercised: at least one compact hit the
     * commit_in_flight guard and returned BUSY. Without this, a run where every
     * compact happened to return OK would pass trivially and prove NOTHING
     * about the guard (it would stay green even if commit_in_flight were
     * removed). The compact loop above runs continuously for the ENTIRE
     * lifetime of the 16 committing workers (it stops only once every worker
     * has exited), so it spans the whole ~1536-commit window regardless of how
     * the scheduler interleaves threads — a fixed round count could finish
     * before any worker got a commit in flight under a saturated CPU (e.g. an
     * ASan/TSan run sharing cores with other tests), leaving compact_busy at 0
     * and failing this assertion spuriously.
     */
    assert(compact_busy > 0U);
    /*
     * With no committer in flight, compaction succeeds — proving the guard
     * only defers during the durability window and never breaks compaction.
     */
    {
        sdb_compact_result result;
        const sdb_status compact_status =
            sdb_database_compact(database, &options, &result);
        if (compact_status != SDB_OK) {
            (void)fprintf(
                stderr, "post-race compact failed: %s (%d)\n",
                sdb_status_string(compact_status), (int)compact_status
            );
        }
        assert(compact_status == SDB_OK);
    }
    verify_all_present(database);
    assert(sdb_database_close(database) == SDB_OK);
    (void)remove(compact_db_path);
    (void)remove(compact_wal_path);
    (void)remove(compact_lock_path);
    (void)printf(
        "  compact_races_commit: ok (%u compacted, %u busy)\n",
        (unsigned)compact_ok, (unsigned)compact_busy
    );
}

/*
 * One shared "hot" key is committed HOT_KEY_WRITES times by a single thread
 * with strictly increasing values. Because only one thread writes it, the
 * last value written is deterministic; the reopened/queried value MUST equal
 * that last writer's value. A group-commit path that lost, reordered, or
 * double-applied a commit for this key would land on a stale value here.
 */
static void verify_last_writer(sdb_database *database)
{
    uint8_t key[8];
    uint8_t expected[8];
    uint8_t actual[8];
    size_t actual_size = 0U;
    encode_u32(key, 0xFFFFFFFFU);
    encode_u32(key + 4U, 0U);
    encode_u32(expected, HOT_KEY_WRITES - 1U);
    encode_u32(expected + 4U, 0xFFFFFFFFU);
    assert(sdb_kv_get(
        database, namespace_name, sizeof(namespace_name),
        key, sizeof(key), actual, sizeof(actual), &actual_size
    ) == SDB_OK);
    assert(actual_size == sizeof(expected));
    assert(memcmp(actual, expected, sizeof(expected)) == 0);
}

int main(void)
{
    sdb_database_options options;
    sdb_database *database = NULL;
    worker_context contexts[THREAD_COUNT];
    uint32_t index;
#ifdef _WIN32
    HANDLE threads[THREAD_COUNT];
#else
    pthread_t threads[THREAD_COUNT];
#endif

    test_compact_races_commit();

    (void)remove(database_path);
    (void)remove(wal_path);
    (void)remove(lock_path);
    sdb_database_options_init(&options);
    assert(sdb_database_create(database_path, &options, &database) == SDB_OK);

    for (index = 0U; index < THREAD_COUNT; ++index) {
        contexts[index].database = database;
        contexts[index].worker = index;
        contexts[index].failed = false;
        contexts[index].fail_status = 0;
        contexts[index].fail_record = 0U;
#ifndef _WIN32
        contexts[index].finished = NULL;
#endif
#ifdef _WIN32
        threads[index] = CreateThread(
            NULL, 0U, worker_entry, &contexts[index], 0U, NULL
        );
        assert(threads[index] != NULL);
#else
        assert(pthread_create(
            &threads[index], NULL, worker_entry, &contexts[index]
        ) == 0);
#endif
    }
    /*
     * While the 16 workers race to commit their own single-write keys, this
     * thread commits ONE shared key HOT_KEY_WRITES times with a strictly
     * increasing value, through the same auto-commit / group-commit path.
     * These puts are serialized by program order on this thread, so the last
     * value is deterministic and must win. The per-key sweeps above each
     * write a key exactly once, so they cannot expose a lost/reordered/
     * double-applied commit on a repeatedly-updated key — this can.
     */
    {
        uint32_t hot;
        for (hot = 0U; hot < HOT_KEY_WRITES; ++hot) {
            uint8_t key[8];
            uint8_t value[8];
            encode_u32(key, 0xFFFFFFFFU);
            encode_u32(key + 4U, 0U);
            encode_u32(value, hot);
            encode_u32(value + 4U, 0xFFFFFFFFU);
            assert(sdb_kv_put(
                database, namespace_name, sizeof(namespace_name),
                key, sizeof(key), value, sizeof(value)
            ) == SDB_OK);
        }
    }
    for (index = 0U; index < THREAD_COUNT; ++index) {
#ifdef _WIN32
        assert(WaitForSingleObject(threads[index], INFINITE) == WAIT_OBJECT_0);
        assert(CloseHandle(threads[index]));
#else
        assert(pthread_join(threads[index], NULL) == 0);
#endif
        if (contexts[index].failed) {
            (void)fprintf(
                stderr,
                "worker %u failed at record %u with status %d\n",
                (unsigned)index, (unsigned)contexts[index].fail_record,
                contexts[index].fail_status
            );
        }
        assert(!contexts[index].failed);
    }

    /*
     * Every committed record is visible in-session, and the repeatedly
     * committed hot key holds the last writer's value (no double-apply /
     * reorder / loss).
     */
    verify_all_present(database);
    verify_last_writer(database);
    assert(sdb_database_close(database) == SDB_OK);

    /*
     * And every committed record is durable across a close/reopen: the group
     * leader's fsync made the whole coalesced batch durable.
     */
    database = NULL;
    assert(sdb_database_open(database_path, &options, &database) == SDB_OK);
    verify_all_present(database);
    verify_last_writer(database);
    assert(sdb_database_close(database) == SDB_OK);

    (void)remove(database_path);
    (void)remove(wal_path);
    (void)remove(lock_path);
    (void)puts("group commit concurrency tests: ok");
    return 0;
}
