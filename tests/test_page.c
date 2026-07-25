#include "page.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    uint8_t page[4096];
    static const uint8_t payload[] = {1U, 2U, 3U, 4U, 5U};
    sdb_page_view view;
    assert(sdb_page_encode(
        page, sizeof(page), (uint16_t)SDB_PAGE_TYPE_DATA,
        7U, 11U, payload, sizeof(payload)
    ) == SDB_OK);
    assert(sdb_page_decode(page, sizeof(page), 7U, &view) == SDB_OK);
    assert(view.type == (uint16_t)SDB_PAGE_TYPE_DATA);
    assert(view.page_id == 7U);
    assert(view.page_lsn == 11U);
    assert(view.payload_size == sizeof(payload));
    assert(memcmp(view.payload, payload, sizeof(payload)) == 0);
    assert(sdb_page_decode(page, sizeof(page), 8U, &view) == SDB_E_CORRUPT);
    page[100] ^= UINT8_C(1);
    assert(sdb_page_decode(page, sizeof(page), 7U, &view) == SDB_E_CORRUPT);
    (void)puts("page tests: ok");
    return 0;
}

