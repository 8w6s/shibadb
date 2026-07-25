#include "encrypted_page.h"
#include "pager.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static const char *test_path = "test-encryption-fault.tmp";
static const char *wal_path = "test-encryption-fault.tmp.wal";
static const uint8_t old_password[] = "old-password-for-fault-tests";
static const uint8_t new_password[] = "new-password-for-fault-tests";

static void identity(uint8_t salt[16], uint8_t file_id[16], uint8_t seed)
{
    size_t index;
    for (index = 0U; index < 16U; ++index) {
        salt[index] = (uint8_t)(seed + index);
        file_id[index] = (uint8_t)(seed + 0x40U + index);
    }
}

static void create_pager(
    sdb_pager *pager, uint8_t seed, uint64_t *page_id_out
)
{
    uint8_t salt[16];
    uint8_t file_id[16];
    identity(salt, file_id, seed);
    (void)remove(test_path);
    (void)remove(wal_path);
    assert(sdb_pager_create_encrypted(
        test_path,
        4096U,
        salt,
        file_id,
        old_password,
        sizeof(old_password) - 1U,
        SDB_MIN_KDF_ITERATIONS,
        pager
    ) == SDB_OK);
    assert(sdb_pager_allocate(pager, page_id_out) == SDB_OK);
}

static void verify_value(sdb_pager *pager, uint64_t page_id, uint8_t expected)
{
    uint8_t page[4096];
    sdb_page_view view;
    assert(sdb_pager_read(
        pager, page_id, page, sizeof(page), &view
    ) == SDB_OK);
    assert(view.type == (uint16_t)SDB_PAGE_TYPE_DATA);
    assert(view.payload_size == 1U);
    assert(view.payload[0] == expected);
}

int main(void)
{
    {
        sdb_pager pager;
        uint64_t page_id;
        uint8_t nonces[128][24];
        size_t iteration;
        create_pager(&pager, 0x10U, &page_id);
        for (iteration = 0U; iteration < 128U; ++iteration) {
            sdb_txn txn;
            uint8_t raw[4096];
            uint8_t value = (uint8_t)iteration;
            sdb_page_view outer;
            size_t previous;
            const uint64_t offset =
                (uint64_t)(SDB_SUPERBLOCK_SLOT_SIZE
                    * SDB_SUPERBLOCK_SLOT_COUNT)
                + ((page_id - 1U) * 4096U);
            assert(sdb_txn_begin(&pager, &txn) == SDB_OK);
            assert(sdb_txn_put(
                &txn,
                page_id,
                (uint16_t)SDB_PAGE_TYPE_DATA,
                &value,
                sizeof(value)
            ) == SDB_OK);
            assert(sdb_txn_commit(&txn) == SDB_OK);
            assert(sdb_file_read_full(
                &pager.file, offset, raw, sizeof(raw)
            ) == SDB_OK);
            assert(sdb_page_decode(
                raw, sizeof(raw), page_id, &outer
            ) == SDB_OK);
            assert(outer.type == (uint16_t)SDB_PAGE_TYPE_ENCRYPTED);
            (void)memcpy(
                nonces[iteration], outer.payload + 8U, sizeof(nonces[0])
            );
            for (previous = 0U; previous < iteration; ++previous) {
                assert(memcmp(
                    nonces[previous],
                    nonces[iteration],
                    sizeof(nonces[0])
                ) != 0);
            }
        }
        verify_value(&pager, page_id, 127U);
        assert(sdb_pager_close(&pager) == SDB_OK);
        for (iteration = 0U; iteration < sizeof(pager.data_key); ++iteration) {
            assert(pager.data_key[iteration] == 0U);
        }
    }

    /* Modify authenticated ciphertext, then recompute the outer page CRC. */
    {
        sdb_pager pager;
        sdb_file file;
        uint64_t page_id;
        uint8_t raw[4096];
        uint8_t payload[4096];
        uint8_t value = 0x73U;
        sdb_page_view outer;
        size_t payload_size;
        uint64_t page_lsn;
        const uint64_t offset =
            (uint64_t)(SDB_SUPERBLOCK_SLOT_SIZE
                * SDB_SUPERBLOCK_SLOT_COUNT);
        create_pager(&pager, 0x30U, &page_id);
        assert(page_id == 1U);
        assert(sdb_pager_write(
            &pager,
            page_id,
            (uint16_t)SDB_PAGE_TYPE_DATA,
            99U,
            &value,
            sizeof(value)
        ) == SDB_OK);
        assert(sdb_pager_close(&pager) == SDB_OK);
        assert(sdb_file_open_existing(test_path, true, &file) == SDB_OK);
        assert(sdb_file_read_full(
            &file, offset, raw, sizeof(raw)
        ) == SDB_OK);
        assert(sdb_page_decode(
            raw, sizeof(raw), page_id, &outer
        ) == SDB_OK);
        payload_size = (size_t)outer.payload_size;
        page_lsn = outer.page_lsn;
        (void)memcpy(payload, outer.payload, payload_size);
        payload[SDB_ENCRYPTED_PAYLOAD_HEADER_SIZE] ^= 1U;
        assert(sdb_page_encode(
            raw,
            sizeof(raw),
            (uint16_t)SDB_PAGE_TYPE_ENCRYPTED,
            page_id,
            page_lsn,
            payload,
            payload_size
        ) == SDB_OK);
        assert(sdb_file_write_full(
            &file, offset, raw, sizeof(raw)
        ) == SDB_OK);
        assert(sdb_file_sync(&file) == SDB_OK);
        assert(sdb_file_close(&file) == SDB_OK);
        assert(sdb_pager_open_encrypted(
            test_path,
            old_password,
            sizeof(old_password) - 1U,
            &pager
        ) == SDB_OK);
        assert(sdb_pager_read(
            &pager, page_id, raw, sizeof(raw), &outer
        ) == SDB_E_CORRUPT);
        assert(sdb_pager_close(&pager) == SDB_OK);
    }

    /* Every interrupted mirror update must leave either password usable. */
    {
        size_t boundary;
        for (boundary = 0U; boundary < 9U; ++boundary) {
            sdb_pager pager;
            uint64_t page_id;
            uint8_t value = 0x4dU;
            bool opened = false;
            create_pager(&pager, (uint8_t)(0x50U + boundary), &page_id);
            assert(sdb_pager_write(
                &pager,
                page_id,
                (uint16_t)SDB_PAGE_TYPE_DATA,
                1U,
                &value,
                sizeof(value)
            ) == SDB_OK);
            sdb_file_fail_after_for_testing(&pager.file, boundary);
            (void)sdb_pager_rotate_password(
                &pager,
                new_password,
                sizeof(new_password) - 1U,
                SDB_MIN_KDF_ITERATIONS
            );
            sdb_file_clear_failure_for_testing(&pager.file);
            assert(sdb_pager_close(&pager) == SDB_OK);
            if (sdb_pager_open_encrypted(
                    test_path,
                    old_password,
                    sizeof(old_password) - 1U,
                    &pager
                ) == SDB_OK) {
                opened = true;
            } else if (sdb_pager_open_encrypted(
                    test_path,
                    new_password,
                    sizeof(new_password) - 1U,
                    &pager
                ) == SDB_OK) {
                opened = true;
            }
            assert(opened);
            verify_value(&pager, page_id, value);
            assert(sdb_pager_close(&pager) == SDB_OK);
        }
    }
    (void)remove(wal_path);
    (void)remove(test_path);
    (void)puts("encryption fault tests: ok");
    return 0;
}
