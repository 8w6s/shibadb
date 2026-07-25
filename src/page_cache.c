#include "page_cache.h"

#include "crypto.h"

#include <stdlib.h>
#include <string.h>

sdb_status sdb_page_cache_init(
    sdb_page_cache *cache, size_t capacity, size_t page_size
)
{
    size_t index;
    if (cache == NULL || capacity == 0U || page_size == 0U) {
        return SDB_E_INVALID_ARGUMENT;
    }
    if (capacity > SIZE_MAX / page_size
        || capacity > SIZE_MAX / sizeof(*cache->entries)) {
        return SDB_E_OVERFLOW;
    }
    (void)memset(cache, 0, sizeof(*cache));
    cache->entries = (sdb_page_cache_entry *)calloc(
        capacity, sizeof(*cache->entries)
    );
    cache->storage = (uint8_t *)malloc(capacity * page_size);
    if (cache->entries == NULL || cache->storage == NULL) {
        sdb_page_cache_destroy(cache);
        return SDB_E_INTERNAL;
    }
    cache->capacity = capacity;
    cache->page_size = page_size;
    for (index = 0U; index < capacity; ++index) {
        cache->entries[index].bytes = cache->storage + (index * page_size);
    }
    return SDB_OK;
}

void sdb_page_cache_destroy(sdb_page_cache *cache)
{
    if (cache != NULL) {
        /*
         * Wipe cached page bytes before free. On plaintext DBs the
         * cache holds user data; on encrypted DBs it holds already-
         * decrypted plaintext. Leaving either in a freed heap region
         * is a disclosure surface for core dumps, swap-file writes,
         * and post-free heap-spray reads. Parity with sdb_txn_release
         * which secure-zeroes staged buffers.
         */
        if (cache->storage != NULL) {
            sdb_secure_zero(
                cache->storage, cache->capacity * cache->page_size
            );
        }
        free(cache->storage);
        free(cache->entries);
        (void)memset(cache, 0, sizeof(*cache));
    }
}

bool sdb_page_cache_get(
    sdb_page_cache *cache, uint64_t page_id, uint8_t *page_out
)
{
    size_t index;
    if (cache == NULL || page_out == NULL) {
        return false;
    }
    for (index = 0U; index < cache->capacity; ++index) {
        if (cache->entries[index].valid
            && cache->entries[index].page_id == page_id) {
            cache->entries[index].stamp = ++cache->clock;
            (void)memcpy(
                page_out, cache->entries[index].bytes, cache->page_size
            );
            return true;
        }
    }
    return false;
}

void sdb_page_cache_put(
    sdb_page_cache *cache, uint64_t page_id, const uint8_t *page
)
{
    size_t index;
    size_t victim = 0U;
    if (cache == NULL || page == NULL || cache->capacity == 0U) {
        return;
    }
    for (index = 0U; index < cache->capacity; ++index) {
        if (cache->entries[index].valid
            && cache->entries[index].page_id == page_id) {
            victim = index;
            break;
        }
        if (!cache->entries[index].valid) {
            victim = index;
            break;
        }
        if (cache->entries[index].stamp < cache->entries[victim].stamp) {
            victim = index;
        }
    }
    cache->entries[victim].page_id = page_id;
    cache->entries[victim].stamp = ++cache->clock;
    cache->entries[victim].valid = true;
    (void)memcpy(cache->entries[victim].bytes, page, cache->page_size);
}

void sdb_page_cache_remove(sdb_page_cache *cache, uint64_t page_id)
{
    size_t index;
    if (cache == NULL) {
        return;
    }
    for (index = 0U; index < cache->capacity; ++index) {
        if (cache->entries[index].valid
            && cache->entries[index].page_id == page_id) {
            cache->entries[index].valid = false;
            /*
             * Also zero the stale id so a subsequent put cannot key off it and
             * insert a duplicate valid entry at a lower slot.
             */
            cache->entries[index].page_id = 0U;
            return;
        }
    }
}
