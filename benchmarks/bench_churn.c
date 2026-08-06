#include "shibadb_engine.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

static const char *churn_path = "bench-churn.tmp";
static const char *churn_wal_path = "bench-churn.tmp.wal";
static const char *churn_lock_path = "bench-churn.tmp.lock";
static const char *churn_replace_path = "bench-churn.tmp.replace";
static const char *churn_replace_temp_path = "bench-churn.tmp.replace.tmp";
static const uint8_t churn_ns[] = {'c', 'h', 'u', 'r', 'n', 0U};
static const uint8_t churn_password[] = "benchmark-password";

static uint64_t churn_now_ns(void)
{
    struct timespec ts;
    (void)clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * UINT64_C(1000000000)
        + (uint64_t)ts.tv_nsec;
}

static uint64_t churn_env_u64(const char *name, uint64_t fallback)
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

static void churn_make_key(uint64_t index, uint8_t key[8])
{
    size_t i;
    for (i = 0U; i < 8U; ++i) {
        key[i] = (uint8_t)(index >> (56U - i * 8U));
    }
}

static void churn_fail(const char *what, sdb_status status)
{
    (void)fprintf(
        stderr, "churn %s failed: %s (%d)\n",
        what, sdb_status_string(status), (int)status
    );
    exit(1);
}

static uint64_t churn_file_size(void)
{
    struct stat info;
    if (stat(churn_path, &info) != 0 || info.st_size < 0) {
        return 0U;
    }
    return (uint64_t)info.st_size;
}

static void churn_cleanup(void)
{
    (void)remove(churn_path);
    (void)remove(churn_wal_path);
    (void)remove(churn_lock_path);
    (void)remove(churn_replace_path);
    (void)remove(churn_replace_temp_path);
}

