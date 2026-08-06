#include "file.h"
#include "internal.h"
#include "key_manager.h"
#include "pager.h"
#include "superblock_store.h"

#include "shibadb.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const uint8_t password[] = "legacy-upgrade-shrink-password";

#define PLEN (sizeof(password) - 1U)
#define ROOT_PAGE_OFFSET ((size_t)40)
#define FREELIST_PAGE_OFFSET ((size_t)48)
#define NEXT_PAGE_ID_OFFSET ((size_t)92)
#define DATA_OFFSET \
    ((uint64_t)(SDB_SUPERBLOCK_SLOT_SIZE * SDB_SUPERBLOCK_SLOT_COUNT))

static const uint8_t marker[] = "LEGACY-PAGE-MUST-SURVIVE";

static uint64_t expected_size_for(uint64_t next_page_id)
{
    return DATA_OFFSET + (next_page_id - 1U) * (uint64_t)SDB_MIN_PAGE_SIZE;
}

static void remove_db(const char *path)
{
    char wal[256];
    (void)remove(path);
    (void)snprintf(wal, sizeof(wal), "%s.wal", path);
    (void)remove(wal);
}

/*
 * Build a legacy (SDB_FLAG_ENCRYPTED, HEADER_AUTH CLEAR) encrypted database
 * whose on-disk size matches next_page_id exactly (a cleanly-closed legacy DB).
 * The last page carries a recognisable marker so a truncation is observable.
 */
static void build_clean_legacy(
    const char *path, uint64_t next_page_id, uint64_t root_page
)
{
    sdb_superblock_v1 sb;
    uint8_t data_key[32];
    sdb_file f;
    uint64_t expected;
    size_t i;

    remove_db(path);
    (void)memset(&sb, 0, sizeof(sb));
    sb.page_size = SDB_MIN_PAGE_SIZE;
    sb.flags = SDB_FLAG_ENCRYPTED;
    sb.generation = 1U;
    sb.checkpoint_lsn = 0U;
    sb.root_page = root_page;
    sb.freelist_page = 0U;
    sb.next_page_id = next_page_id;
    sb.key_wrap_id = 1U;
    sb.kdf_iterations = SDB_MIN_KDF_ITERATIONS;
    for (i = 0U; i < SDB_SALT_SIZE; ++i) {
        sb.salt[i] = (uint8_t)(0x30U + i);
    }
    for (i = 0U; i < SDB_FILE_ID_SIZE; ++i) {
        sb.file_id[i] = (uint8_t)(0x50U + i);
    }
    for (i = 0U; i < sizeof(data_key); ++i) {
        data_key[i] = (uint8_t)(0x80U + i);
    }
    assert(sdb_key_wrap(&sb, password, PLEN, data_key) == SDB_OK);
    assert(sdb_superblock_store_create(path, &sb) == SDB_OK);

    expected = expected_size_for(next_page_id);
    assert(sdb_file_open_existing(path, true, &f) == SDB_OK);
    assert(sdb_file_resize(&f, expected) == SDB_OK);
    {
        uint8_t page[SDB_MIN_PAGE_SIZE];
        (void)memset(page, 0, sizeof(page));
        (void)memcpy(page, marker, sizeof(marker));
        assert(sdb_file_write_full(
            &f, expected - (uint64_t)SDB_MIN_PAGE_SIZE, page, sizeof(page)
        ) == SDB_OK);
    }
    assert(sdb_file_sync(&f) == SDB_OK);
    assert(sdb_file_close(&f) == SDB_OK);
}

/*
 * Overwrite a u64 header field in both mirrors and repair the keyless CRC32,
 * exactly as an offline attacker without the password would.
 */
