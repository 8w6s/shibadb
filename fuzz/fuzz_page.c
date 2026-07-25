#include "page.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/*
 * The prior fuzz target guarded decode behind `size == 4096` which meant
 * libFuzzer had no way to reach the decode path from an empty seed corpus
 * (any mutation stays under 4096 for millions of generations). Replace
 * with a corpus that:
 *   1. Selects a page_size from the first byte, mapping into the valid
 *      power-of-two range [SDB_MIN_PAGE_SIZE, SDB_MAX_PAGE_SIZE].
 *   2. Uses the remaining bytes as a template; pads with zeros or
 *      truncates to page_size to keep the decode API happy.
 *   3. Sweeps expected_page_id across a few values so the id-mismatch
 *      branch is reachable from any input.
 */
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
    /* map first byte to one of the supported page sizes */
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
