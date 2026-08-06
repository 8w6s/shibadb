#include "page.h"

#include "internal.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#define TEST_PAGE_SIZE ((size_t)4096)
#define TEST_PAGE_CHECKSUM_OFFSET ((size_t)28)

/*
 * Recompute the page checksum exactly as sdb_page_encode does, so a mutated
 * field can be tested WITHOUT the checksum gate masking the intended reject
 * branch.
 */
static void reseal(uint8_t *page)
{
    sdb_write_u32_le(
        page + TEST_PAGE_CHECKSUM_OFFSET,
        sdb_crc32_zeroed_range(
            page, TEST_PAGE_SIZE, TEST_PAGE_CHECKSUM_OFFSET, sizeof(uint32_t)
        )
    );
}

static void test_roundtrip_and_corruption(void)
{
    uint8_t page[TEST_PAGE_SIZE];
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
}

/*
 * Each reject branch of sdb_page_decode must surface its SPECIFIC status
 * code. Branches whose gate runs before the checksum are resealed after the
 * mutation so the checksum stays valid and the rejection is proven to come
 * from that branch alone.
 */
static void test_reject_branches_have_specific_status(void)
{
    uint8_t page[TEST_PAGE_SIZE];
    static const uint8_t payload[] = {1U, 2U, 3U, 4U, 5U};
    sdb_page_view view;

    /* bad magic -> SDB_E_BAD_MAGIC (distinct from generic corruption) */
    assert(sdb_page_encode(
        page, sizeof(page), (uint16_t)SDB_PAGE_TYPE_DATA,
        7U, 11U, payload, sizeof(payload)
    ) == SDB_OK);
    page[0] ^= UINT8_C(1);
    assert(sdb_page_decode(page, sizeof(page), 7U, &view) == SDB_E_BAD_MAGIC);

    /* unknown type -> SDB_E_CORRUPT */
    assert(sdb_page_encode(
        page, sizeof(page), (uint16_t)SDB_PAGE_TYPE_DATA,
        7U, 11U, payload, sizeof(payload)
    ) == SDB_OK);
    sdb_write_u16_le(page + 4U, UINT16_C(0));
    reseal(page);
    assert(sdb_page_decode(page, sizeof(page), 7U, &view) == SDB_E_CORRUPT);

    /* nonzero flags -> SDB_E_CORRUPT */
    assert(sdb_page_encode(
        page, sizeof(page), (uint16_t)SDB_PAGE_TYPE_DATA,
        7U, 11U, payload, sizeof(payload)
    ) == SDB_OK);
    sdb_write_u16_le(page + 6U, UINT16_C(1));
    reseal(page);
    assert(sdb_page_decode(page, sizeof(page), 7U, &view) == SDB_E_CORRUPT);

    /* page_id mismatch -> SDB_E_CORRUPT */
    assert(sdb_page_encode(
        page, sizeof(page), (uint16_t)SDB_PAGE_TYPE_DATA,
        7U, 11U, payload, sizeof(payload)
    ) == SDB_OK);
    assert(sdb_page_decode(page, sizeof(page), 8U, &view) == SDB_E_CORRUPT);

    /* payload_size beyond the page body -> SDB_E_CORRUPT */
    assert(sdb_page_encode(
        page, sizeof(page), (uint16_t)SDB_PAGE_TYPE_DATA,
        7U, 11U, payload, sizeof(payload)
    ) == SDB_OK);
    sdb_write_u32_le(
        page + 24U, (uint32_t)(sizeof(page) - SDB_PAGE_HEADER_SIZE + 1U)
    );
    reseal(page);
    assert(sdb_page_decode(page, sizeof(page), 7U, &view) == SDB_E_CORRUPT);

    /* checksum mismatch -> SDB_E_CORRUPT (flip a payload byte, no reseal) */
    assert(sdb_page_encode(
        page, sizeof(page), (uint16_t)SDB_PAGE_TYPE_DATA,
        7U, 11U, payload, sizeof(payload)
    ) == SDB_OK);
    page[SDB_PAGE_HEADER_SIZE] ^= UINT8_C(1);
    assert(sdb_page_decode(page, sizeof(page), 7U, &view) == SDB_E_CORRUPT);

    /* nonzero byte in the zero-fill after the payload -> SDB_E_CORRUPT */
    assert(sdb_page_encode(
        page, sizeof(page), (uint16_t)SDB_PAGE_TYPE_DATA,
        7U, 11U, payload, sizeof(payload)
    ) == SDB_OK);
    page[SDB_PAGE_HEADER_SIZE + sizeof(payload)] ^= UINT8_C(1);
    reseal(page);
    assert(sdb_page_decode(page, sizeof(page), 7U, &view) == SDB_E_CORRUPT);
}