static int churn_cmp_u64(const void *a, const void *b)
{
    const uint64_t x = *(const uint64_t *)a;
    const uint64_t y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

static void churn_batched_put(
    sdb_database *database, uint64_t base, uint64_t count,
    const uint8_t *value, size_t value_size, uint64_t batch
)
{
    uint64_t done = 0U;
    while (done < count) {
        sdb_transaction *txn = NULL;
        const uint64_t n = count - done < batch ? count - done : batch;
        uint64_t j;
        sdb_status status = sdb_transaction_begin(database, &txn);
        if (status != SDB_OK) {
            churn_fail("txn_begin", status);
        }
        for (j = 0U; j < n; ++j) {
            uint8_t key[8];
            churn_make_key(base + done + j, key);
            status = sdb_transaction_kv_put(
                txn, churn_ns, sizeof(churn_ns), key, sizeof(key),
                value, value_size
            );
            if (status != SDB_OK) {
                churn_fail("txn_put", status);
            }
        }
        status = sdb_transaction_commit(txn);
        if (status != SDB_OK) {
            churn_fail("txn_commit", status);
        }
        (void)sdb_transaction_close(txn);
        done += n;
    }
}

static void churn_batched_delete(
    sdb_database *database, uint64_t base, uint64_t count, uint64_t batch
)
{
    uint64_t done = 0U;
    while (done < count) {
        sdb_transaction *txn = NULL;
        const uint64_t n = count - done < batch ? count - done : batch;
        uint64_t j;
        sdb_status status = sdb_transaction_begin(database, &txn);
        if (status != SDB_OK) {
            churn_fail("txn_begin", status);
        }
        for (j = 0U; j < n; ++j) {
            uint8_t key[8];
            churn_make_key(base + done + j, key);
            status = sdb_transaction_kv_delete(
                txn, churn_ns, sizeof(churn_ns), key, sizeof(key)
            );
            if (status != SDB_OK) {
                churn_fail("txn_delete", status);
            }
        }
        status = sdb_transaction_commit(txn);
        if (status != SDB_OK) {
            churn_fail("txn_commit", status);
        }
        (void)sdb_transaction_close(txn);
        done += n;
    }
}

static double churn_get_p50_us(
    sdb_database *database, uint64_t present_base, uint64_t present_count,
    size_t value_size
)
{
    const size_t samples = present_count < 1000U
        ? (size_t)present_count : 1000U;
    uint64_t *lat;
    uint8_t *buf;
    size_t i;
    double result;
    if (samples == 0U) {
        return 0.0;
    }
    lat = (uint64_t *)malloc(samples * sizeof(*lat));
    buf = (uint8_t *)malloc(value_size == 0U ? 1U : value_size);
    if (lat == NULL || buf == NULL) {
        free(lat);
        free(buf);
        return 0.0;
    }
    for (i = 0U; i < samples; ++i) {
        uint8_t key[8];
        size_t got = 0U;
        uint64_t t0;
        sdb_status status;
        churn_make_key(
            present_base + (uint64_t)i * (present_count / samples), key
        );
        t0 = churn_now_ns();
        status = sdb_kv_get(
            database, churn_ns, sizeof(churn_ns), key, sizeof(key),
            buf, value_size, &got
        );
        lat[i] = churn_now_ns() - t0;
        if (status != SDB_OK) {
            churn_fail("sample_get", status);
        }
    }
    qsort(lat, samples, sizeof(*lat), churn_cmp_u64);
    {
        const size_t mid = samples / 2U;
        result = (double)lat[mid] / 1000.0;
    }
    free(lat);
    free(buf);
    return result;
}

static void churn_stage(
    const char *label, sdb_database *database,
    uint64_t present_base, uint64_t present_count, size_t value_size
)
{
    sdb_verify_result verify;
    sdb_status status = sdb_database_verify(database, &verify);
    if (status != SDB_OK) {
        churn_fail("verify", status);
    }
    (void)printf(
        "  %-14s file=%7.2f MB  raw_entries=%8llu  objects=%8llu  "
        "pages=%8llu  get_P50=%7.1fus\n",
        label,
        (double)churn_file_size() / (1024.0 * 1024.0),
        (unsigned long long)verify.raw_entry_count,
        (unsigned long long)verify.object_count,
        (unsigned long long)verify.allocated_page_count,
        churn_get_p50_us(database, present_base, present_count, value_size)
    );
}

int main(void)
{
    const uint64_t n = churn_env_u64("SDB_CHURN_KEYS", 50000U);
    const size_t value_size = (size_t)churn_env_u64("SDB_CHURN_VALUE_SIZE", 100U);
    const uint64_t batch = churn_env_u64("SDB_CHURN_BATCH", 1000U);
    const uint64_t sustained_rounds = churn_env_u64(
        "SDB_CHURN_ROUNDS", 1U
    );
    sdb_database_options options;
    sdb_database *database = NULL;
    sdb_compact_result compact;
    uint8_t *value;
    uint64_t compact_start;
    double compact_ms;
    sdb_status status;

    value = (uint8_t *)malloc(value_size == 0U ? 1U : value_size);
    if (value == NULL) {
        (void)fprintf(stderr, "churn: out of memory\n");
        return 1;
    }
    (void)memset(value, 0x5A, value_size);

    churn_cleanup();
    sdb_database_options_init(&options);
    options.kdf_iterations = SDB_MIN_KDF_ITERATIONS;
    options.password = churn_password;
    options.password_size = sizeof(churn_password) - 1U;
    status = sdb_database_create(churn_path, &options, &database);
    if (status != SDB_OK) {
        churn_fail("create", status);
    }

    (void)printf(
        "shibadb churn lifecycle: keys=%llu value=%zuB batch=%llu "
        "(encrypted)\n",
        (unsigned long long)n, value_size, (unsigned long long)batch
    );

    churn_batched_put(database, 0U, n, value, value_size, batch);
    churn_stage("after put", database, n / 2U, n - n / 2U, value_size);

    (void)memset(value, 0xA5, value_size);
    churn_batched_put(database, 0U, n, value, value_size, batch);
    churn_stage("after update", database, n / 2U, n - n / 2U, value_size);

    churn_batched_delete(database, 0U, n / 2U, batch);
    churn_stage("after delete", database, n / 2U, n - n / 2U, value_size);

    churn_batched_put(database, 0U, n / 2U, value, value_size, batch);
    churn_stage("after reput", database, n / 2U, n - n / 2U, value_size);

    if (sustained_rounds > 1U) {
        uint64_t round;
        for (round = 1U; round < sustained_rounds; ++round) {
            char label[32];
            churn_batched_delete(database, 0U, n, batch);
            churn_batched_put(database, 0U, n, value, value_size, batch);
            (void)snprintf(
                label, sizeof(label), "churn %llu",
                (unsigned long long)(round + 1U)
            );
            churn_stage(label, database, 0U, n, value_size);
        }
    }

    compact_start = churn_now_ns();
    status = sdb_database_compact(database, &options, &compact);
    compact_ms = (double)(churn_now_ns() - compact_start) / 1e6;
    if (status != SDB_OK) {
        churn_fail("compact", status);
    }
    (void)printf(
        "  compact: %.0f ms  bytes %llu -> %llu  raw_entries %llu -> %llu\n",
        compact_ms,
        (unsigned long long)compact.byte_count_before,
        (unsigned long long)compact.byte_count_after,
        (unsigned long long)compact.raw_entries_before,
        (unsigned long long)compact.raw_entries_after
    );
    churn_stage("after compact", database, n / 2U, n - n / 2U, value_size);

    status = sdb_database_close(database);
    if (status != SDB_OK) {
        churn_fail("close", status);
    }
    free(value);
    churn_cleanup();
    return 0;
}
