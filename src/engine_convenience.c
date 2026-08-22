#include "shibadb_engine.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/*
 * Convenience API (SDB_ENGINE_API_VERSION 2) — see shibadb_engine.h. Thin
 * wrappers that compose the public KV, scan, and transaction entry points;
 * they add no on-disk state and never touch the pager/WAL/B+Tree directly.
 */

void sdb_free(void *pointer)
{
    free(pointer);
}

sdb_status sdb_kv_get_alloc(
    sdb_database *database,
    const uint8_t *namespace_name,
    size_t namespace_size,
    const uint8_t *key,
    size_t key_size,
    uint8_t **value_out,
    size_t *value_size_out
)
{
    if (value_out == NULL || value_size_out == NULL) {
        return SDB_E_INVALID_ARGUMENT;
    }
    *value_out = NULL;
    *value_size_out = 0U;
    for (;;) {
        size_t needed = 0U;
        size_t capacity;
        size_t written = 0U;
        uint8_t *buffer;
        sdb_status status = sdb_kv_get(
            database, namespace_name, namespace_size, key, key_size,
            NULL, 0U, &needed
        );
        if (status != SDB_OK && status != SDB_E_BUFFER_TOO_SMALL) {
            return status; /* SDB_E_NOT_FOUND or a real error */
        }
        /* Allocate at least one byte so a zero-length value is non-NULL. */
        capacity = needed == 0U ? 1U : needed;
        buffer = (uint8_t *)malloc(capacity);
        if (buffer == NULL) {
            return SDB_E_OUT_OF_MEMORY;
        }
        status = sdb_kv_get(
            database, namespace_name, namespace_size, key, key_size,
            buffer, capacity, &written
        );
        if (status == SDB_E_BUFFER_TOO_SMALL) {
            /* A concurrent writer grew the value; retry with the new size. */
            free(buffer);
            continue;
        }
        if (status != SDB_OK) {
            free(buffer);
            return status;
        }
        *value_out = buffer;
        *value_size_out = written;
        return SDB_OK;
    }
}

sdb_status sdb_kv_exists(
    sdb_database *database,
    const uint8_t *namespace_name,
    size_t namespace_size,
    const uint8_t *key,
    size_t key_size,
    bool *exists_out
)
{
    size_t needed = 0U;
    sdb_status status;
    if (exists_out == NULL) {
        return SDB_E_INVALID_ARGUMENT;
    }
    status = sdb_kv_get(
        database, namespace_name, namespace_size, key, key_size,
        NULL, 0U, &needed
    );
    if (status == SDB_OK || status == SDB_E_BUFFER_TOO_SMALL) {
        *exists_out = true;
        return SDB_OK;
    }
    if (status == SDB_E_NOT_FOUND) {
        *exists_out = false;
        return SDB_OK;
    }
    return status;
}

static bool sdb_convenience_count_visitor(
    void *context,
    const uint8_t *key,
    size_t key_size,
    const uint8_t *value,
    size_t value_size
)
{
    (void)key;
    (void)key_size;
    (void)value;
    (void)value_size;
    ++*(uint64_t *)context;
    return true;
}

sdb_status sdb_kv_count_prefix(
    sdb_database *database,
    const uint8_t *namespace_name,
    size_t namespace_size,
    const uint8_t *prefix,
    size_t prefix_size,
    uint64_t *count_out
)
{
    uint64_t total = 0U;
    sdb_status status;
    if (count_out == NULL) {
        return SDB_E_INVALID_ARGUMENT;
    }
    status = sdb_kv_scan_prefix(
        database, namespace_name, namespace_size, prefix, prefix_size,
        NULL, sdb_convenience_count_visitor, &total, NULL
    );
    if (status == SDB_OK) {
        *count_out = total;
    }
    return status;
}

sdb_status sdb_kv_count(
    sdb_database *database,
    const uint8_t *namespace_name,
    size_t namespace_size,
    uint64_t *count_out
)
{
    return sdb_kv_count_prefix(
        database, namespace_name, namespace_size, NULL, 0U, count_out
    );
}

sdb_status sdb_kv_put_if_absent(
    sdb_database *database,
    const uint8_t *namespace_name,
    size_t namespace_size,
    const uint8_t *key,
    size_t key_size,
    const uint8_t *value,
    size_t value_size
)
{
    sdb_transaction *txn = NULL;
    size_t needed = 0U;
    sdb_status op;
    sdb_status status = sdb_transaction_begin(database, &txn);
    if (status != SDB_OK) {
        return status;
    }
    op = sdb_transaction_kv_get(
        txn, namespace_name, namespace_size, key, key_size, NULL, 0U, &needed
    );
    if (op == SDB_OK || op == SDB_E_BUFFER_TOO_SMALL) {
        op = SDB_E_CONFLICT; /* key already present */
    } else if (op == SDB_E_NOT_FOUND) {
        op = sdb_transaction_kv_put(
            txn, namespace_name, namespace_size, key, key_size,
            value, value_size
        );
    }
    if (op == SDB_OK) {
        status = sdb_transaction_commit(txn);
    } else {
        (void)sdb_transaction_rollback(txn);
        status = op;
    }
    (void)sdb_transaction_close(txn);
    return status;
}