static void tamper_u64(const char *path, size_t field_offset, uint64_t value)
{
    sdb_file f;
    uint8_t slot;

    assert(sdb_file_open_existing(path, true, &f) == SDB_OK);
    for (slot = 0U; slot < (uint8_t)SDB_SUPERBLOCK_SLOT_COUNT; ++slot) {
        uint8_t bytes[SDB_SUPERBLOCK_SLOT_SIZE];
        const uint64_t offset =
            (uint64_t)slot * (uint64_t)SDB_SUPERBLOCK_SLOT_SIZE;
        uint32_t crc;
        assert(sdb_file_read_full(&f, offset, bytes, sizeof(bytes)) == SDB_OK);
        sdb_write_u64_le(bytes + field_offset, value);
        (void)memset(
            bytes + SDB_SUPERBLOCK_CHECKSUM_OFFSET, 0,
            SDB_SUPERBLOCK_CHECKSUM_SIZE
        );
        crc = sdb_crc32(bytes, SDB_SUPERBLOCK_HEADER_SIZE);
        sdb_write_u32_le(bytes + SDB_SUPERBLOCK_CHECKSUM_OFFSET, crc);
        assert(sdb_file_write_full(&f, offset, bytes, sizeof(bytes)) == SDB_OK);
    }
    assert(sdb_file_sync(&f) == SDB_OK);
    assert(sdb_file_close(&f) == SDB_OK);
}

static uint64_t file_size_of(const char *path)
{
    sdb_file f;
    uint64_t size;
    assert(sdb_file_open_existing(path, false, &f) == SDB_OK);
    assert(sdb_file_size(&f, &size) == SDB_OK);
    assert(sdb_file_close(&f) == SDB_OK);
    return size;
}

/*
 * Attack: lower next_page_id below the real page count on an unauthenticated
 * legacy header. Before the fix the first open trusts the forged geometry,
 * seals it under HEADER_AUTH, and canonicalize shrinks the file — destroying
 * the trailing pages. After the fix the open is refused with SDB_E_CORRUPT
 * and the file is left intact.
 */
static void test_tamper_next_page_id_truncates(void)
{
    static const char *path = "test-legacy-shrink-truncate.tmp";
    sdb_pager pager;
    uint64_t before;
    uint64_t after;
    sdb_status rc;

    build_clean_legacy(path, 6U, 1U);
    tamper_u64(path, NEXT_PAGE_ID_OFFSET, 3U);
    before = file_size_of(path);

    rc = sdb_pager_open_encrypted(path, password, PLEN, &pager);
    if (rc == SDB_OK) {
        assert(sdb_pager_close(&pager) == SDB_OK);
    }
    after = file_size_of(path);
    (void)fprintf(
        stderr,
        "[truncate] rc=%d before=%llu after=%llu expected_after=%llu\n",
        (int)rc,
        (unsigned long long)before,
        (unsigned long long)after,
        (unsigned long long)expected_size_for(3U)
    );

    assert(rc == SDB_E_CORRUPT);
    assert(after == before);
    remove_db(path);
}

/*
 * root_page must be < next_page_id (0 = "none"). A forged root_page pointing at
 * an unallocated slot must be rejected at decode time.
 */
static void test_root_page_out_of_range_rejected(void)
{
    static const char *path = "test-legacy-shrink-root.tmp";
    sdb_pager pager;
    sdb_status rc;

    build_clean_legacy(path, 6U, 1U);
    tamper_u64(path, ROOT_PAGE_OFFSET, 6U);
    rc = sdb_pager_open_encrypted(path, password, PLEN, &pager);
    if (rc == SDB_OK) {
        assert(sdb_pager_close(&pager) == SDB_OK);
    }
    assert(rc == SDB_E_CORRUPT);
    remove_db(path);
}

/* freelist_page must be < next_page_id (0 = "none"). */
static void test_freelist_page_out_of_range_rejected(void)
{
    static const char *path = "test-legacy-shrink-freelist.tmp";
    sdb_pager pager;
    sdb_status rc;

    build_clean_legacy(path, 6U, 1U);
    tamper_u64(path, FREELIST_PAGE_OFFSET, 6U);
    rc = sdb_pager_open_encrypted(path, password, PLEN, &pager);
    if (rc == SDB_OK) {
        assert(sdb_pager_close(&pager) == SDB_OK);
    }
    assert(rc == SDB_E_CORRUPT);
    remove_db(path);
}

/*
 * Regression guard: a genuine, untampered legacy database (actual == expected)
 * MUST still open, upgrade to HEADER_AUTH, keep its file intact, and reopen.
 */
