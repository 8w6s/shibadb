#include "page.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    sdb_page_view view;
    uint8_t *page = NULL;
    size_t page_size;
    uint64_t expected_ids[4] = { 1U, 2U, UINT64_C(0xffffffff), UINT64_C(1) << 20 };
    unsigned selector;

    if (size < 2U) {
        return 0;
    }

    switch (data[0] & 0x07U) {
    case 0U: page_size = 512U; break;
    case 1U: page_size = 1024U; break;
    case 2U: page_size = 2048U; break;
    case 3U: page_size = 4096U; break;
    case 4U: page_size = 8192U; break;
    case 5U: page_size = 16384U; break;
    case 6U: page_size = 32768U; break;
    default: page_size = 65536U; break;
    }
    if (page_size < SDB_MIN_PAGE_SIZE || page_size > SDB_MAX_PAGE_SIZE) {
        return 0;
    }
    selector = (unsigned)data[1] & 0x03U;

    page = (uint8_t *)malloc(page_size);
    if (page == NULL) {
        return 0;
    }
    if (size - 2U >= page_size) {
        (void)memcpy(page, data + 2U, page_size);
    } else {
        (void)memcpy(page, data + 2U, size - 2U);
        (void)memset(page + (size - 2U), 0, page_size - (size - 2U));
    }

    (void)sdb_page_decode(page, page_size, expected_ids[selector], &view);
    free(page);
    return 0;
}
