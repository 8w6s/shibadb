#include "shibadb_engine.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SOAK_KEY_COUNT ((size_t)64)
#define SOAK_MAX_VALUE_SIZE ((size_t)4096)
#define SOAK_DEFAULT_OPERATIONS UINT64_C(2000)
#define SOAK_MAX_OPERATIONS UINT64_C(10000000)

static const char *test_path = "test-engine-soak.tmp";
static const char *wal_path = "test-engine-soak.tmp.wal";
static const char *lock_path = "test-engine-soak.tmp.lock";
static const char *replace_path = "test-engine-soak.tmp.replace";
static const char *replace_temp_path = "test-engine-soak.tmp.replace.tmp";
static const char *backup_path = "test-engine-soak-backup.tmp";
static const char *backup_wal_path = "test-engine-soak-backup.tmp.wal";
static const char *backup_lock_path = "test-engine-soak-backup.tmp.lock";
static const uint8_t namespace_name[] = {'s', 'o', 'a', 'k', 0U};
static const uint8_t password[] = "deterministic-soak-password";

static uint64_t next_random(uint64_t *state)
{
    uint64_t value = *state;
    value ^= value << 13U;
    value ^= value >> 7U;
    value ^= value << 17U;
    *state = value;
    return value;
}

static uint64_t operation_count(void)
{
    const char *text = getenv("SDB_SOAK_OPERATIONS");
    char *end = NULL;
    unsigned long long parsed;
    if (text == NULL || text[0] == '\0') {
        return SOAK_DEFAULT_OPERATIONS;
    }
    errno = 0;
    parsed = strtoull(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0'
        || parsed == 0ULL
        || parsed > (unsigned long long)SOAK_MAX_OPERATIONS) {
        return SOAK_DEFAULT_OPERATIONS;
    }
    return (uint64_t)parsed;
}

static void cleanup(void)
{
    (void)remove(wal_path);
    (void)remove(test_path);
    (void)remove(lock_path);
    (void)remove(replace_path);
    (void)remove(replace_temp_path);
    (void)remove(backup_wal_path);
    (void)remove(backup_path);
    (void)remove(backup_lock_path);
}

static void make_key(size_t index, uint8_t key[4])
{
    key[0] = (uint8_t)index;
    key[1] = UINT8_C(0xa5);
    key[2] = (uint8_t)(index >> 8U);
    key[3] = 0U;
}

static void verify_model(
    sdb_database *database,
    uint8_t model[SOAK_KEY_COUNT][SOAK_MAX_VALUE_SIZE],
    const size_t sizes[SOAK_KEY_COUNT],
    const bool present[SOAK_KEY_COUNT]
)
{
    uint8_t actual[SOAK_MAX_VALUE_SIZE];
    size_t index;
    for (index = 0U; index < SOAK_KEY_COUNT; ++index) {
        uint8_t key[4];
        size_t actual_size = 0U;
        sdb_status status;
        make_key(index, key);
        status = sdb_kv_get(
            database,
            namespace_name,
            sizeof(namespace_name),
            key,
            sizeof(key),
            actual,
            sizeof(actual),
            &actual_size
        );
        if (present[index]) {
            assert(status == SDB_OK);
            assert(actual_size == sizes[index]);
            assert(memcmp(actual, model[index], actual_size) == 0);
        } else {
            assert(status == SDB_E_NOT_FOUND);
        }
    }
}

