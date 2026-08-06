#include "shibadb_engine.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif

#define THREAD_COUNT 4U
#define RECORDS_PER_THREAD 40U
#define SHARED_KEY_COUNT 32U
#define SHARED_KEY_MARKER 0xFFFFFFFFU

static const char *database_path = "test-public-transaction-concurrency.tmp";
static const char *wal_path = "test-public-transaction-concurrency.tmp.wal";
static const char *lock_path = "test-public-transaction-concurrency.tmp.lock";
static const uint8_t namespace_name[] = "threads";

typedef struct worker_context {
    sdb_transaction *transaction;
    uint32_t worker;
    bool failed;
} worker_context;

static void encode_u32(uint8_t output[4], uint32_t value)
{
    output[0] = (uint8_t)value;
    output[1] = (uint8_t)(value >> 8U);
    output[2] = (uint8_t)(value >> 16U);
    output[3] = (uint8_t)(value >> 24U);
}

static void run_worker(worker_context *context)
{
    uint32_t record;
    for (record = 0U; record < RECORDS_PER_THREAD; ++record) {
        uint8_t key[8];
        uint8_t value[8];
        uint8_t actual[8];
        size_t actual_size = 0U;
        encode_u32(key, context->worker);
        encode_u32(key + 4U, record);
        encode_u32(value, record);
        encode_u32(value + 4U, context->worker);
        if (sdb_transaction_kv_put(
                context->transaction,
                namespace_name,
                sizeof(namespace_name),
                key,
                sizeof(key),
                value,
                sizeof(value)
            ) != SDB_OK
            || sdb_transaction_kv_get(
                context->transaction,
                namespace_name,
                sizeof(namespace_name),
                key,
                sizeof(key),
                actual,
                sizeof(actual),
                &actual_size
            ) != SDB_OK
            || actual_size != sizeof(value)
            || memcmp(actual, value, sizeof(value)) != 0) {
            context->failed = true;
            return;
        }
    }
    /*
     * Now every thread hammers the SAME set of shared keys through the one
     * shared transaction — overlapping writes, not the disjoint per-worker
     * keys above. The value for each shared key is derived solely from the
     * key (worker-independent), so whichever thread writes last, the value is
     * identical and the post-condition is deterministic: after commit each
     * shared key must equal its key-derived value. This exercises the
     * transaction's internal serialization of concurrent writes to the same
     * key (a data race here would corrupt the tree or leave a torn value).
     */
    {
        uint32_t shared;
        for (shared = 0U; shared < SHARED_KEY_COUNT; ++shared) {
            uint8_t key[8];
            uint8_t value[8];
            uint8_t actual[8];
            size_t actual_size = 0U;
            encode_u32(key, SHARED_KEY_MARKER);
            encode_u32(key + 4U, shared);
            encode_u32(value, shared);
            encode_u32(value + 4U, SHARED_KEY_MARKER);
            if (sdb_transaction_kv_put(
                    context->transaction,
                    namespace_name,
                    sizeof(namespace_name),
                    key,
                    sizeof(key),
                    value,
                    sizeof(value)
                ) != SDB_OK
                || sdb_transaction_kv_get(
                    context->transaction,
                    namespace_name,
                    sizeof(namespace_name),
                    key,
                    sizeof(key),
                    actual,
                    sizeof(actual),
                    &actual_size
                ) != SDB_OK
                || actual_size != sizeof(value)
                || memcmp(actual, value, sizeof(value)) != 0) {
                context->failed = true;
                return;
            }
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
    run_worker((worker_context *)argument);
    return NULL;
}
#endif

int main(void)
{
    sdb_database_options options;
    sdb_database *database = NULL;
    sdb_transaction *transaction = NULL;
    worker_context contexts[THREAD_COUNT];
    uint32_t index;
#ifdef _WIN32
    HANDLE threads[THREAD_COUNT];
#else
    pthread_t threads[THREAD_COUNT];
#endif

    (void)remove(database_path);
    (void)remove(wal_path);
    (void)remove(lock_path);
    sdb_database_options_init(&options);
    assert(sdb_database_create(
        database_path, &options, &database
    ) == SDB_OK);
    assert(sdb_transaction_begin(database, &transaction) == SDB_OK);
    for (index = 0U; index < THREAD_COUNT; ++index) {
        contexts[index].transaction = transaction;
        contexts[index].worker = index;
        contexts[index].failed = false;
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
    for (index = 0U; index < THREAD_COUNT; ++index) {
#ifdef _WIN32
        assert(WaitForSingleObject(
            threads[index], INFINITE
        ) == WAIT_OBJECT_0);
        assert(CloseHandle(threads[index]));
#else
        assert(pthread_join(threads[index], NULL) == 0);
#endif
        assert(!contexts[index].failed);
    }
    assert(sdb_transaction_commit(transaction) == SDB_OK);
    assert(sdb_transaction_close(transaction) == SDB_OK);
    for (index = 0U; index < THREAD_COUNT; ++index) {
        uint32_t record;
        for (record = 0U; record < RECORDS_PER_THREAD; ++record) {
            uint8_t key[8];
            uint8_t expected[8];
            uint8_t actual[8];
            size_t actual_size = 0U;
            encode_u32(key, index);
            encode_u32(key + 4U, record);
            encode_u32(expected, record);
            encode_u32(expected + 4U, index);
            assert(sdb_kv_get(
                database, namespace_name, sizeof(namespace_name),
                key, sizeof(key), actual, sizeof(actual), &actual_size
            ) == SDB_OK);
            assert(actual_size == sizeof(expected));
            assert(memcmp(actual, expected, sizeof(expected)) == 0);
        }
    }
    /*
     * The overlapping shared keys have a deterministic committed state: every
     * thread wrote the same key-derived value, so each shared key must read
     * back exactly that value. A lost or torn write from the concurrent
     * same-key updates would break this.
     */
    {
        uint32_t shared;
        for (shared = 0U; shared < SHARED_KEY_COUNT; ++shared) {
            uint8_t key[8];
            uint8_t expected[8];
            uint8_t actual[8];
            size_t actual_size = 0U;
            encode_u32(key, SHARED_KEY_MARKER);
            encode_u32(key + 4U, shared);
            encode_u32(expected, shared);
            encode_u32(expected + 4U, SHARED_KEY_MARKER);
            assert(sdb_kv_get(
                database, namespace_name, sizeof(namespace_name),
                key, sizeof(key), actual, sizeof(actual), &actual_size
            ) == SDB_OK);
            assert(actual_size == sizeof(expected));
            assert(memcmp(actual, expected, sizeof(expected)) == 0);
        }
    }
    assert(sdb_database_close(database) == SDB_OK);
    (void)remove(database_path);
    (void)remove(wal_path);
    (void)remove(lock_path);
    (void)puts("public transaction concurrency tests: ok");
    return 0;
}