sdb_status sdb_kv_compare_and_swap(
    sdb_database *database,
    const uint8_t *namespace_name,
    size_t namespace_size,
    const uint8_t *key,
    size_t key_size,
    const uint8_t *expected,
    size_t expected_size,
    const uint8_t *desired,
    size_t desired_size
)
{
    sdb_transaction *txn = NULL;
    uint8_t *current = NULL;
    size_t current_size = 0U;
    size_t needed = 0U;
    sdb_status op;
    sdb_status status = sdb_transaction_begin(database, &txn);
    if (status != SDB_OK) {
        return status;
    }
    op = sdb_transaction_kv_get(
        txn, namespace_name, namespace_size, key, key_size, NULL, 0U, &needed
    );
    if (op == SDB_E_NOT_FOUND) {
        op = SDB_E_CONFLICT; /* nothing to compare against */
    } else if (op == SDB_OK || op == SDB_E_BUFFER_TOO_SMALL) {
        size_t capacity = needed == 0U ? 1U : needed;
        current = (uint8_t *)malloc(capacity);
        if (current == NULL) {
            op = SDB_E_OUT_OF_MEMORY;
        } else {
            op = sdb_transaction_kv_get(
                txn, namespace_name, namespace_size, key, key_size,
                current, capacity, &current_size
            );
            if (op == SDB_OK) {
                bool match = current_size == expected_size
                    && (expected_size == 0U
                        || memcmp(current, expected, expected_size) == 0);
                if (match) {
                    op = sdb_transaction_kv_put(
                        txn, namespace_name, namespace_size, key, key_size,
                        desired, desired_size
                    );
                } else {
                    op = SDB_E_CONFLICT;
                }
            }
        }
    }
    free(current);
    if (op == SDB_OK) {
        status = sdb_transaction_commit(txn);
    } else {
        (void)sdb_transaction_rollback(txn);
        status = op;
    }
    (void)sdb_transaction_close(txn);
    return status;
}

sdb_status sdb_kv_increment(
    sdb_database *database,
    const uint8_t *namespace_name,
    size_t namespace_size,
    const uint8_t *key,
    size_t key_size,
    int64_t delta,
    int64_t *new_value_out
)
{
    sdb_transaction *txn = NULL;
    uint8_t buffer[8];
    size_t got = 0U;
    int64_t current = 0;
    int64_t result = 0;
    sdb_status op;
    sdb_status status = sdb_transaction_begin(database, &txn);
    if (status != SDB_OK) {
        return status;
    }
    op = sdb_transaction_kv_get(
        txn, namespace_name, namespace_size, key, key_size,
        buffer, sizeof(buffer), &got
    );
    if (op == SDB_OK) {
        if (got != 8U) {
            op = SDB_E_INVALID_ARGUMENT; /* not an 8-byte counter */
        } else {
            uint64_t raw = 0U;
            int index;
            for (index = 0; index < 8; ++index) {
                raw |= (uint64_t)buffer[index] << (8 * index);
            }
            current = (int64_t)raw;
        }
    } else if (op == SDB_E_NOT_FOUND) {
        current = 0;
        op = SDB_OK; /* absent counter starts at zero */
    } else if (op == SDB_E_BUFFER_TOO_SMALL) {
        op = SDB_E_INVALID_ARGUMENT; /* value larger than 8 bytes */
    }
    if (op == SDB_OK) {
        if ((delta > 0 && current > INT64_MAX - delta)
            || (delta < 0 && current < INT64_MIN - delta)) {
            op = SDB_E_OVERFLOW;
        } else {
            uint64_t raw;
            int index;
            result = current + delta;
            raw = (uint64_t)result;
            for (index = 0; index < 8; ++index) {
                buffer[index] = (uint8_t)((raw >> (8 * index)) & 0xFFU);
            }
            op = sdb_transaction_kv_put(
                txn, namespace_name, namespace_size, key, key_size,
                buffer, 8U
            );
        }
    }
    if (op == SDB_OK) {
        status = sdb_transaction_commit(txn);
        if (status == SDB_OK && new_value_out != NULL) {
            *new_value_out = result;
        }
    } else {
        (void)sdb_transaction_rollback(txn);
        status = op;
    }
    (void)sdb_transaction_close(txn);
    return status;
}