int main(void)
{
    sdb_database_options options;
    sdb_database *database = NULL;
    uint8_t model[SOAK_KEY_COUNT][SOAK_MAX_VALUE_SIZE];
    size_t sizes[SOAK_KEY_COUNT] = {0U};
    bool present[SOAK_KEY_COUNT] = {false};
    uint64_t random_state = UINT64_C(0x7368696261646221);
    const uint64_t operations = operation_count();
    uint64_t operation;
    cleanup();
    (void)memset(model, 0, sizeof(model));
    sdb_database_options_init(&options);
    options.kdf_iterations = SDB_MIN_KDF_ITERATIONS;
    options.password = password;
    options.password_size = sizeof(password) - 1U;
    assert(sdb_database_create(test_path, &options, &database) == SDB_OK);

    for (operation = 1U; operation <= operations; ++operation) {
        const uint64_t random = next_random(&random_state);
        const size_t slot = (size_t)(random % (uint64_t)SOAK_KEY_COUNT);
        const unsigned action = (unsigned)((random >> 8U) % 10U);
        uint8_t key[4];
        make_key(slot, key);
        if (action < 6U) {
            const size_t size =
                (size_t)((random >> 16U) % SOAK_MAX_VALUE_SIZE);
            size_t index;
            for (index = 0U; index < size; ++index) {
                model[slot][index] = (uint8_t)(
                    random + operation + (uint64_t)(index * 31U)
                );
            }
            {
                const sdb_status put_status = sdb_kv_put(
                database,
                namespace_name,
                sizeof(namespace_name),
                key,
                sizeof(key),
                model[slot],
                size
                );
                if (put_status != SDB_OK) {
                    (void)fprintf(
                        stderr,
                        "put failed at operation %llu slot %zu size %zu: "
                        "%s (%d)\n",
                        (unsigned long long)operation,
                        slot,
                        size,
                        sdb_status_string(put_status),
                        (int)put_status
                    );
                    abort();
                }
            }
            sizes[slot] = size;
            present[slot] = true;
        } else if (action < 8U) {
            const sdb_status status = sdb_kv_delete(
                database,
                namespace_name,
                sizeof(namespace_name),
                key,
                sizeof(key)
            );
            assert(status == (present[slot] ? SDB_OK : SDB_E_NOT_FOUND));
            present[slot] = false;
            sizes[slot] = 0U;
        } else {
            uint8_t actual[SOAK_MAX_VALUE_SIZE];
            size_t actual_size = 0U;
            const sdb_status status = sdb_kv_get(
                database,
                namespace_name,
                sizeof(namespace_name),
                key,
                sizeof(key),
                actual,
                sizeof(actual),
                &actual_size
            );
            if (present[slot]) {
                assert(status == SDB_OK);
                assert(actual_size == sizes[slot]);
                assert(memcmp(actual, model[slot], actual_size) == 0);
            } else {
                assert(status == SDB_E_NOT_FOUND);
            }
        }
        if (operation % UINT64_C(250) == 0U) {
            sdb_verify_result verify;
            assert(sdb_database_verify(database, &verify) == SDB_OK);
            assert(sdb_database_close(database) == SDB_OK);
            assert(sdb_database_open(
                test_path, &options, &database
            ) == SDB_OK);
            verify_model(database, model, sizes, present);
        }
        if (operation % UINT64_C(500) == 0U) {
            sdb_backup_result backup;
            sdb_database *snapshot = NULL;
            assert(sdb_database_backup(
                database, backup_path, true, &backup
            ) == SDB_OK);
            assert(backup.byte_count != 0U);
            assert(sdb_database_open(
                backup_path, &options, &snapshot
            ) == SDB_OK);
            verify_model(snapshot, model, sizes, present);
            assert(sdb_database_close(snapshot) == SDB_OK);
        }
        if (operation % UINT64_C(1000) == 0U) {
            sdb_compact_result compact;
            assert(sdb_database_compact(
                database, &options, &compact
            ) == SDB_OK);
            assert(compact.raw_entries_after
                <= compact.raw_entries_before);
        }
    }
    verify_model(database, model, sizes, present);
    {
        sdb_verify_result verify;
        assert(sdb_database_verify(database, &verify) == SDB_OK);
    }
    assert(sdb_database_close(database) == SDB_OK);
    cleanup();
    (void)printf(
        "engine soak tests: ok (%llu operations)\n",
        (unsigned long long)operations
    );
    return 0;
}
