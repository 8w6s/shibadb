#include "shibadb_engine.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#endif

static const char *bench_path = "bench-kv.tmp";
static const char *bench_wal_path = "bench-kv.tmp.wal";
static const char *bench_lock_path = "bench-kv.tmp.lock";
static const char *bench_replace_path = "bench-kv.tmp.replace";
static const char *bench_replace_temp_path = "bench-kv.tmp.replace.tmp";
static const uint8_t bench_ns[] = {'b', 'e', 'n', 'c', 'h', 0U};
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
    return (uint64_t)ts.tv_sec * UINT64_C(1000000000)
        + (uint64_t)ts.tv_nsec;
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

static int bench_cmp_u64(const void *a, const void *b)
{
    const uint64_t x = *(const uint64_t *)a;
    const uint64_t y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

static void bench_make_key(uint64_t index, uint8_t key[8])
{
    size_t i;
    for (i = 0U; i < 8U; ++i) {
        key[i] = (uint8_t)(index >> (56U - i * 8U));
    }
}

static double bench_pct(const uint64_t *sorted, size_t n, double p)
{
    if (n == 0U) {
        return 0.0;
    }
    return (double)sorted[(size_t)((double)(n - 1U) * p)] / 1000.0;
}

static void bench_report(
    const char *name, uint64_t *lat, size_t n, uint64_t total_ns
)
{
    const double secs = (double)total_ns / 1e9;
    qsort(lat, n, sizeof(*lat), bench_cmp_u64);
    (void)printf(
        "  %-16s %9.0f ops/s  P50=%8.1fus P95=%8.1fus "
        "P99=%8.1fus max=%9.1fus\n",
        name,
        secs > 0.0 ? (double)n / secs : 0.0,
        bench_pct(lat, n, 0.50),
        bench_pct(lat, n, 0.95),
        bench_pct(lat, n, 0.99),
        (double)lat[n - 1U] / 1000.0
    );
}

static void bench_fail(const char *what, sdb_status status, uint64_t at)
{
    (void)fprintf(
        stderr, "bench %s failed at %llu: %s (%d)\n",
        what, (unsigned long long)at, sdb_status_string(status), (int)status
    );
    exit(1);
}

static void bench_cleanup(void)
{
    (void)remove(bench_path);
    (void)remove(bench_wal_path);
    (void)remove(bench_lock_path);
    (void)remove(bench_replace_path);
    (void)remove(bench_replace_temp_path);
}

int main(void)
{
    const uint64_t operations = bench_env_u64("SDB_BENCH_OPERATIONS", 50000U);
    const size_t value_size =
        (size_t)bench_env_u64("SDB_BENCH_VALUE_SIZE", 100U);
    const uint64_t batch = bench_env_u64("SDB_BENCH_BATCH", 1000U);
    const bool plaintext =
        getenv("SDB_BENCH_PLAINTEXT") != NULL
        && strcmp(getenv("SDB_BENCH_PLAINTEXT"), "0") != 0;
    const bool skip_auto =
        getenv("SDB_BENCH_SKIP_AUTO") != NULL
        && strcmp(getenv("SDB_BENCH_SKIP_AUTO"), "0") != 0;
    sdb_database_options options;
    sdb_database *database = NULL;
    uint8_t *value;
    uint64_t *lat;
    uint8_t *readback;
    uint64_t i;
    uint64_t phase_start;
    uint64_t phase_end;
    sdb_status status;

    value = (uint8_t *)malloc(value_size == 0U ? 1U : value_size);
    readback = (uint8_t *)malloc(value_size == 0U ? 1U : value_size);
    lat = (uint64_t *)malloc((size_t)operations * sizeof(*lat));
    if (value == NULL || readback == NULL || lat == NULL) {
        (void)fprintf(stderr, "bench: out of memory\n");
        return 1;
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
        bench_fail("create", status, 0U);
    }

    (void)printf(
        "shibadb KV benchmark: mode=%s ops=%llu value=%zuB batch=%llu\n",
        plaintext ? "plaintext" : "encrypted",
        (unsigned long long)operations, value_size,
        (unsigned long long)batch
    );

  if (!skip_auto) {
    phase_start = bench_now_ns();
    for (i = 0U; i < operations; ++i) {
        uint8_t key[8];
        uint64_t t0;
        bench_make_key(i, key);
        t0 = bench_now_ns();
        status = sdb_kv_put(
            database, bench_ns, sizeof(bench_ns), key, sizeof(key),
            value, value_size
        );
        lat[i] = bench_now_ns() - t0;
        if (status != SDB_OK) {
            bench_fail("put", status, i);
        }
    }
    phase_end = bench_now_ns();
    bench_report("put (auto)", lat, (size_t)operations, phase_end - phase_start);

    phase_start = bench_now_ns();
    for (i = 0U; i < operations; ++i) {
        uint8_t key[8];
        size_t got = 0U;
        uint64_t t0;
        bench_make_key(i, key);
        t0 = bench_now_ns();
        status = sdb_kv_get(
            database, bench_ns, sizeof(bench_ns), key, sizeof(key),
            readback, value_size, &got
        );
        lat[i] = bench_now_ns() - t0;
        if (status != SDB_OK || got != value_size) {
            bench_fail("get", status, i);
        }
    }
    phase_end = bench_now_ns();
    bench_report("get (auto)", lat, (size_t)operations, phase_end - phase_start);

    phase_start = bench_now_ns();
    for (i = 0U; i < operations; ++i) {
        uint8_t key[8];
        uint64_t t0;
        bench_make_key(i, key);
        t0 = bench_now_ns();
        status = sdb_kv_delete(
            database, bench_ns, sizeof(bench_ns), key, sizeof(key)
        );
        lat[i] = bench_now_ns() - t0;
        if (status != SDB_OK) {
            bench_fail("delete", status, i);
        }
    }
    phase_end = bench_now_ns();
    bench_report(
        "delete (auto)", lat, (size_t)operations, phase_end - phase_start
    );
  }

    {
        uint64_t committed = 0U;
        uint64_t txns = 0U;
        phase_start = bench_now_ns();
        while (committed < operations) {
            sdb_transaction *txn = NULL;
            uint64_t n = operations - committed < batch
                ? operations - committed : batch;
            uint64_t j;
            uint64_t t0 = bench_now_ns();
            status = sdb_transaction_begin(database, &txn);
            if (status != SDB_OK) {
                bench_fail("txn_begin", status, committed);
            }
            for (j = 0U; j < n; ++j) {
                uint8_t key[8];
                bench_make_key(operations + committed + j, key);
                status = sdb_transaction_kv_put(
                    txn, bench_ns, sizeof(bench_ns), key, sizeof(key),
                    value, value_size
                );
                if (status != SDB_OK) {
                    bench_fail("txn_put", status, committed + j);
                }
            }
            status = sdb_transaction_commit(txn);
            if (status != SDB_OK) {
                bench_fail("txn_commit", status, committed);
            }
            (void)sdb_transaction_close(txn);
            lat[txns] = bench_now_ns() - t0;
            committed += n;
            ++txns;
        }
        phase_end = bench_now_ns();
        {
            const double secs = (double)(phase_end - phase_start) / 1e9;
            qsort(lat, (size_t)txns, sizeof(*lat), bench_cmp_u64);
            (void)printf(
                "  %-16s %9.0f ops/s  (%llu txns of ~%llu; "
                "commit P50=%.1fus P99=%.1fus)\n",
                "put (batched)",
                secs > 0.0 ? (double)operations / secs : 0.0,
                (unsigned long long)txns, (unsigned long long)batch,
                bench_pct(lat, (size_t)txns, 0.50),
                bench_pct(lat, (size_t)txns, 0.99)
            );
        }
    }

    status = sdb_database_close(database);
    if (status != SDB_OK) {
        bench_fail("close", status, operations);
    }
    free(value);
    free(readback);
    free(lat);
    bench_cleanup();
    return 0;
}
