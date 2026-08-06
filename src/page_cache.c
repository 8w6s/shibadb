#include "page_cache.h"

#include "crypto.h"

#include <stdlib.h>
#include <string.h>

/*
 * Multiplicative (Fibonacci) hash constant, 2^64 / phi rounded. Matches
 * the multiplier used by src/wal_index.c and src/wal.c so the hashing
 * style stays consistent across the codebase.
 */
#define SDB_PAGE_CACHE_HASH_MUL UINT64_C(11400714819323198485)

static size_t sdb_page_cache_home(const sdb_page_cache *cache, uint64_t page_id)
{
    return (size_t)(
        (page_id * SDB_PAGE_CACHE_HASH_MUL) & (uint64_t)cache->slot_mask
    );
}

/*
 * Find the slot holding `page_id`, or SIZE_MAX if absent. The load factor
 * is held at or below 0.5, so probing always terminates at an empty slot.
 */
static size_t sdb_page_cache_find_slot(
    const sdb_page_cache *cache, uint64_t page_id
)
{
    size_t index = sdb_page_cache_home(cache, page_id);
    size_t probes;
    for (probes = 0U; probes < cache->slot_capacity; ++probes) {
        uint64_t slot_id = cache->slots[index].page_id;
        if (slot_id == 0U) {
            return SIZE_MAX;
        }
        if (slot_id == page_id) {
            return index;
        }
        index = (index + 1U) & cache->slot_mask;
    }
    return SIZE_MAX;
}

/* Insert (page_id -> frame) into the hash, assuming page_id is absent. */
static void sdb_page_cache_slot_insert(
    sdb_page_cache *cache, uint64_t page_id, size_t frame
)
{
    size_t index = sdb_page_cache_home(cache, page_id);
    while (cache->slots[index].page_id != 0U) {
        index = (index + 1U) & cache->slot_mask;
    }
    cache->slots[index].page_id = page_id;
    cache->slots[index].entry_index = frame;
}

/*
 * Backward-shift deletion (Knuth 6.4 algorithm R) for linear probing: after
 * emptying slot `i`, pull forward any following entry whose home slot lets
 * it fill the hole, so no probe chain is ever broken and no tombstone is
 * needed.
 */
static void sdb_page_cache_slot_delete_at(sdb_page_cache *cache, size_t i)
{
    size_t mask = cache->slot_mask;
    for (;;) {
        size_t j = i;
        cache->slots[i].page_id = 0U;
        for (;;) {
            size_t home;
            j = (j + 1U) & mask;
            if (cache->slots[j].page_id == 0U) {
                return;
            }
            home = sdb_page_cache_home(cache, cache->slots[j].page_id);
            /*
             * Keep slot j in place when its home is cyclically within the
             * open interval (i, j]; otherwise it may move back to fill i.
             */
            if (i <= j) {
                if (home > i && home <= j) {
                    continue;
                }
            } else {
                if (home > i || home <= j) {
                    continue;
                }
            }
            break;
        }
        cache->slots[i] = cache->slots[j];
        i = j;
    }
}

sdb_status sdb_page_cache_init(
    sdb_page_cache *cache, size_t capacity, size_t page_size
)
{
    size_t index;
    size_t slot_capacity;
    if (cache == NULL || capacity == 0U || page_size == 0U) {
        return SDB_E_INVALID_ARGUMENT;
    }
    if (capacity > SIZE_MAX / page_size
        || capacity > SIZE_MAX / sizeof(*cache->entries)
        || capacity > SIZE_MAX / sizeof(*cache->free_list)) {
        return SDB_E_OVERFLOW;
    }
    /*
     * Slot table: smallest power of two that keeps the load factor <= 0.5
     * (slot_capacity >= 2 * capacity). Guard against overflow before the
     * final doubling.
     */
    if (capacity > SIZE_MAX / 4U) {
        return SDB_E_OVERFLOW;
    }
    slot_capacity = 1U;
    while (slot_capacity < capacity * 2U) {
        slot_capacity <<= 1U;
    }
    if (slot_capacity > SIZE_MAX / sizeof(*cache->slots)) {
        return SDB_E_OVERFLOW;
    }
    (void)memset(cache, 0, sizeof(*cache));
    cache->entries = (sdb_page_cache_entry *)calloc(
        capacity, sizeof(*cache->entries)
    );
    cache->slots = (sdb_page_cache_slot *)calloc(
        slot_capacity, sizeof(*cache->slots)
    );
    cache->free_list = (size_t *)malloc(capacity * sizeof(*cache->free_list));
    cache->storage = (uint8_t *)malloc(capacity * page_size);
    if (cache->entries == NULL || cache->slots == NULL
        || cache->free_list == NULL || cache->storage == NULL) {
        sdb_page_cache_destroy(cache);
        return SDB_E_OUT_OF_MEMORY;
    }
    cache->capacity = capacity;
    cache->slot_capacity = slot_capacity;
    cache->slot_mask = slot_capacity - 1U;
    cache->page_size = page_size;
    cache->clock_hand = 0U;
    cache->free_count = capacity;
    for (index = 0U; index < capacity; ++index) {
        cache->entries[index].bytes = cache->storage + (index * page_size);
        cache->entries[index].valid = false;
        cache->entries[index].ref = false;
        /* Free-list is a stack; push frames so index 0 is popped first. */
        cache->free_list[index] = capacity - 1U - index;
    }
    return SDB_OK;
}

