#include "shibadb_engine.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#endif

#define TEST_THREAD_COUNT 4U
#define TEST_RECORDS_PER_THREAD 50U
#define TEST_PROCESS_COUNT 4U
#define TEST_RECORDS_PER_PROCESS 10U

static const char *test_path = "test-engine-concurrency.tmp";
static const char *wal_path = "test-engine-concurrency.tmp.wal";
static const char *lock_path = "test-engine-concurrency.tmp.lock";
#ifndef _WIN32
static const char *symlink_path = "test-engine-concurrency-alias.tmp";
static const char *hardlink_path = "test-engine-concurrency-hardlink.tmp";
#endif
static const uint8_t test_namespace[] = {'s', 'o', 'a', 'k'};

typedef struct worker_context {
    sdb_database *database;
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

static void worker_run(worker_context *context)
{
    uint32_t record;
    for (record = 0U; record < TEST_RECORDS_PER_THREAD; ++record) {
        uint8_t key[8];
        uint8_t value[16];
        uint8_t output[16];
        size_t output_size = 0U;
        encode_u32(key, context->worker);
        encode_u32(key + 4U, record);
        (void)memset(value, (int)(context->worker + record), sizeof(value));
        if (sdb_kv_put(
                context->database,
                test_namespace,
                sizeof(test_namespace),
                key,
                sizeof(key),
                value,
                sizeof(value)
            ) != SDB_OK
            || sdb_kv_get(
                context->database,
                test_namespace,
                sizeof(test_namespace),
                key,
                sizeof(key),
                output,
                sizeof(output),
                &output_size
            ) != SDB_OK
            || output_size != sizeof(value)
            || memcmp(output, value, sizeof(value)) != 0) {
            context->failed = true;
            return;
        }
    }
}

#ifdef _WIN32
static DWORD WINAPI worker_entry(LPVOID argument)
{
    worker_run((worker_context *)argument);
    return 0U;
}
#else
static void *worker_entry(void *argument)
{
    worker_run((worker_context *)argument);
    return NULL;
}
#endif

static void verify_all(sdb_database *database)
{
    uint32_t worker;
    for (worker = 0U; worker < TEST_THREAD_COUNT; ++worker) {
        uint32_t record;
        for (record = 0U; record < TEST_RECORDS_PER_THREAD; ++record) {
            uint8_t key[8];
            uint8_t expected[16];
            uint8_t output[16];
            size_t output_size = 0U;
            encode_u32(key, worker);
            encode_u32(key + 4U, record);
            (void)memset(expected, (int)(worker + record), sizeof(expected));
            assert(sdb_kv_get(
                database,
                test_namespace,
                sizeof(test_namespace),
                key,
                sizeof(key),
                output,
                sizeof(output),
                &output_size
            ) == SDB_OK);
            assert(output_size == sizeof(expected));
            assert(memcmp(output, expected, sizeof(expected)) == 0);
        }
    }
}

#ifndef _WIN32
static void process_writer(
    const sdb_database_options *options, uint32_t process_index
)
{
    const struct timespec retry_delay = {0, 1000000L};
    sdb_database *database = NULL;
    sdb_status status;
    uint32_t attempts = 0U;
    uint32_t record;
    do {
        status = sdb_database_open(test_path, options, &database);
        if (status == SDB_E_BUSY) {
            (void)nanosleep(&retry_delay, NULL);
        }
        ++attempts;
    } while (status == SDB_E_BUSY && attempts < 10000U);
    if (status != SDB_OK) {
        _exit(2);
    }
    for (record = 0U; record < TEST_RECORDS_PER_PROCESS; ++record) {
        uint8_t key[9] = {'p', 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U};
        uint8_t value[8];
        encode_u32(key + 1U, process_index);
        encode_u32(key + 5U, record);
        encode_u32(value, process_index);
        encode_u32(value + 4U, record);
        if (sdb_kv_put(
                database,
                test_namespace,
                sizeof(test_namespace),
                key,
                sizeof(key),
                value,
                sizeof(value)
            ) != SDB_OK) {
            (void)sdb_database_close(database);
            _exit(3);
        }
    }
    _exit(sdb_database_close(database) == SDB_OK ? 0 : 4);
}

static void verify_process_records(sdb_database *database)
{
    uint32_t process_index;
    for (process_index = 0U;
         process_index < TEST_PROCESS_COUNT;
         ++process_index) {
        uint32_t record;
        for (record = 0U; record < TEST_RECORDS_PER_PROCESS; ++record) {
            uint8_t key[9] = {'p', 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U};
            uint8_t expected[8];
            uint8_t output[8];
            size_t output_size = 0U;
            encode_u32(key + 1U, process_index);
            encode_u32(key + 5U, record);
            encode_u32(expected, process_index);
            encode_u32(expected + 4U, record);
            assert(sdb_kv_get(
                database,
                test_namespace,
                sizeof(test_namespace),
                key,
                sizeof(key),
                output,
                sizeof(output),
                &output_size
            ) == SDB_OK);
            assert(output_size == sizeof(expected));
            assert(memcmp(output, expected, sizeof(expected)) == 0);
        }
    }
}
#endif

int main(void)
{
    sdb_database_options options;
    sdb_database *database = NULL;
    sdb_database *second = NULL;
    worker_context contexts[TEST_THREAD_COUNT];
    uint32_t index;
#ifdef _WIN32
    HANDLE threads[TEST_THREAD_COUNT];
#else
    pthread_t threads[TEST_THREAD_COUNT];
#endif

    (void)remove(wal_path);
    (void)remove(test_path);
    (void)remove(lock_path);
#ifndef _WIN32
    (void)remove(symlink_path);
    (void)remove(hardlink_path);
#endif
    sdb_database_options_init(&options);
    assert(sdb_database_create(test_path, &options, &database) == SDB_OK);

    assert(sdb_database_open(test_path, &options, &second) == SDB_E_BUSY);
    assert(second == NULL);
    assert(sdb_database_open(
        "./test-engine-concurrency.tmp", &options, &second
    ) == SDB_E_BUSY);
    assert(second == NULL);

#ifndef _WIN32
    assert(symlink(test_path, symlink_path) == 0);
    assert(sdb_database_open(
        symlink_path, &options, &second
    ) == SDB_E_BUSY);
    assert(second == NULL);
    assert(link(test_path, hardlink_path) == 0);
    assert(sdb_database_open(
        hardlink_path, &options, &second
    ) == SDB_E_INVALID_ARGUMENT);
    assert(second == NULL);
    assert(unlink(hardlink_path) == 0);
    {
        const pid_t child = fork();
        int child_status = 0;
        assert(child >= 0);
        if (child == 0) {
            sdb_database *child_database = NULL;
            const sdb_status status =
                sdb_database_open(test_path, &options, &child_database);
            _exit(status == SDB_E_BUSY && child_database == NULL ? 0 : 1);
        }
        assert(waitpid(child, &child_status, 0) == child);
        assert(WIFEXITED(child_status));
        assert(WEXITSTATUS(child_status) == 0);
    }
#endif

    for (index = 0U; index < TEST_THREAD_COUNT; ++index) {
        contexts[index].database = database;
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
    for (index = 0U; index < TEST_THREAD_COUNT; ++index) {
#ifdef _WIN32
        assert(WaitForSingleObject(threads[index], INFINITE) == WAIT_OBJECT_0);
        assert(CloseHandle(threads[index]));
#else
        assert(pthread_join(threads[index], NULL) == 0);
#endif
        assert(!contexts[index].failed);
    }
    verify_all(database);
    assert(sdb_database_close(database) == SDB_OK);

#ifndef _WIN32
    assert(sdb_database_open(
        symlink_path, &options, &second
    ) == SDB_OK);
    assert(sdb_database_close(second) == SDB_OK);
    second = NULL;
    {
        int ready[2];
        pid_t child;
        int child_status = 0;
        uint8_t marker = 0U;
        assert(pipe(ready) == 0);
        child = fork();
        assert(child >= 0);
        if (child == 0) {
            sdb_database *child_database = NULL;
            (void)close(ready[0]);
            if (sdb_database_open(
                    test_path, &options, &child_database
                ) != SDB_OK
                || write(ready[1], "x", 1U) != 1) {
                _exit(5);
            }
            for (;;) {
                pause();
            }
        }
        (void)close(ready[1]);
        assert(read(ready[0], &marker, 1U) == 1);
        assert(marker == (uint8_t)'x');
        (void)close(ready[0]);
        assert(kill(child, SIGKILL) == 0);
        assert(waitpid(child, &child_status, 0) == child);
        assert(WIFSIGNALED(child_status));
        assert(WTERMSIG(child_status) == SIGKILL);
        assert(sdb_database_open(test_path, &options, &second) == SDB_OK);
        assert(sdb_database_close(second) == SDB_OK);
        second = NULL;
    }
    {
        pid_t children[TEST_PROCESS_COUNT];
        for (index = 0U; index < TEST_PROCESS_COUNT; ++index) {
            children[index] = fork();
            assert(children[index] >= 0);
            if (children[index] == 0) {
                process_writer(&options, index);
            }
        }
        for (index = 0U; index < TEST_PROCESS_COUNT; ++index) {
            int child_status = 0;
            assert(waitpid(children[index], &child_status, 0)
                == children[index]);
            assert(WIFEXITED(child_status));
            assert(WEXITSTATUS(child_status) == 0);
        }
    }
#endif

    assert(sdb_database_open(test_path, &options, &database) == SDB_OK);
    verify_all(database);
#ifndef _WIN32
    verify_process_records(database);
#endif
    assert(sdb_database_close(database) == SDB_OK);

    (void)remove(wal_path);
    (void)remove(test_path);
    (void)remove(lock_path);
#ifndef _WIN32
    (void)remove(symlink_path);
    (void)remove(hardlink_path);
#endif
    (void)puts("engine concurrency tests: ok");
    return 0;
}