static void test_payload_size_boundaries(void)
{
    uint8_t page[TEST_PAGE_SIZE];
    static uint8_t big[4096 - 32];
    sdb_page_view view;
    size_t index;

    /* zero-length payload round-trips */
    assert(sdb_page_encode(
        page, sizeof(page), (uint16_t)SDB_PAGE_TYPE_DATA, 7U, 11U, NULL, 0U
    ) == SDB_OK);
    assert(sdb_page_decode(page, sizeof(page), 7U, &view) == SDB_OK);
    assert(view.payload_size == 0U);

    /* maximal payload (page_size - header) round-trips */
    for (index = 0U; index < sizeof(big); ++index) {
        big[index] = (uint8_t)(index & 0xffU);
    }
    assert(sizeof(big) == sizeof(page) - SDB_PAGE_HEADER_SIZE);
    assert(sdb_page_encode(
        page, sizeof(page), (uint16_t)SDB_PAGE_TYPE_DATA,
        7U, 11U, big, sizeof(big)
    ) == SDB_OK);
    assert(sdb_page_decode(page, sizeof(page), 7U, &view) == SDB_OK);
    assert(view.payload_size == (uint32_t)sizeof(big));
    assert(memcmp(view.payload, big, sizeof(big)) == 0);

    /* one byte past the maximum is rejected at encode time */
    assert(sdb_page_encode(
        page, sizeof(page), (uint16_t)SDB_PAGE_TYPE_DATA,
        7U, 11U, big, sizeof(big) + 1U
    ) == SDB_E_INVALID_ARGUMENT);
}

static void test_encode_rejects_invalid_arguments(void)
{
    uint8_t page[TEST_PAGE_SIZE];
    static const uint8_t payload[] = {1U, 2U, 3U};

    /* page_id must be nonzero */
    assert(sdb_page_encode(
        page, sizeof(page), (uint16_t)SDB_PAGE_TYPE_DATA,
        0U, 11U, payload, sizeof(payload)
    ) == SDB_E_INVALID_ARGUMENT);
    /* unknown page type */
    assert(sdb_page_encode(
        page, sizeof(page), UINT16_C(0),
        7U, 11U, payload, sizeof(payload)
    ) == SDB_E_INVALID_ARGUMENT);
    /* NULL payload with nonzero size */
    assert(sdb_page_encode(
        page, sizeof(page), (uint16_t)SDB_PAGE_TYPE_DATA,
        7U, 11U, NULL, 1U
    ) == SDB_E_INVALID_ARGUMENT);
    /* non-power-of-two page size */
    assert(sdb_page_encode(
        page, 4097U, (uint16_t)SDB_PAGE_TYPE_DATA,
        7U, 11U, payload, sizeof(payload)
    ) == SDB_E_INVALID_ARGUMENT);
    /* NULL page buffer */
    assert(sdb_page_encode(
        NULL, sizeof(page), (uint16_t)SDB_PAGE_TYPE_DATA,
        7U, 11U, payload, sizeof(payload)
    ) == SDB_E_INVALID_ARGUMENT);
}

int main(void)
{
    test_roundtrip_and_corruption();
    test_reject_branches_have_specific_status();
    test_payload_size_boundaries();
    test_encode_rejects_invalid_arguments();
    (void)puts("page tests: ok");
    return 0;
}
