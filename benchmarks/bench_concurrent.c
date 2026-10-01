/*
 * Concurrent auto-commit throughput — the R4 group-commit workload.
 *
 * bench_kv measures a SINGLE thread, where auto-commit is fsync-bound (~1
 * durable commit per fsync). That does not represent a real shop/VPS load of
 * many clients committing at once, which is exactly what R4 group commit is
 * for: many threads on one handle, the leader coalescing their fsyncs. This
 * harness spawns SDB_BENCH_THREADS threads each doing SDB_BENCH_OPS_PER_THREAD
 * independent auto-commit puts against ONE shared handle, measures aggregate
 * durable-commits/s, and then REOPENS and verifies every record survived with
 * the exact value — throughput numbers are meaningless if data was lost, so the
 * verify is part of the benchmark, not an afterthought.
 *
 * Env: SDB_BENCH_THREADS (default 16), SDB_BENCH_OPS_PER_THREAD (default 500),
 *      SDB_BENCH_VALUE_SIZE (default 100), SDB_BENCH_PLAINTEXT (default off).
 */
#include "shibadb_engine.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif

static const char *bench_path = "bench-concurrent.tmp";
static const char *bench_wal_path = "bench-concurrent.tmp.wal";
static const char *bench_lock_path = "bench-concurrent.tmp.lock";
static const uint8_t bench_ns[] = {'b', 'e', 'n', 'c', 0U};
static const uint8_t bench_password[] = "benchmark-password";

static uint64_t bench_now_ns(void)
{
#ifdef _WIN32
    LARGE_INTEGER freq;
    LARGE_INTEGER count;
    (void)QueryPerformanceFrequency(&freq);
    (void)QueryPerformanceCounter(&count);
    return (uint64_t)(count.QuadPart / freq.QuadPart) * UINT64_C(1000000000)
        + (uint64_t)(count.QuadPart % freq.QuadPart) * UINT64_C(1000000000)
            / (uint64_t)freq.QuadPart;
#else
    struct timespec ts;
    (void)clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
#endif
}

static uint64_t bench_env_u64(const char *name, uint64_t fallback)
{
    const char *text = getenv(name);
    char *end = NULL;
    unsigned long long value;
    if (text == NULL || text[0] == '\0') {
        return fallback;
    }
    value = strtoull(text, &end, 10);
    if (end == text || *end != '\0' || value == 0ULL) {
        return fallback;
    }
    return (uint64_t)value;
}

/* Key layout: [worker:4 big-endian][op:4 big-endian] — globally unique. */
static void bench_make_key(uint32_t worker, uint32_t op, uint8_t key[8])
{
    key[0] = (uint8_t)(worker >> 24U);
    key[1] = (uint8_t)(worker >> 16U);
    key[2] = (uint8_t)(worker >> 8U);
    key[3] = (uint8_t)worker;
    key[4] = (uint8_t)(op >> 24U);
    key[5] = (uint8_t)(op >> 16U);
    key[6] = (uint8_t)(op >> 8U);
    key[7] = (uint8_t)op;
}

typedef struct worker_ctx {
    sdb_database *database;
    uint32_t worker;
    uint32_t ops;
    size_t value_size;
    const uint8_t *value;
    bool failed;
    sdb_status fail_status;
} worker_ctx;

static void run_worker(worker_ctx *c)
{
    uint32_t op;
    for (op = 0U; op < c->ops; ++op) {
        uint8_t key[8];
        sdb_status status;
        bench_make_key(c->worker, op, key);
        status = sdb_kv_put(
            c->database, bench_ns, sizeof(bench_ns), key, sizeof(key),
            c->value, c->value_size
        );
        if (status != SDB_OK) {
            c->failed = true;
            c->fail_status = status;
            return;
        }
    }
}

#ifdef _WIN32
static DWORD WINAPI worker_entry(LPVOID arg)
{
    run_worker((worker_ctx *)arg);
    return 0U;
}
#else
static void *worker_entry(void *arg)
{
    run_worker((worker_ctx *)arg);
    return NULL;
}
#endif

