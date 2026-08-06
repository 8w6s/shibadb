#ifndef SHIBADB_WAL_INDEX_H
#define SHIBADB_WAL_INDEX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "shibadb.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * In-memory hash map: page_id (uint64_t, non-zero) -> wal_offset (uint64_t).
 *
 * Open addressing with linear probing, power-of-two capacity, Fibonacci
 * hashing (same style as sdb_wal_id_set_insert in src/wal.c). Grows when
 * load factor exceeds ~0.5. `page_id == 0` is reserved as the empty-slot
 * sentinel and MUST NOT be inserted.
 *
 * Runtime-only: no persistence, no file I/O. Consumers hold the struct
 * by value/pointer.
 */

typedef struct sdb_wal_index_slot {
    uint64_t page_id;
    uint64_t wal_offset;
} sdb_wal_index_slot;

typedef struct sdb_wal_index {
    sdb_wal_index_slot *slots;
    size_t capacity; /* power of two, 0 iff slots == NULL */
    size_t size;     /* number of live (non-empty) slots */
} sdb_wal_index;

/* Initializes an empty index. Returns SDB_OK on success. */
sdb_status sdb_wal_index_init(sdb_wal_index *ix);

/* Frees the backing allocation and zeroes the struct. Safe on empty/NULL. */
void sdb_wal_index_free(sdb_wal_index *ix);

/*
 * Insert or update the offset for page_id. Newest wins (upsert).
 * page_id == 0 is rejected with SDB_E_INVALID_ARGUMENT.
 */
sdb_status sdb_wal_index_put(
    sdb_wal_index *ix, uint64_t page_id, uint64_t wal_offset
);

/*
 * Lookup. On hit, writes the stored offset to *offset_out and returns
 * true. On miss (or page_id == 0), returns false and does not touch
 * *offset_out.
 */
bool sdb_wal_index_get(
    const sdb_wal_index *ix, uint64_t page_id, uint64_t *offset_out
);

/* Empties the index without releasing the allocation. Safe on empty. */
void sdb_wal_index_clear(sdb_wal_index *ix);

#ifdef __cplusplus
}
#endif

#endif /* SHIBADB_WAL_INDEX_H */