static void test_valid_legacy_still_upgrades(void)
{
    static const char *path = "test-legacy-shrink-valid.tmp";
    sdb_pager pager;
    uint64_t before;
    uint64_t after;

    build_clean_legacy(path, 6U, 1U);
    before = file_size_of(path);

    assert(sdb_pager_open_encrypted(path, password, PLEN, &pager) == SDB_OK);
    assert((pager.superblock.flags & SDB_FLAG_HEADER_AUTH) != 0U);
    assert(pager.superblock.next_page_id == 6U);
    assert(sdb_pager_close(&pager) == SDB_OK);

    after = file_size_of(path);
    assert(after == before);

    assert(sdb_pager_open_encrypted(path, password, PLEN, &pager) == SDB_OK);
    assert((pager.superblock.flags & SDB_FLAG_HEADER_AUTH) != 0U);
    assert(sdb_pager_close(&pager) == SDB_OK);
    remove_db(path);
}

/*
 * A legitimate abandoned trailing allocation (invariant R.3): a clean legacy DB
 * crashed after ftruncate grew the file but before the superblock advance, so
 * next_page_id is untampered and the extra trailing pages are entirely zero.
 * The open must SUCCEED, reclaim (truncate) the orphan back to the size implied
 * by next_page_id, upgrade to HEADER_AUTH, and reopen — not refuse.
 */
static void test_legacy_zero_orphan_tail_reclaimed(void)
{
    static const char *path = "test-legacy-shrink-orphan.tmp";
    sdb_pager pager;
    uint64_t clean_expected;
    uint64_t after;
    sdb_file f;

    build_clean_legacy(path, 6U, 1U);
    clean_expected = file_size_of(path);
    assert(sdb_file_open_existing(path, true, &f) == SDB_OK);
    assert(sdb_file_resize(
        &f, clean_expected + 2U * (uint64_t)SDB_MIN_PAGE_SIZE
    ) == SDB_OK);
    assert(sdb_file_sync(&f) == SDB_OK);
    assert(sdb_file_close(&f) == SDB_OK);

    assert(sdb_pager_open_encrypted(path, password, PLEN, &pager) == SDB_OK);
    assert((pager.superblock.flags & SDB_FLAG_HEADER_AUTH) != 0U);
    assert(pager.superblock.next_page_id == 6U);
    assert(sdb_pager_close(&pager) == SDB_OK);

    after = file_size_of(path);
    assert(after == clean_expected);

    assert(sdb_pager_open_encrypted(path, password, PLEN, &pager) == SDB_OK);
    assert((pager.superblock.flags & SDB_FLAG_HEADER_AUTH) != 0U);
    assert(sdb_pager_close(&pager) == SDB_OK);
    remove_db(path);
}

/*
 * A trailing region carrying real (non-zero) bytes beyond the size implied by
 * next_page_id must NOT be reclaimed on an unauthenticated legacy open, even
 * with no next_page_id tamper: it may be the victim's data isolated by a lowered
 * geometry. Refuse and leave the file intact.
 */
static void test_legacy_nonzero_tail_refused(void)
{
    static const char *path = "test-legacy-shrink-nonzero.tmp";
    sdb_pager pager;
    uint64_t before;
    uint64_t after;
    sdb_status rc;
    sdb_file f;
    uint8_t page[SDB_MIN_PAGE_SIZE];
    size_t i;

    build_clean_legacy(path, 6U, 1U);
    before = file_size_of(path);
    for (i = 0U; i < sizeof(page); ++i) {
        page[i] = (uint8_t)(0xA5U ^ (uint8_t)i);
    }
    assert(sdb_file_open_existing(path, true, &f) == SDB_OK);
    assert(sdb_file_write_full(&f, before, page, sizeof(page)) == SDB_OK);
    assert(sdb_file_sync(&f) == SDB_OK);
    assert(sdb_file_close(&f) == SDB_OK);
    before = file_size_of(path);

    rc = sdb_pager_open_encrypted(path, password, PLEN, &pager);
    if (rc == SDB_OK) {
        assert(sdb_pager_close(&pager) == SDB_OK);
    }
    after = file_size_of(path);
    assert(rc == SDB_E_CORRUPT);
    assert(after == before);
    remove_db(path);
}

int main(void)
{
    test_tamper_next_page_id_truncates();
    test_root_page_out_of_range_rejected();
    test_freelist_page_out_of_range_rejected();
    test_valid_legacy_still_upgrades();
    test_legacy_zero_orphan_tail_reclaimed();
    test_legacy_nonzero_tail_refused();
    (void)puts("legacy upgrade no-shrink: ok");
    return 0;
}