sdb_status sdb_kv_batch_apply(
    sdb_database *database,
    const sdb_batch_op *ops,
    size_t op_count
)
{
    sdb_transaction *txn = NULL;
    sdb_status status;
    sdb_status op = SDB_OK;
    size_t index;
    if (op_count == 0U) {
        return SDB_OK;
    }
    if (ops == NULL) {
        return SDB_E_INVALID_ARGUMENT;
    }
    status = sdb_transaction_begin(database, &txn);
    if (status != SDB_OK) {
        return status;
    }
    for (index = 0U; index < op_count && op == SDB_OK; ++index) {
        const sdb_batch_op *entry = &ops[index];
        switch (entry->kind) {
        case SDB_BATCH_OP_KV_PUT:
            op = sdb_transaction_kv_put(
                txn, entry->namespace_name, entry->namespace_size,
                entry->key, entry->key_size, entry->value, entry->value_size
            );
            break;
        case SDB_BATCH_OP_KV_DELETE:
            op = sdb_transaction_kv_delete(
                txn, entry->namespace_name, entry->namespace_size,
                entry->key, entry->key_size
            );
            break;
        default:
            op = SDB_E_INVALID_ARGUMENT;
            break;
        }
    }
    if (op == SDB_OK) {
        status = sdb_transaction_commit(txn);
    } else {
        (void)sdb_transaction_rollback(txn);
        status = op;
    }
    (void)sdb_transaction_close(txn);
    return status;
}

typedef struct sdb_collected_id {
    uint8_t *bytes;
    size_t size;
} sdb_collected_id;

typedef struct sdb_index_id_collector {
    sdb_collected_id *items;
    size_t count;
    size_t capacity;
    bool out_of_memory;
} sdb_index_id_collector;

static bool sdb_index_collect_visitor(
    void *context, const uint8_t *document_id, size_t document_id_size
)
{
    sdb_index_id_collector *collector = (sdb_index_id_collector *)context;
    uint8_t *copy;
    if (collector->out_of_memory) {
        return false;
    }
    if (collector->count == collector->capacity) {
        size_t new_capacity =
            collector->capacity == 0U ? 8U : collector->capacity * 2U;
        sdb_collected_id *grown = (sdb_collected_id *)realloc(
            collector->items, new_capacity * sizeof(*grown)
        );
        if (grown == NULL) {
            collector->out_of_memory = true;
            return false;
        }
        collector->items = grown;
        collector->capacity = new_capacity;
    }
    copy = (uint8_t *)malloc(document_id_size == 0U ? 1U : document_id_size);
    if (copy == NULL) {
        collector->out_of_memory = true;
        return false;
    }
    if (document_id_size != 0U) {
        (void)memcpy(copy, document_id, document_id_size);
    }
    collector->items[collector->count].bytes = copy;
    collector->items[collector->count].size = document_id_size;
    collector->count++;
    return true;
}

sdb_status sdb_index_query_documents(
    sdb_database *database,
    const uint8_t *collection,
    size_t collection_size,
    const uint8_t *index_name,
    size_t index_name_size,
    const uint8_t *value,
    size_t value_size,
    sdb_document_visit_fn visitor,
    void *context,
    size_t *match_count_out
)
{
    sdb_index_id_collector collector;
    sdb_status status;
    size_t emitted = 0U;
    size_t index;
    size_t visit_count = 0U;
    if (visitor == NULL) {
        return SDB_E_INVALID_ARGUMENT;
    }
    collector.items = NULL;
    collector.count = 0U;
    collector.capacity = 0U;
    collector.out_of_memory = false;

    /*
     * Phase 1: collect matching document ids. The visitor only copies bytes,
     * so it never re-enters the engine — safe while sdb_index_visit holds the
     * handle lock. sdb_index_visit requires a non-NULL match-count out param.
     */
    status = sdb_index_visit(
        database, collection, collection_size, index_name, index_name_size,
        value, value_size, sdb_index_collect_visitor, &collector, &visit_count
    );
    if (status == SDB_OK && collector.out_of_memory) {
        status = SDB_E_OUT_OF_MEMORY;
    }

    /* Phase 2: the lock is released; fetch each body and hand it to caller. */
    for (index = 0U; status == SDB_OK && index < collector.count; ++index) {
        size_t needed = 0U;
        size_t capacity;
        size_t got = 0U;
        uint8_t *body;
        bool keep_going;
        sdb_status read = sdb_document_get(
            database, collection, collection_size,
            collector.items[index].bytes, collector.items[index].size,
            NULL, 0U, &needed
        );
        if (read == SDB_E_NOT_FOUND) {
            continue; /* deleted between the two phases */
        }
        if (read != SDB_OK && read != SDB_E_BUFFER_TOO_SMALL) {
            status = read;
            break;
        }
        capacity = needed == 0U ? 1U : needed;
        body = (uint8_t *)malloc(capacity);
        if (body == NULL) {
            status = SDB_E_OUT_OF_MEMORY;
            break;
        }
        read = sdb_document_get(
            database, collection, collection_size,
            collector.items[index].bytes, collector.items[index].size,
            body, capacity, &got
        );
        if (read == SDB_E_NOT_FOUND) {
            free(body);
            continue;
        }
        if (read != SDB_OK) {
            free(body);
            status = read;
            break;
        }
        keep_going = visitor(
            context, collector.items[index].bytes,
            collector.items[index].size, body, got
        );
        free(body);
        ++emitted;
        if (!keep_going) {
            break;
        }
    }

    for (index = 0U; index < collector.count; ++index) {
        free(collector.items[index].bytes);
    }
    free(collector.items);
    if (status == SDB_OK && match_count_out != NULL) {
        *match_count_out = emitted;
    }
    return status;
}
