/*
 * BUG#3 regression — sdb_transaction_begin must reject with SDB_E_BUSY while a
 * group-commit leader is inside its off-mutex durability window
 * (commit_in_flight != 0), exactly as close, compact, and backup already do.
 *
 * Before the fix, begin's guard checked only callback_depth and
 * active_transaction. An auto-commit leader never sets active_transaction: it
 * only bumps commit_in_flight, drops the engine mutex, and drives
 * sdb_pager_commit_durable (the leader fsync) off the mutex. In that window
 * begin's guard let a caller slip in — its sdb_engine_mutation_begin, and the
 * checkpoint its own commit then triggers (WAL ftruncate + superblock rewrite),
 * touched the very WAL / pager state the leader was still driving to durability
 * off the mutex. That is a data race and can tear the WAL / superblock and lose
 * an already-acknowledged commit.
 *
 * The harness mirrors test_compact_races_commit: WORKER_COUNT auto-commit
 * worker threads keep a leader in flight on ONE shared handle while a single
 * begin-thread hammers begin/put/commit. Using exactly ONE begin-thread is
 * deliberate — no other thread ever holds active_transaction, so every
 * SDB_E_BUSY that begin observes can ONLY originate from commit_in_flight.
 * begin_busy > 0 therefore proves the race window was actually entered (an
 * all-OK run would prove nothing, staying green even with the guard removed).
 *
 * Run under ThreadSanitizer: this is RED (data race report) on the unfixed
 * guard and GREEN once begin also checks commit_in_flight. Every worker commit
 * that returned SDB_OK must additionally survive a close/reopen.
 */
#include "shibadb_engine.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif

#define WORKER_COUNT 8U
#define RECORDS_PER_WORKER 300U
#define BEGIN_ATTEMPTS 3000U

static const char *database_path = "test-txn-begin-commit-race.tmp";
static const char *wal_path = "test-txn-begin-commit-race.tmp.wal";
static const char *lock_path = "test-txn-begin-commit-race.tmp.lock";
static const uint8_t namespace_name[] = "race";
static const uint32_t begin_key_marker = 0xAAAAAAAAU;

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
} worker_context;

typedef struct begin_context {
    sdb_database *database;
    bool failed;
    int fail_status;
    size_t begin_ok;
    size_t begin_busy;
} begin_context;

/*
 * Each worker commits RECORDS_PER_WORKER disjoint keys through the auto-commit
 * API. A put returns SDB_E_BUSY only while the begin-thread is holding its
 * short-lived transaction, so BUSY is retried; any other status is a failure.
 */
static void run_worker(worker_context *context)
{
    uint32_t record;
    for (record = 0U; record < RECORDS_PER_WORKER; ++record) {
        uint8_t key[8];
        uint8_t value[8];
        sdb_status put_status;
        encode_u32(key, context->worker);
        encode_u32(key + 4U, record);
        encode_u32(value, record);
        encode_u32(value + 4U, context->worker);
        do {
            put_status = sdb_kv_put(
                context->database,
                namespace_name,
                sizeof(namespace_name),
                key,
                sizeof(key),
                value,
                sizeof(value)
            );
        } while (put_status == SDB_E_BUSY);
        if (put_status != SDB_OK) {
            context->failed = true;
            context->fail_status = (int)put_status;
            context->fail_record = record;
            return;
        }
    }
}

/*
 * A single thread repeatedly opens a transaction, writes one key, and commits.
 * When begin returns SDB_E_BUSY it can only be because a group-commit leader is
 * in its off-mutex durability window (no other thread holds active_transaction)
 * — that is the exact window the guard must cover, so the count is recorded.
 */
