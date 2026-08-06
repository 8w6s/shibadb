/*
 * Mixed-workload concurrency stress. A single shared sdb_database handle is
 * driven simultaneously by four kinds of thread:
 *   - writers      : auto-commit puts of deterministic, write-once keys
 *   - readers      : gets of those keys, racing the writers
 *   - a backup loop : sdb_database_backup in a tight loop
 *   - a compact loop: sdb_database_compact in a tight loop
 *
 * Every op runs under the engine mutex, so the only genuinely parallel window
 * is the R4 group-commit DURABILITY phase (the leader fsyncs off the mutex
 * while other threads may lock it). test_group_commit covers compact vs that
 * window; this adds readers and backup racing it too. It is a data-race gate
 * (run under ThreadSanitizer) AND a correctness check:
 *   - a reader that finds a key MUST see that key's one true value (no torn,
 *     stale-from-another-key, or garbage read) — never a wrong value;
 *   - backup and compact each return only SDB_OK or SDB_E_BUSY (the
 *     commit_in_flight guard), never a crash or spurious error;
 *   - after every thread joins, all committed data is present and survives a
 *     close/reopen.
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

#define WRITER_THREADS 8U
#define READER_THREADS 8U
#define RECORDS_PER_WRITER 128U
#define READ_ITERATIONS 3000U
#define BACKUP_ROUNDS 120U
#define COMPACT_ROUNDS 120U

static const char *database_path = "test-mixed.tmp";
static const char *wal_path = "test-mixed.tmp.wal";
static const char *lock_path = "test-mixed.tmp.lock";
static const char *backup_path = "test-mixed-backup.tmp";
static const char *backup_wal_path = "test-mixed-backup.tmp.wal";
static const char *backup_lock_path = "test-mixed-backup.tmp.lock";
static const uint8_t namespace_name[] = "mix";

static void encode_u32(uint8_t output[4], uint32_t value)
{
    output[0] = (uint8_t)value;
    output[1] = (uint8_t)(value >> 8U);
    output[2] = (uint8_t)(value >> 16U);
    output[3] = (uint8_t)(value >> 24U);
}

/*
 * Key (worker, record) is written exactly once with value (record, worker),
 * so any successful read of it has exactly one correct answer.
 */
static void make_key(uint8_t key[8], uint32_t worker, uint32_t record)
{
    encode_u32(key, worker);
    encode_u32(key + 4U, record);
}

static void make_value(uint8_t value[8], uint32_t worker, uint32_t record)
{
    encode_u32(value, record);
    encode_u32(value + 4U, worker);
}

typedef struct writer_context {
    sdb_database *database;
    uint32_t worker;
    bool failed;
    int fail_status;
} writer_context;

typedef struct reader_context {
    sdb_database *database;
    uint32_t seed;
    bool failed;
    size_t found;
    size_t missing;
} reader_context;

typedef struct loop_context {
    sdb_database *database;
    const sdb_database_options *options;
    size_t rounds;
    size_t ok;
    size_t busy;
    bool failed;
    int fail_status;
} loop_context;

static void run_writer(writer_context *context)
{
    uint32_t record;
    for (record = 0U; record < RECORDS_PER_WRITER; ++record) {
        uint8_t key[8];
        uint8_t value[8];
        sdb_status status;
        make_key(key, context->worker, record);
        make_value(value, context->worker, record);
        status = sdb_kv_put(
            context->database, namespace_name, sizeof(namespace_name),
            key, sizeof(key), value, sizeof(value)
        );
        if (status != SDB_OK) {
            context->failed = true;
            context->fail_status = (int)status;
            return;
        }
    }
}

/*
 * A cheap deterministic per-thread PRNG (xorshift) so readers pick varied
 * keys without touching any shared state (no rand() lock, no data race).
 */
static uint32_t next_random(uint32_t *state)
{
    uint32_t x = *state;
    x ^= x << 13U;
    x ^= x >> 17U;
    x ^= x << 5U;
    *state = x;
    return x;
}

