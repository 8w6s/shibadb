#include "encrypted_page.h"
#include "pager.h"
#include "wal.h"

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
            sdb_file wal;
            uint8_t raw[4096];
            uint8_t value = (uint8_t)iteration;
            sdb_page_view outer;
            size_t previous;
            uint64_t frame_offset = 0U;
            assert(sdb_txn_begin(&pager, &txn) == SDB_OK);
            assert(sdb_txn_put(
                &txn,
                page_id,
                (uint16_t)SDB_PAGE_TYPE_DATA,
                &value,
                sizeof(value)
            ) == SDB_OK);
            assert(sdb_txn_commit(&txn) == SDB_OK);
            /*
             * WAL-mode: commit no longer writes the encrypted page bytes
             * to the data file — the fresh nonce ends up in the WAL frame
             * instead. Read the frame's page bytes to inspect the
             * per-commit nonce.
             */
            assert(sdb_wal_index_get(
                &pager.wal_index, page_id, &frame_offset
            ));
            assert(sdb_file_open_existing(wal_path, false, &wal) == SDB_OK);
            assert(sdb_file_read_full(
                &wal,
                frame_offset + (uint64_t)SDB_WAL_RECORD_HEADER_SIZE_V5,
                raw,
                sizeof(raw)
            ) == SDB_OK);
            assert(sdb_file_close(&wal) == SDB_OK);
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
        /*
         * Close and reopen before verifying: reading through the live pager
         * would be served from the page cache (plaintext) and never exercise
         * decryption. The reopen starts with an empty cache and replays the
         * WAL into the data file, so verify_value must decode and decrypt the
         * persisted ciphertext to recover the last committed value.
         */
        assert(sdb_pager_close(&pager) == SDB_OK);
        assert(sdb_pager_open_encrypted(
            test_path,
            old_password,
            sizeof(old_password) - 1U,
            &pager
        ) == SDB_OK);
        verify_value(&pager, page_id, 127U);
        assert(sdb_pager_close(&pager) == SDB_OK);
        for (iteration = 0U; iteration < sizeof(pager.data_key); ++iteration) {
            assert(pager.data_key[iteration] == 0U);
        }
    }

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

    {
        sdb_pager pager;
        sdb_file file;
        uint64_t page_id;
        uint8_t raw[4096];
        uint8_t payload[4096];
        uint8_t value = 0x5aU;
        sdb_page_view outer;
        size_t payload_size;
        uint64_t page_lsn;
        const uint64_t offset =
            (uint64_t)(SDB_SUPERBLOCK_SLOT_SIZE
                * SDB_SUPERBLOCK_SLOT_COUNT);
        create_pager(&pager, 0x40U, &page_id);
        assert(page_id == 1U);
        assert(sdb_pager_write(
            &pager,
            page_id,
            (uint16_t)SDB_PAGE_TYPE_DATA,
            42U,
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
        assert(outer.type == (uint16_t)SDB_PAGE_TYPE_ENCRYPTED);
        payload_size = (size_t)outer.payload_size;
        page_lsn = outer.page_lsn;
        (void)memcpy(payload, outer.payload, payload_size);
        /*
         * Re-encode the *unmodified* ciphertext+tag under a bumped page_lsn.
         * The outer page frame stays structurally valid (page_id matches, the
         * checksum is recomputed by sdb_page_encode), so sdb_page_decode
         * succeeds — but page_lsn is folded into the AEAD associated data, so
         * the authentication tag no longer verifies. This proves the AAD
         * binding rejects a page relocated to a different LSN, distinct from
         * the raw ciphertext flip exercised above.
         */
        assert(sdb_page_encode(
            raw,
            sizeof(raw),
            (uint16_t)SDB_PAGE_TYPE_ENCRYPTED,
            page_id,
            page_lsn + 1U,
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

    {
        size_t boundary;
        for (boundary = 0U; boundary < 9U; ++boundary) {
            sdb_pager pager;
            uint64_t page_id;
            uint8_t value = 0x4dU;
            sdb_status rotate_status;
            sdb_status open_old;
            sdb_status open_new;
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
            rotate_status = sdb_pager_rotate_password(
                &pager,
                new_password,
                sizeof(new_password) - 1U,
                SDB_MIN_KDF_ITERATIONS
            );
            sdb_file_clear_failure_for_testing(&pager.file);
            assert(sdb_pager_close(&pager) == SDB_OK);
            open_old = sdb_pager_open_encrypted(
                test_path,
                old_password,
                sizeof(old_password) - 1U,
                &pager
            );
            if (open_old == SDB_OK) {
                verify_value(&pager, page_id, value);
                assert(sdb_pager_close(&pager) == SDB_OK);
            }
            open_new = sdb_pager_open_encrypted(
                test_path,
                new_password,
                sizeof(new_password) - 1U,
                &pager
            );
            if (open_new == SDB_OK) {
                verify_value(&pager, page_id, value);
                assert(sdb_pager_close(&pager) == SDB_OK);
            }
            if (rotate_status == SDB_OK) {
                /*
                 * No fault fired: the rotation is durable. The old password
                 * must no longer authenticate, and the new one must — a bare
                 * "something opened" check would miss a rotation that failed
                 * to re-wrap the key and still accepts the old password.
                 */
                assert(open_old == SDB_E_AUTHENTICATION);
                assert(open_new == SDB_OK);
            } else {
                /*
                 * Interrupted mid-write: recovery must land on exactly one
                 * consistent wrap slot. Both opening is impossible (two live
                 * wraps); neither opening is data loss. Exactly one wins.
                 */
                if ((open_old == SDB_OK) == (open_new == SDB_OK)) {
                    (void)fprintf(
                        stderr,
                        "rotate boundary=%zu status=%d old=%d new=%d\n",
                        boundary,
                        (int)rotate_status,
                        (int)open_old,
                        (int)open_new
                    );
                }
                assert((open_old == SDB_OK) != (open_new == SDB_OK));
            }
        }
    }
    (void)remove(wal_path);
    (void)remove(test_path);
    (void)puts("encryption fault tests: ok");
    return 0;
}
