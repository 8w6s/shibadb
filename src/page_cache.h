#ifndef SHIBADB_PAGE_CACHE_H
#define SHIBADB_PAGE_CACHE_H

#include "internal.h"

/*
 * A cache frame: one clean, plaintext page copy. `ref` is the CLOCK
 * second-chance reference bit; `valid` distinguishes an occupied frame
 * from a free one. `bytes` points into the contiguous `storage` block.
 */
typedef struct sdb_page_cache_entry {
    uint64_t page_id;
    uint8_t *bytes;
    bool valid;
    bool ref;
} sdb_page_cache_entry;

/*
 * Open-addressing hash slot mapping page_id -> frame index. A page_id of
 * 0 marks an empty slot (page 0 is the superblock and is never cached),
 * mirroring the sentinel convention used by src/wal_index.c.
 */
typedef struct sdb_page_cache_slot {
    uint64_t page_id;
    size_t entry_index;
} sdb_page_cache_slot;

typedef struct sdb_page_cache {
    sdb_page_cache_entry *entries; /* `capacity` frames */
    sdb_page_cache_slot *slots;    /* `slot_capacity` (power of two) */
    size_t *free_list;             /* stack of unused frame indices */
    uint8_t *storage;              /* capacity * page_size bytes */
    size_t capacity;
    size_t slot_capacity;
    size_t slot_mask;
    size_t free_count;
    size_t clock_hand;
    size_t page_size;
} sdb_page_cache;

sdb_status sdb_page_cache_init(
    sdb_page_cache *cache, size_t capacity, size_t page_size
);
void sdb_page_cache_destroy(sdb_page_cache *cache);
bool sdb_page_cache_get(
    sdb_page_cache *cache, uint64_t page_id, uint8_t *page_out
);
void sdb_page_cache_put(
    sdb_page_cache *cache, uint64_t page_id, const uint8_t *page
);
void sdb_page_cache_remove(sdb_page_cache *cache, uint64_t page_id);

#endif