static void run_reader(reader_context *context)
{
    uint32_t iteration;
    uint32_t state = context->seed | 1U;
    for (iteration = 0U; iteration < READ_ITERATIONS; ++iteration) {
        uint8_t key[8];
        uint8_t expected[8];
        uint8_t actual[8];
        size_t actual_size = 0U;
        const uint32_t worker = next_random(&state) % WRITER_THREADS;
        const uint32_t record = next_random(&state) % RECORDS_PER_WRITER;
        sdb_status status;
        make_key(key, worker, record);
        make_value(expected, worker, record);
        status = sdb_kv_get(
            context->database, namespace_name, sizeof(namespace_name),
            key, sizeof(key), actual, sizeof(actual), &actual_size
        );
        if (status == SDB_OK) {
            /*
             * Found: it MUST be this key's one true value. A torn write, a
             * value bled from another key, or garbage all fail here.
             */
            if (actual_size != sizeof(expected)
                || memcmp(actual, expected, sizeof(expected)) != 0) {
                context->failed = true;
                return;
            }
            ++context->found;
        } else if (status == SDB_E_NOT_FOUND) {
            /* Not yet written by its writer — legitimate during the race. */
            ++context->missing;
        } else {
            context->failed = true;
            return;
        }
    }
}

static void run_backup_loop(loop_context *context)
{
    size_t round;
    for (round = 0U; round < context->rounds; ++round) {
        sdb_backup_result result;
        const sdb_status status =
            sdb_database_backup(context->database, backup_path, true, &result);
        if (status == SDB_OK) {
            ++context->ok;
        } else if (status == SDB_E_BUSY) {
            ++context->busy;
        } else {
            context->failed = true;
            context->fail_status = (int)status;
            return;
        }
    }
}

static void run_compact_loop(loop_context *context)
{
    size_t round;
    for (round = 0U; round < context->rounds; ++round) {
        sdb_compact_result result;
        const sdb_status status =
            sdb_database_compact(context->database, context->options, &result);
        if (status == SDB_OK) {
            ++context->ok;
        } else if (status == SDB_E_BUSY) {
            ++context->busy;
        } else {
            context->failed = true;
            context->fail_status = (int)status;
            return;
        }
    }
}

#ifdef _WIN32
#define THREAD_RET DWORD WINAPI
#define THREAD_HANDLE HANDLE
#else
#define THREAD_RET void *
#define THREAD_HANDLE pthread_t
#endif

static THREAD_RET writer_entry(void *argument)
{
    run_writer((writer_context *)argument);
#ifdef _WIN32
    return 0U;
#else
    return NULL;
#endif
}

static THREAD_RET reader_entry(void *argument)
{
    run_reader((reader_context *)argument);
#ifdef _WIN32
    return 0U;
#else
    return NULL;
#endif
}

static THREAD_RET backup_entry(void *argument)
{
    run_backup_loop((loop_context *)argument);
#ifdef _WIN32
    return 0U;
#else
    return NULL;
#endif
}

static THREAD_RET compact_entry(void *argument)
{
    run_compact_loop((loop_context *)argument);
#ifdef _WIN32
    return 0U;
#else
    return NULL;
#endif
}

#ifdef _WIN32
static THREAD_HANDLE spawn(LPTHREAD_START_ROUTINE fn, void *arg)
{
    THREAD_HANDLE handle = CreateThread(NULL, 0U, fn, arg, 0U, NULL);
    assert(handle != NULL);
    return handle;
}
static void join(THREAD_HANDLE handle)
{
    assert(WaitForSingleObject(handle, INFINITE) == WAIT_OBJECT_0);
    assert(CloseHandle(handle));
}
#else
static THREAD_HANDLE spawn(void *(*fn)(void *), void *arg)
{
    THREAD_HANDLE handle;
    assert(pthread_create(&handle, NULL, fn, arg) == 0);
    return handle;
}
static void join(THREAD_HANDLE handle)
{
    assert(pthread_join(handle, NULL) == 0);
}
#endif

