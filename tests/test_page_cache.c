#include "page_cache.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    sdb_page_cache cache;
    uint8_t page[32];
    uint8_t output[32];
    uint64_t id;

    assert(sdb_page_cache_init(&cache, 3U, sizeof(page)) == SDB_OK);
    assert(!sdb_page_cache_get(&cache, 1U, output));
    for (id = 1U; id <= 3U; ++id) {
        (void)memset(page, (int)id, sizeof(page));
        sdb_page_cache_put(&cache, id, page);
    }
    assert(sdb_page_cache_get(&cache, 1U, output));
    assert(output[0] == 1U);

    /* ID 2 is the least recently used entry and must be evicted. */
    (void)memset(page, 4, sizeof(page));
    sdb_page_cache_put(&cache, 4U, page);
    assert(!sdb_page_cache_get(&cache, 2U, output));
    assert(sdb_page_cache_get(&cache, 4U, output));
    assert(output[31] == 4U);

    sdb_page_cache_remove(&cache, 1U);
    assert(!sdb_page_cache_get(&cache, 1U, output));
    sdb_page_cache_destroy(&cache);
    (void)puts("page cache tests: ok");
    return 0;
}
