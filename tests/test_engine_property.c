#include "shibadb_engine.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MODEL_COUNT ((size_t)64)
#define MAX_VALUE_SIZE ((size_t)6000)
#define OPERATION_COUNT ((size_t)300)

static const char *test_path = "test-engine-property.tmp";
static const char *wal_path = "test-engine-property.tmp.wal";
static const char *lock_path = "test-engine-property.tmp.lock";
static const uint8_t namespace_name[] = {'n', 0U, 's'};

static uint32_t next_random(uint32_t *state)
{
    *state = (*state * UINT32_C(1664525)) + UINT32_C(1013904223);
    return *state;
}

static void make_key(size_t id, uint8_t key[4])
{
    key[0] = (uint8_t)id;
    key[1] = 0U;
    key[2] = (uint8_t)(id >> 8U);
    key[3] = 0xffU;
}

static void verify_model(
    sdb_database *database,
    uint8_t model[MODEL_COUNT][MAX_VALUE_SIZE],
    const size_t sizes[MODEL_COUNT],
    const bool present[MODEL_COUNT]
)
{
    uint8_t actual[MAX_VALUE_SIZE];
    size_t id;
    for (id = 0U; id < MODEL_COUNT; ++id) {
        uint8_t key[4];
        size_t actual_size;
        sdb_status status;
        make_key(id, key);
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
        if (!present[id]) {
            assert(status == SDB_E_NOT_FOUND);
        } else {
            assert(status == SDB_OK);
            assert(actual_size == sizes[id]);
            assert(memcmp(actual, model[id], actual_size) == 0);
        }
    }
}

int main(void)
{
    sdb_database_options options;
    sdb_database *database;
    uint8_t (*model)[MAX_VALUE_SIZE];
    size_t sizes[MODEL_COUNT] = {0U};
    bool present[MODEL_COUNT] = {false};
    uint32_t state = UINT32_C(0x5eed1234);
    size_t operation;

    (void)remove(test_path);
    (void)remove(wal_path);
    (void)remove(lock_path);
    model = (uint8_t (*)[MAX_VALUE_SIZE])calloc(
        MODEL_COUNT, MAX_VALUE_SIZE
    );
    assert(model != NULL);
    sdb_database_options_init(&options);
    assert(sdb_database_create(
        test_path, &options, &database
    ) == SDB_OK);
    for (operation = 0U; operation < OPERATION_COUNT; ++operation) {
        const uint32_t random = next_random(&state);
        const size_t id = (size_t)(random % (uint32_t)MODEL_COUNT);
        uint8_t key[4];
        make_key(id, key);
        if ((random % 5U) == 0U) {
            const sdb_status expected =
                present[id] ? SDB_OK : SDB_E_NOT_FOUND;
            assert(sdb_kv_delete(
                database,
                namespace_name,
                sizeof(namespace_name),
                key,
                sizeof(key)
            ) == expected);
            present[id] = false;
            sizes[id] = 0U;
        } else {
            size_t index;
            const size_t value_size =
                (size_t)(next_random(&state) % (uint32_t)MAX_VALUE_SIZE);
            for (index = 0U; index < value_size; ++index) {
                model[id][index] = (uint8_t)(
                    random + (uint32_t)index + (uint32_t)operation
                );
            }
            assert(sdb_kv_put(
                database,
                namespace_name,
                sizeof(namespace_name),
                key,
                sizeof(key),
                model[id],
                value_size
            ) == SDB_OK);
            present[id] = true;
            sizes[id] = value_size;
        }
        if ((operation % 50U) == 49U) {
            assert(sdb_database_close(database) == SDB_OK);
            assert(sdb_database_open(
                test_path, &options, &database
            ) == SDB_OK);
            verify_model(database, model, sizes, present);
        }
    }
    verify_model(database, model, sizes, present);
    assert(sdb_database_close(database) == SDB_OK);
    free(model);
    (void)remove(wal_path);
    (void)remove(test_path);
    (void)remove(lock_path);
    (void)puts("engine property tests: ok");
    return 0;
}
