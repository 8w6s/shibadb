/* tests/test_wal_format.c */
#include "wal.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_commit_rec_roundtrip(void)
{
    sdb_wal_commit_rec rec = { .txn_id = 42U, .frame_count = 3U,
                               .next_page_id = 10U, .freelist_page = 4U };
    uint8_t buf[SDB_WAL_COMMIT_SIZE];
    sdb_wal_commit_rec got;
    uint32_t crc = 0U;
    sdb_wal_encode_commit_rec(buf, &rec, 0xDEADBEEFU);
    assert(sdb_wal_decode_commit_rec(buf, sizeof(buf), &got, &crc) == SDB_OK);
    assert(got.txn_id == 42U && got.frame_count == 3U);
    assert(got.next_page_id == 10U && got.freelist_page == 4U);
    assert(crc == 0xDEADBEEFU);
    buf[8] ^= 0xFFU; /* tamper */
    assert(sdb_wal_decode_commit_rec(buf, sizeof(buf), &got, &crc)
           == SDB_E_CORRUPT);
    (void)puts("wal commit-rec roundtrip: ok");
}

int main(void)
{
    test_commit_rec_roundtrip();
    (void)puts("wal format: ok");
    return 0;
}