void sdb_page_cache_destroy(sdb_page_cache *cache)
{
    if (cache != NULL) {
        if (cache->storage != NULL) {
            sdb_secure_zero(
                cache->storage, cache->capacity * cache->page_size
            );
        }
        free(cache->storage);
        free(cache->entries);
        free(cache->slots);
        free(cache->free_list);
        (void)memset(cache, 0, sizeof(*cache));
    }
}

bool sdb_page_cache_get(
    sdb_page_cache *cache, uint64_t page_id, uint8_t *page_out
)
{
    size_t slot;
    size_t frame;
    if (cache == NULL || page_out == NULL || page_id == 0U
        || cache->slots == NULL) {
        return false;
    }
    slot = sdb_page_cache_find_slot(cache, page_id);
    if (slot == SIZE_MAX) {
        return false;
    }
    frame = cache->slots[slot].entry_index;
    cache->entries[frame].ref = true;
    (void)memcpy(page_out, cache->entries[frame].bytes, cache->page_size);
    return true;
}

/*
 * CLOCK second-chance: sweep the frames giving each a one-pass reprieve if
 * its ref bit is set, then evict the first frame with a clear ref bit.
 * Only called when the cache is full, so every frame is valid; the sweep
 * clears at most `capacity` ref bits before it must pick a victim, so the
 * cost is O(1) amortized. Removes the victim's hash slot and returns the
 * freed frame index (not pushed onto the free list — the caller reuses it).
 */
static size_t sdb_page_cache_clock_evict(sdb_page_cache *cache)
{
    for (;;) {
        size_t frame = cache->clock_hand;
        sdb_page_cache_entry *entry = &cache->entries[frame];
        cache->clock_hand = (cache->clock_hand + 1U) % cache->capacity;
        if (!entry->valid) {
            continue;
        }
        if (entry->ref) {
            entry->ref = false;
            continue;
        }
        {
            size_t slot = sdb_page_cache_find_slot(cache, entry->page_id);
            if (slot != SIZE_MAX) {
                sdb_page_cache_slot_delete_at(cache, slot);
            }
        }
        entry->valid = false;
        entry->page_id = 0U;
        return frame;
    }
}

void sdb_page_cache_put(
    sdb_page_cache *cache, uint64_t page_id, const uint8_t *page
)
{
    size_t slot;
    size_t frame;
    if (cache == NULL || page == NULL || page_id == 0U
        || cache->capacity == 0U || cache->slots == NULL) {
        return;
    }
    slot = sdb_page_cache_find_slot(cache, page_id);
    if (slot != SIZE_MAX) {
        /* Update in place: refresh the copy and grant a reference. */
        frame = cache->slots[slot].entry_index;
        (void)memcpy(cache->entries[frame].bytes, page, cache->page_size);
        cache->entries[frame].ref = true;
        return;
    }
    /* New page: take a free frame, else evict one via CLOCK. */
    if (cache->free_count > 0U) {
        frame = cache->free_list[--cache->free_count];
    } else {
        frame = sdb_page_cache_clock_evict(cache);
    }
    cache->entries[frame].page_id = page_id;
    cache->entries[frame].valid = true;
    cache->entries[frame].ref = false;
    (void)memcpy(cache->entries[frame].bytes, page, cache->page_size);
    /* Re-probe for the insertion slot: eviction may have shifted slots. */
    sdb_page_cache_slot_insert(cache, page_id, frame);
}

void sdb_page_cache_remove(sdb_page_cache *cache, uint64_t page_id)
{
    size_t slot;
    size_t frame;
    if (cache == NULL || page_id == 0U || cache->slots == NULL) {
        return;
    }
    slot = sdb_page_cache_find_slot(cache, page_id);
    if (slot == SIZE_MAX) {
        return;
    }
    frame = cache->slots[slot].entry_index;
    sdb_page_cache_slot_delete_at(cache, slot);
    cache->entries[frame].valid = false;
    cache->entries[frame].ref = false;
    cache->entries[frame].page_id = 0U;
    cache->free_list[cache->free_count++] = frame;
}
