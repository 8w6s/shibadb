#include "wal_index.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
    sdb_wal_index ix;
    assert(sdb_wal_index_init(&ix) == SDB_OK);
    uint64_t off = 0U;
    assert(!sdb_wal_index_get(&ix, 7U, &off));
    assert(sdb_wal_index_put(&ix, 7U, 4096U) == SDB_OK);
    assert(sdb_wal_index_get(&ix, 7U, &off) && off == 4096U);
    assert(sdb_wal_index_put(&ix, 7U, 8192U) == SDB_OK);      /* upsert newest */
    assert(sdb_wal_index_get(&ix, 7U, &off) && off == 8192U);
    for (uint64_t i = 1U; i < 5000U; ++i) assert(sdb_wal_index_put(&ix, i, i*512U) == SDB_OK);
    assert(sdb_wal_index_get(&ix, 4321U, &off) && off == 4321U*512U);
    sdb_wal_index_clear(&ix);
    assert(!sdb_wal_index_get(&ix, 7U, &off));
    sdb_wal_index_free(&ix);
    (void)puts("wal index: ok"); return 0;
}