static void run_begin(begin_context *context)
{
    uint32_t attempt;
    for (attempt = 0U; attempt < BEGIN_ATTEMPTS; ++attempt) {
        sdb_transaction *transaction = NULL;
        uint8_t key[8];
        uint8_t value[8];
        sdb_status status;
        const sdb_status begin_status =
            sdb_transaction_begin(context->database, &transaction);
        if (begin_status == SDB_E_BUSY) {
            ++context->begin_busy;
            continue;
        }
        if (begin_status != SDB_OK) {
            context->failed = true;
            context->fail_status = (int)begin_status;
            return;
        }
        ++context->begin_ok;
        encode_u32(key, begin_key_marker);
        encode_u32(key + 4U, attempt);
        encode_u32(value, attempt);
        encode_u32(value + 4U, begin_key_marker);
        status = sdb_transaction_kv_put(
            transaction,
            namespace_name,
            sizeof(namespace_name),
            key,
            sizeof(key),
            value,
            sizeof(value)
        );
        if (status == SDB_OK) {
            status = sdb_transaction_commit(transaction);
        } else {
            (void)sdb_transaction_rollback(transaction);
        }
        (void)sdb_transaction_close(transaction);
        if (status != SDB_OK) {
            context->failed = true;
            context->fail_status = (int)status;
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
static DWORD WINAPI begin_entry(LPVOID argument)
{
    run_begin((begin_context *)argument);
    return 0U;
}
#else
static void *worker_entry(void *argument)
{
    run_worker((worker_context *)argument);
    return NULL;
}
static void *begin_entry(void *argument)
{
    run_begin((begin_context *)argument);
    return NULL;
}
#endif

static void verify_all_present(sdb_database *database)
{
    uint32_t worker;
    for (worker = 0U; worker < WORKER_COUNT; ++worker) {
        uint32_t record;
        for (record = 0U; record < RECORDS_PER_WORKER; ++record) {
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

int main(void)
{
    sdb_database_options options;
    sdb_database *database = NULL;
    worker_context contexts[WORKER_COUNT];
    begin_context begin;
    uint32_t index;
#ifdef _WIN32
    HANDLE threads[WORKER_COUNT];
    HANDLE begin_thread;
#else
    pthread_t threads[WORKER_COUNT];
    pthread_t begin_thread;
#endif

    (void)remove(database_path);
    (void)remove(wal_path);
    (void)remove(lock_path);
    sdb_database_options_init(&options);
    assert(sdb_database_create(database_path, &options, &database) == SDB_OK);

    begin.database = database;
    begin.failed = false;
    begin.fail_status = 0;
    begin.begin_ok = 0U;
    begin.begin_busy = 0U;
    for (index = 0U; index < WORKER_COUNT; ++index) {
        contexts[index].database = database;
        contexts[index].worker = index;
        contexts[index].failed = false;
        contexts[index].fail_status = 0;
        contexts[index].fail_record = 0U;
    }

#ifdef _WIN32
    for (index = 0U; index < WORKER_COUNT; ++index) {
        threads[index] = CreateThread(
            NULL, 0U, worker_entry, &contexts[index], 0U, NULL
        );
        assert(threads[index] != NULL);
    }
    begin_thread = CreateThread(NULL, 0U, begin_entry, &begin, 0U, NULL);
    assert(begin_thread != NULL);
    for (index = 0U; index < WORKER_COUNT; ++index) {
        assert(WaitForSingleObject(threads[index], INFINITE) == WAIT_OBJECT_0);
        assert(CloseHandle(threads[index]));
    }
    assert(WaitForSingleObject(begin_thread, INFINITE) == WAIT_OBJECT_0);
    assert(CloseHandle(begin_thread));
#else
    for (index = 0U; index < WORKER_COUNT; ++index) {
        assert(pthread_create(
            &threads[index], NULL, worker_entry, &contexts[index]
        ) == 0);
    }
    assert(pthread_create(&begin_thread, NULL, begin_entry, &begin) == 0);
    for (index = 0U; index < WORKER_COUNT; ++index) {
        assert(pthread_join(threads[index], NULL) == 0);
    }
    assert(pthread_join(begin_thread, NULL) == 0);
#endif

    for (index = 0U; index < WORKER_COUNT; ++index) {
        if (contexts[index].failed) {
            (void)fprintf(
                stderr, "worker %u failed at record %u: %s (%d)\n",
                (unsigned)index, (unsigned)contexts[index].fail_record,
                sdb_status_string((sdb_status)contexts[index].fail_status),
                contexts[index].fail_status
            );
        }
        assert(!contexts[index].failed);
    }
    if (begin.failed) {
        (void)fprintf(
            stderr, "begin-thread failed: %s (%d)\n",
            sdb_status_string((sdb_status)begin.fail_status),
            begin.fail_status
        );
    }
    assert(!begin.failed);

    /*
     * The race window MUST have been exercised: with a single begin-thread the
     * only source of SDB_E_BUSY from begin is a leader in its off-mutex
     * durability window. begin_busy == 0 would mean the window was never hit
     * and the test proves nothing about the guard. Before the fix this assert
     * may not even be reached — ThreadSanitizer aborts on the data race first.
     */
    assert(begin.begin_busy > 0U);

    /*
     * With every committer drained, begin must succeed again — proving the
     * guard only defers during the durability window and never rejects a
     * transaction permanently. Without this a guard that always returned BUSY
     * would pass the race check trivially yet be badly broken.
     */
    {
        sdb_transaction *transaction = NULL;
        uint8_t key[8];
        uint8_t value[8];
        encode_u32(key, begin_key_marker);
        encode_u32(key + 4U, BEGIN_ATTEMPTS);
        encode_u32(value, 0U);
        encode_u32(value + 4U, begin_key_marker);
        assert(sdb_transaction_begin(database, &transaction) == SDB_OK);
        assert(sdb_transaction_kv_put(
            transaction, namespace_name, sizeof(namespace_name),
            key, sizeof(key), value, sizeof(value)
        ) == SDB_OK);
        assert(sdb_transaction_commit(transaction) == SDB_OK);
        assert(sdb_transaction_close(transaction) == SDB_OK);
    }

    verify_all_present(database);
    assert(sdb_database_close(database) == SDB_OK);

    assert(sdb_database_open(database_path, &options, &database) == SDB_OK);
    verify_all_present(database);
    assert(sdb_database_close(database) == SDB_OK);

    (void)remove(database_path);
    (void)remove(wal_path);
    (void)remove(lock_path);
    (void)printf(
        "  txn_begin_commit_race: ok (%u begin commits, %u begin busy)\n",
        (unsigned)begin.begin_ok, (unsigned)begin.begin_busy
    );
    return 0;
}
