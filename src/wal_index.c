#include "wal_index.h"

#include <stdlib.h>
#include <string.h>

/*
 * Fibonacci hash constant (2^64 / phi, rounded), matches the multiplier
 * used by sdb_wal_id_set_insert() in src/wal.c so we stay consistent
 * with the existing hashing style in this codebase.
 */
#define SDB_WAL_INDEX_HASH_MUL UINT64_C(11400714819323198485)

/*
 * Initial capacity: must be a power of two. 16 is small enough to grow
 * at least once in a modest test, large enough to avoid immediate
 * thrash for real workloads.
 */
#define SDB_WAL_INDEX_INITIAL_CAPACITY ((size_t)16)

static size_t sdb_wal_index_slot_for(
    const sdb_wal_index_slot *slots, size_t capacity, uint64_t page_id
)
{
    size_t index = (size_t)(
        (page_id * SDB_WAL_INDEX_HASH_MUL) & (uint64_t)(capacity - 1U)
    );
    while (slots[index].page_id != 0U && slots[index].page_id != page_id) {
        index = (index + 1U) & (capacity - 1U);
    }
    return index;
}

static sdb_status sdb_wal_index_grow(sdb_wal_index *ix)
{
    size_t new_capacity;
    sdb_wal_index_slot *new_slots;
    size_t i;

    if (ix->capacity > SIZE_MAX / 2U) {
        return SDB_E_OUT_OF_MEMORY;
    }
    new_capacity = ix->capacity * 2U;
    if (new_capacity > SIZE_MAX / sizeof(*new_slots)) {
        return SDB_E_OUT_OF_MEMORY;
    }
    new_slots = (sdb_wal_index_slot *)calloc(
        new_capacity, sizeof(*new_slots)
    );
    if (new_slots == NULL) {
        return SDB_E_OUT_OF_MEMORY;
    }
    for (i = 0U; i < ix->capacity; ++i) {
        uint64_t page_id = ix->slots[i].page_id;
        size_t target;
        if (page_id == 0U) {
            continue;
        }
        target = sdb_wal_index_slot_for(new_slots, new_capacity, page_id);
        new_slots[target].page_id = page_id;
        new_slots[target].wal_offset = ix->slots[i].wal_offset;
    }
    free(ix->slots);
    ix->slots = new_slots;
    ix->capacity = new_capacity;
    return SDB_OK;
}

sdb_status sdb_wal_index_init(sdb_wal_index *ix)
{
    sdb_wal_index_slot *slots;

    if (ix == NULL) {
        return SDB_E_INVALID_ARGUMENT;
    }
    slots = (sdb_wal_index_slot *)calloc(
        SDB_WAL_INDEX_INITIAL_CAPACITY, sizeof(*slots)
    );
    if (slots == NULL) {
        return SDB_E_OUT_OF_MEMORY;
    }
    ix->slots = slots;
    ix->capacity = SDB_WAL_INDEX_INITIAL_CAPACITY;
    ix->size = 0U;
    return SDB_OK;
}

void sdb_wal_index_free(sdb_wal_index *ix)
{
    if (ix == NULL) {
        return;
    }
    free(ix->slots);
    ix->slots = NULL;
    ix->capacity = 0U;
    ix->size = 0U;
}

sdb_status sdb_wal_index_put(
    sdb_wal_index *ix, uint64_t page_id, uint64_t wal_offset
)
{
    size_t index;

    if (ix == NULL || ix->slots == NULL || page_id == 0U) {
        return SDB_E_INVALID_ARGUMENT;
    }
    /*
     * Probe first. WAL mode rewrites the offset of the SAME page on every
     * commit, so puts are update-heavy; growing on an update (which does not
     * add an entry) would keep doubling the table for no new keys. If the key
     * already has a slot, update it in place with no grow. The load factor
     * stays < 0.5 from the insert path below, so this initial probe always
     * lands on a match or an empty slot and terminates.
     */
    index = sdb_wal_index_slot_for(ix->slots, ix->capacity, page_id);
    if (ix->slots[index].page_id == page_id) {
        ix->slots[index].wal_offset = wal_offset;
        return SDB_OK;
    }
    /*
     * New key. Grow at ~0.5 load factor before inserting (size*2 >= capacity,
     * written that way to stay overflow-safe), then re-probe into the resized
     * table so the slot index is valid.
     */
    if (ix->size * 2U >= ix->capacity) {
        sdb_status status = sdb_wal_index_grow(ix);
        if (status != SDB_OK) {
            return status;
        }
        index = sdb_wal_index_slot_for(ix->slots, ix->capacity, page_id);
    }
    ix->slots[index].page_id = page_id;
    ix->slots[index].wal_offset = wal_offset;
    ix->size += 1U;
    return SDB_OK;
}

bool sdb_wal_index_get(
    const sdb_wal_index *ix, uint64_t page_id, uint64_t *offset_out
)
{
    size_t index;
    size_t mask;
    size_t probes;

    if (ix == NULL || ix->slots == NULL || page_id == 0U) {
        return false;
    }
    mask = ix->capacity - 1U;
    index = (size_t)(
        (page_id * SDB_WAL_INDEX_HASH_MUL) & (uint64_t)mask
    );
    for (probes = 0U; probes < ix->capacity; ++probes) {
        uint64_t slot_id = ix->slots[index].page_id;
        if (slot_id == 0U) {
            return false;
        }
        if (slot_id == page_id) {
            if (offset_out != NULL) {
                *offset_out = ix->slots[index].wal_offset;
            }
            return true;
        }
        index = (index + 1U) & mask;
    }
    return false;
}

void sdb_wal_index_clear(sdb_wal_index *ix)
{
    if (ix == NULL || ix->slots == NULL) {
        return;
    }
    memset(ix->slots, 0, ix->capacity * sizeof(*ix->slots));
    ix->size = 0U;
}