static void bench_cleanup(void)
{
    (void)remove(bench_path);
    (void)remove(bench_wal_path);
    (void)remove(bench_lock_path);
}

int main(void)
{
    const uint32_t threads = (uint32_t)bench_env_u64("SDB_BENCH_THREADS", 16U);
    const uint32_t ops_per = (uint32_t)bench_env_u64("SDB_BENCH_OPS_PER_THREAD", 500U);
    const size_t value_size = (size_t)bench_env_u64("SDB_BENCH_VALUE_SIZE", 100U);
    const bool plaintext =
        getenv("SDB_BENCH_PLAINTEXT") != NULL
        && strcmp(getenv("SDB_BENCH_PLAINTEXT"), "0") != 0;
    sdb_database_options options;
    sdb_database *database = NULL;
    /*
     * Every allocation starts NULL and every error path funnels through the
     * `fail` cleanup below. The earlier version returned 1 straight out of
     * main on nine different paths, leaking value/readback/ctx and the thread
     * array each time (cppcheck: memleak at the OOM return). The OS reclaims it
     * anyway for a short-lived benchmark, but an error path that leaks is an
     * error path whose leak report is indistinguishable from a real one -- and
     * this harness is also used under ASan, where the noise matters.
     */
    uint8_t *value = NULL;
    uint8_t *readback = NULL;
    worker_ctx *ctx = NULL;
    uint64_t start;
    uint64_t end;
    uint64_t total_ops;
    uint32_t w;
    uint32_t spawned = 0U;
    int rc = 1;   /* set to 0 only on the fully verified success path */
    sdb_status status;
#ifdef _WIN32
    HANDLE *hthreads = NULL;
#else
    pthread_t *pthreads = NULL;
#endif

    value = (uint8_t *)malloc(value_size == 0U ? 1U : value_size);
    readback = (uint8_t *)malloc(value_size == 0U ? 1U : value_size);
    ctx = (worker_ctx *)malloc((size_t)threads * sizeof(*ctx));
#ifdef _WIN32
    hthreads = (HANDLE *)malloc((size_t)threads * sizeof(*hthreads));
    if (value == NULL || readback == NULL || ctx == NULL || hthreads == NULL) {
#else
    pthreads = (pthread_t *)malloc((size_t)threads * sizeof(*pthreads));
    if (value == NULL || readback == NULL || ctx == NULL || pthreads == NULL) {
#endif
        (void)fprintf(stderr, "bench: out of memory\n");
        goto cleanup;
    }
    (void)memset(value, 0xA5, value_size);

    bench_cleanup();
    sdb_database_options_init(&options);
    options.kdf_iterations = SDB_MIN_KDF_ITERATIONS;
    if (!plaintext) {
        options.password = bench_password;
        options.password_size = sizeof(bench_password) - 1U;
    }
    status = sdb_database_create(bench_path, &options, &database);
    if (status != SDB_OK) {
        (void)fprintf(stderr, "create failed: %s\n", sdb_status_string(status));
        goto cleanup;
    }

    total_ops = (uint64_t)threads * (uint64_t)ops_per;
    (void)printf(
        "shibadb concurrent benchmark: mode=%s threads=%u ops/thread=%u "
        "value=%zuB total=%llu\n",
        plaintext ? "plaintext" : "encrypted",
        threads, ops_per, value_size, (unsigned long long)total_ops
    );

    for (w = 0U; w < threads; ++w) {
        ctx[w].database = database;
        ctx[w].worker = w;
        ctx[w].ops = ops_per;
        ctx[w].value_size = value_size;
        ctx[w].value = value;
        ctx[w].failed = false;
        ctx[w].fail_status = SDB_OK;
    }

    start = bench_now_ns();
    for (w = 0U; w < threads; ++w) {
#ifdef _WIN32
        hthreads[w] = CreateThread(NULL, 0U, worker_entry, &ctx[w], 0U, NULL);
        if (hthreads[w] == NULL) {
            (void)fprintf(stderr, "CreateThread failed\n");
            goto fail_spawned;
        }
#else
        if (pthread_create(&pthreads[w], NULL, worker_entry, &ctx[w]) != 0) {
            (void)fprintf(stderr, "pthread_create failed\n");
            goto fail_spawned;
        }
#endif
        spawned = (uint32_t)(w + 1U);
    }
    for (w = 0U; w < threads; ++w) {
#ifdef _WIN32
        (void)WaitForSingleObject(hthreads[w], INFINITE);
        (void)CloseHandle(hthreads[w]);
#else
        (void)pthread_join(pthreads[w], NULL);
#endif
    }
    spawned = 0U;   /* all reaped: later error paths have no threads to join */
    end = bench_now_ns();

    for (w = 0U; w < threads; ++w) {
        if (ctx[w].failed) {
            (void)fprintf(
                stderr, "worker %u failed: %s\n", w,
                sdb_status_string(ctx[w].fail_status)
            );
            goto cleanup;
        }
    }

    {
        const double secs = (double)(end - start) / 1e9;
        (void)printf(
            "  concurrent put   %9.0f durable-commits/s  (%llu commits, "
            "%u threads, %.3fs wall)\n",
            secs > 0.0 ? (double)total_ops / secs : 0.0,
            (unsigned long long)total_ops, threads, secs
        );
    }

    /*
     * Durability + correctness gate: reopen and verify every committed record
     * survived with its exact value. Throughput is only meaningful if nothing
     * was lost or corrupted under the concurrent group-commit path.
     */
    status = sdb_database_close(database);
    if (status != SDB_OK) {
        (void)fprintf(stderr, "close failed: %s\n", sdb_status_string(status));
        goto cleanup;
    }
    database = NULL;
    status = sdb_database_open(bench_path, &options, &database);
    if (status != SDB_OK) {
        (void)fprintf(stderr, "reopen failed: %s\n", sdb_status_string(status));
        goto cleanup;
    }
    for (w = 0U; w < threads; ++w) {
        uint32_t op;
        for (op = 0U; op < ops_per; ++op) {
            uint8_t key[8];
            size_t got = 0U;
            bench_make_key(w, op, key);
            status = sdb_kv_get(
                database, bench_ns, sizeof(bench_ns), key, sizeof(key),
                readback, value_size, &got
            );
            if (status != SDB_OK || got != value_size
                || memcmp(readback, value, value_size) != 0) {
                (void)fprintf(
                    stderr,
                    "VERIFY FAILED worker=%u op=%u status=%s got=%zu\n",
                    w, op, sdb_status_string(status), got
                );
                goto cleanup;
            }
        }
    }
    (void)printf("  durability verify: all %llu records intact after reopen\n",
        (unsigned long long)total_ops);

    status = sdb_database_close(database);
    if (status != SDB_OK) {
        (void)fprintf(stderr, "final close failed: %s\n", sdb_status_string(status));
        goto cleanup;
    }
    database = NULL;   /* closed: cleanup must not close the freed handle */
    rc = 0;
    goto cleanup;

fail_spawned:
    /*
     * A worker did not start, but the ones before it are running and each holds
     * a pointer to its own ctx[w]. Reap them before the array is released --
     * freeing ctx while a live thread reads it is a use-after-free, which would
     * be a strictly worse defect than the leak this cleanup path exists to fix.
     * This label then falls through to the shared cleanup below.
     */
    for (w = 0U; w < spawned; ++w) {
#ifdef _WIN32
        (void)WaitForSingleObject(hthreads[w], INFINITE);
        (void)CloseHandle(hthreads[w]);
#else
        (void)pthread_join(pthreads[w], NULL);
#endif
    }
cleanup:
    if (database != NULL) {
        (void)sdb_database_close(database);
    }
    free(value);
    free(readback);
    free(ctx);
#ifdef _WIN32
    free(hthreads);
#else
    free(pthreads);
#endif
    bench_cleanup();
    return rc;
}
