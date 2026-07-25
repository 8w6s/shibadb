#include "shibadb_engine.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#define RECORD_COUNT 32U
#define TRANSACTION_COUNT 200U
#define MAX_OPERATIONS 20U

static const char *database_path = "test-public-transaction-property.tmp";
static const char *wal_path = "test-public-transaction-property.tmp.wal";
static const char *lock_path = "test-public-transaction-property.tmp.lock";
static const uint8_t namespace_name[] = "model";

typedef struct model_record {
    uint32_t value;
    bool present;
} model_record;

static uint32_t random_state = UINT32_C(0x8f312a09);

static uint32_t next_random(void)
{
    random_state = random_state * UINT32_C(1664525) + UINT32_C(1013904223);
    return random_state;
}

static void encode_u32(uint8_t output[4], uint32_t value)
{
    output[0] = (uint8_t)value;
    output[1] = (uint8_t)(value >> 8U);
    output[2] = (uint8_t)(value >> 16U);
    output[3] = (uint8_t)(value >> 24U);
}

static void verify_transaction_record(
    sdb_transaction *transaction, uint32_t record, const model_record *expected
)
{
    uint8_t key[4];
    uint8_t value[4];
    uint8_t actual[4];
    size_t actual_size = 0U;
    sdb_status status;
    encode_u32(key, record);
    encode_u32(value, expected->value);
    status = sdb_transaction_kv_get(
        transaction, namespace_name, sizeof(namespace_name),
        key, sizeof(key), actual, sizeof(actual), &actual_size
    );
    if (!expected->present) {
        assert(status == SDB_E_NOT_FOUND);
    } else {
        assert(status == SDB_OK);
        assert(actual_size == sizeof(value));
        assert(memcmp(actual, value, sizeof(value)) == 0);
    }
}

static void verify_database(
    sdb_database *database, const model_record model[RECORD_COUNT]
)
{
    uint32_t record;
    for (record = 0U; record < RECORD_COUNT; ++record) {
        uint8_t key[4];
        uint8_t value[4];
        uint8_t actual[4];
        size_t actual_size = 0U;
        sdb_status status;
        encode_u32(key, record);
        encode_u32(value, model[record].value);
        status = sdb_kv_get(
            database, namespace_name, sizeof(namespace_name),
            key, sizeof(key), actual, sizeof(actual), &actual_size
        );
        if (!model[record].present) {
            assert(status == SDB_E_NOT_FOUND);
        } else {
            assert(status == SDB_OK);
            assert(actual_size == sizeof(value));
            assert(memcmp(actual, value, sizeof(value)) == 0);
        }
    }
}

int main(void)
{
    sdb_database_options options;
    sdb_database *database = NULL;
    model_record committed[RECORD_COUNT] = {{0U, false}};
    uint32_t transaction_index;

    (void)remove(database_path);
    (void)remove(wal_path);
    (void)remove(lock_path);
    sdb_database_options_init(&options);
    assert(sdb_database_create(
        database_path, &options, &database
    ) == SDB_OK);
    for (transaction_index = 0U;
         transaction_index < TRANSACTION_COUNT;
         ++transaction_index) {
        model_record staged[RECORD_COUNT];
        sdb_transaction *transaction = NULL;
        const uint32_t operation_count =
            1U + next_random() % MAX_OPERATIONS;
        const bool commit = (next_random() & 3U) != 0U;
        uint32_t operation;
        (void)memcpy(staged, committed, sizeof(staged));
        assert(sdb_transaction_begin(database, &transaction) == SDB_OK);
        for (operation = 0U; operation < operation_count; ++operation) {
            const uint32_t record = next_random() % RECORD_COUNT;
            uint8_t key[4];
            encode_u32(key, record);
            if (staged[record].present && (next_random() & 3U) == 0U) {
                assert(sdb_transaction_kv_delete(
                    transaction, namespace_name, sizeof(namespace_name),
                    key, sizeof(key)
                ) == SDB_OK);
                staged[record].present = false;
            } else {
                uint8_t value[4];
                staged[record].value = next_random();
                staged[record].present = true;
                encode_u32(value, staged[record].value);
                assert(sdb_transaction_kv_put(
                    transaction, namespace_name, sizeof(namespace_name),
                    key, sizeof(key), value, sizeof(value)
                ) == SDB_OK);
            }
            verify_transaction_record(
                transaction, record, &staged[record]
            );
        }
        if (commit) {
            assert(sdb_transaction_commit(transaction) == SDB_OK);
            (void)memcpy(committed, staged, sizeof(committed));
        } else {
            assert(sdb_transaction_rollback(transaction) == SDB_OK);
        }
        assert(sdb_transaction_close(transaction) == SDB_OK);
        verify_database(database, committed);
    }
    assert(sdb_database_close(database) == SDB_OK);
    database = NULL;
    assert(sdb_database_open(
        database_path, &options, &database
    ) == SDB_OK);
    verify_database(database, committed);
    assert(sdb_database_close(database) == SDB_OK);
    (void)remove(database_path);
    (void)remove(wal_path);
    (void)remove(lock_path);
    (void)puts("public transaction property tests: ok");
    return 0;
}