static void verify_all_present(sdb_database *database)
{
    uint32_t worker;
    for (worker = 0U; worker < WRITER_THREADS; ++worker) {
        uint32_t record;
        for (record = 0U; record < RECORDS_PER_WRITER; ++record) {
            uint8_t key[8];
            uint8_t expected[8];
            uint8_t actual[8];
            size_t actual_size = 0U;
            make_key(key, worker, record);
            make_value(expected, worker, record);
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
    writer_context writers[WRITER_THREADS];
    reader_context readers[READER_THREADS];
    loop_context backup_ctx;
    loop_context compact_ctx;
    THREAD_HANDLE writer_threads[WRITER_THREADS];
    THREAD_HANDLE reader_threads[READER_THREADS];
    THREAD_HANDLE backup_thread;
    THREAD_HANDLE compact_thread;
    uint32_t index;
    size_t total_found = 0U;
    size_t total_missing = 0U;

    (void)remove(database_path);
    (void)remove(wal_path);
    (void)remove(lock_path);
    (void)remove(backup_path);
    (void)remove(backup_wal_path);
    (void)remove(backup_lock_path);

    sdb_database_options_init(&options);
    assert(sdb_database_create(database_path, &options, &database) == SDB_OK);

    backup_ctx.database = database;
    backup_ctx.options = &options;
    backup_ctx.rounds = BACKUP_ROUNDS;
    backup_ctx.ok = 0U;
    backup_ctx.busy = 0U;
    backup_ctx.failed = false;
    backup_ctx.fail_status = 0;
    compact_ctx.database = database;
    compact_ctx.options = &options;
    compact_ctx.rounds = COMPACT_ROUNDS;
    compact_ctx.ok = 0U;
    compact_ctx.busy = 0U;
    compact_ctx.failed = false;
    compact_ctx.fail_status = 0;

    /*
     * Launch every kind of worker together so readers, backup and compact all
     * overlap the writers' commit/durability windows.
     */
    for (index = 0U; index < WRITER_THREADS; ++index) {
        writers[index].database = database;
        writers[index].worker = index;
        writers[index].failed = false;
        writers[index].fail_status = 0;
        writer_threads[index] = spawn(writer_entry, &writers[index]);
    }
    for (index = 0U; index < READER_THREADS; ++index) {
        readers[index].database = database;
        readers[index].seed = 0x9e3779b9U + index * 0x85ebca6bU;
        readers[index].failed = false;
        readers[index].found = 0U;
        readers[index].missing = 0U;
        reader_threads[index] = spawn(reader_entry, &readers[index]);
    }
    backup_thread = spawn(backup_entry, &backup_ctx);
    compact_thread = spawn(compact_entry, &compact_ctx);

    for (index = 0U; index < WRITER_THREADS; ++index) {
        join(writer_threads[index]);
        assert(!writers[index].failed);
    }
    for (index = 0U; index < READER_THREADS; ++index) {
        join(reader_threads[index]);
        assert(!readers[index].failed);
        total_found += readers[index].found;
        total_missing += readers[index].missing;
    }
    join(backup_thread);
    assert(!backup_ctx.failed);
    join(compact_thread);
    assert(!compact_ctx.failed);

    /*
     * Both maintenance loops must have actually run against a live committer:
     * at least some rounds should have hit the commit_in_flight guard (BUSY),
     * otherwise the race window was never exercised and the guard is untested
     * here. Compact BUSY is reliable while writers are mid-commit.
     */
    assert(compact_ctx.busy > 0U);

    /* All committed data present in-session, and durable across reopen. */
    verify_all_present(database);
    assert(sdb_database_close(database) == SDB_OK);
    database = NULL;
    assert(sdb_database_open(database_path, &options, &database) == SDB_OK);
    verify_all_present(database);
    assert(sdb_database_close(database) == SDB_OK);

    (void)remove(database_path);
    (void)remove(wal_path);
    (void)remove(lock_path);
    (void)remove(backup_path);
    (void)remove(backup_wal_path);
    (void)remove(backup_lock_path);

    (void)printf(
        "mixed workload: ok (reads found=%u missing=%u, backup ok=%u busy=%u, "
        "compact ok=%u busy=%u)\n",
        (unsigned)total_found, (unsigned)total_missing,
        (unsigned)backup_ctx.ok, (unsigned)backup_ctx.busy,
        (unsigned)compact_ctx.ok, (unsigned)compact_ctx.busy
    );
    return 0;
}
