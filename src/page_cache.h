#ifndef SHIBADB_PAGE_CACHE_H
#define SHIBADB_PAGE_CACHE_H

#include "internal.h"

typedef struct sdb_page_cache_entry {
    uint64_t page_id;
    uint64_t stamp;
    uint8_t *bytes;
    bool valid;
} sdb_page_cache_entry;

typedef struct sdb_page_cache {
    sdb_page_cache_entry *entries;
    uint8_t *storage;
    size_t capacity;
    size_t page_size;
    uint64_t clock;
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
