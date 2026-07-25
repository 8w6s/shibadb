#include "pager.h"

#include "encrypted_page.h"
#include "internal.h"
#include "key_manager.h"
#include "replace.h"
#include "superblock_store.h"

#include <stdlib.h>
#include <string.h>

static sdb_status sdb_pager_make_wal_path(
    const char *path, char **wal_path_out
)
{
    static const char suffix[] = ".wal";
    size_t path_size;
    size_t total_size;
    char *result;
    if (path == NULL || wal_path_out == NULL) {
        return SDB_E_INVALID_ARGUMENT;
    }
    path_size = strlen(path);
    if (!sdb_checked_add_size(path_size, sizeof(suffix), &total_size)) {
        return SDB_E_OVERFLOW;
    }
    result = (char *)malloc(total_size);
    if (result == NULL) {
        return SDB_E_INTERNAL;
    }
    (void)memcpy(result, path, path_size);
    (void)memcpy(result + path_size, suffix, sizeof(suffix));
    *wal_path_out = result;
    return SDB_OK;
}

static sdb_status sdb_pager_offset(
    const sdb_pager *pager, uint64_t page_id, uint64_t *offset_out
)
{
    const uint64_t data_offset =
        (uint64_t)(SDB_SUPERBLOCK_SLOT_SIZE * SDB_SUPERBLOCK_SLOT_COUNT);
    uint64_t relative;
    if (pager == NULL || offset_out == NULL || page_id == 0U) {
        return SDB_E_INVALID_ARGUMENT;
    }
    if (page_id - 1U > UINT64_MAX / (uint64_t)pager->superblock.page_size) {
        return SDB_E_OVERFLOW;
    }
    relative = (page_id - 1U) * (uint64_t)pager->superblock.page_size;
    if (relative > UINT64_MAX - data_offset) {
        return SDB_E_OVERFLOW;
    }
    *offset_out = data_offset + relative;
    return SDB_OK;
}

static sdb_status sdb_pager_advance_superblock(
    sdb_pager *pager, const sdb_superblock_v1 *next
)
{
    sdb_status status = sdb_superblock_store_update_file(&pager->file, next);
    if (status == SDB_OK) {
        pager->superblock = *next;
    }
    return status;
}

/*
 * Bump superblock->generation, refusing to wrap. UINT64_MAX generations
 * is far beyond any realistic database lifetime, but wrapping would
 * silently break the S.2 monotonicity invariant and the highest-valid-
 * generation recovery in S.3, so reject the operation instead.
 */
static sdb_status sdb_generation_bump(uint64_t *generation)
{
    if (generation == NULL || *generation == UINT64_MAX) {
        return SDB_E_OVERFLOW;
    }
    ++(*generation);
    return SDB_OK;
}

static sdb_status sdb_pager_canonicalize_file_size(sdb_pager *pager)
{
    const uint64_t data_offset =
        (uint64_t)(SDB_SUPERBLOCK_SLOT_SIZE * SDB_SUPERBLOCK_SLOT_COUNT);
    uint64_t page_count;
    uint64_t expected_size;
    uint64_t actual_size;
    sdb_status status;
    if (pager == NULL || pager->superblock.next_page_id == 0U) {
        return SDB_E_INVALID_ARGUMENT;
    }
    page_count = pager->superblock.next_page_id - 1U;
    if (page_count > (UINT64_MAX - data_offset)
            / (uint64_t)pager->superblock.page_size) {
        return SDB_E_CORRUPT;
    }
    expected_size = data_offset
        + (page_count * (uint64_t)pager->superblock.page_size);
    status = sdb_file_size(&pager->file, &actual_size);
    if (status != SDB_OK) {
        return status;
    }
    if (actual_size < expected_size) {
        return SDB_E_TRUNCATED;
    }
    if (actual_size == expected_size) {
        return SDB_OK;
    }
    status = sdb_file_resize(&pager->file, expected_size);
    if (status == SDB_OK) {
        status = sdb_file_sync(&pager->file);
    }
    return status;
}

static sdb_status sdb_pager_open_internal(
    const char *path,
    const uint8_t *password,
    size_t password_size,
    sdb_pager *pager_out
)
{
    sdb_superblock_read_result result;
    sdb_status status;
    if (path == NULL || pager_out == NULL) {
        return SDB_E_INVALID_ARGUMENT;
    }
    (void)memset(pager_out, 0, sizeof(*pager_out));
    status = sdb_file_open_existing(path, true, &pager_out->file);
    if (status != SDB_OK) {
        return status;
    }
    status = sdb_superblock_store_read_file(&pager_out->file, &result);
    if (status != SDB_OK) {
        (void)sdb_file_close(&pager_out->file);
        return status;
    }
    pager_out->superblock = result.superblock;
    status = sdb_replace_recover(
        path, pager_out->superblock.file_id
    );
    if (status != SDB_OK) {
        (void)sdb_file_close(&pager_out->file);
        return status;
    }
    if ((pager_out->superblock.flags & SDB_FLAG_ENCRYPTED) != 0U) {
        if (password == NULL || password_size == 0U) {
            (void)sdb_file_close(&pager_out->file);
            return SDB_E_AUTHENTICATION;
        }
        status = sdb_key_unwrap(
            &pager_out->superblock,
            password,
            password_size,
            pager_out->data_key
        );
        if (status != SDB_OK) {
            (void)sdb_file_close(&pager_out->file);
            return status;
        }
        pager_out->encryption_enabled = true;
    } else if (password != NULL || password_size != 0U) {
        (void)sdb_file_close(&pager_out->file);
        return SDB_E_INVALID_ARGUMENT;
    }
    status = sdb_pager_make_wal_path(path, &pager_out->wal_path);
    if (status != SDB_OK) {
        sdb_secure_zero(pager_out->data_key, sizeof(pager_out->data_key));
        (void)sdb_file_close(&pager_out->file);
        return status;
    }
    {
        uint64_t recovered_lsn;
        uint64_t recovered_next_page_id;
        uint64_t recovered_freelist_page;
        bool replayed;
        status = sdb_wal_recover(
            pager_out->wal_path,
            &pager_out->file,
            &pager_out->superblock,
            pager_out->encryption_enabled ? pager_out->data_key : NULL,
            &recovered_lsn,
            &recovered_next_page_id,
            &recovered_freelist_page,
            &replayed
        );
        if (status == SDB_OK
            && recovered_lsn > pager_out->superblock.checkpoint_lsn) {
            sdb_superblock_v1 next = pager_out->superblock;
            next.checkpoint_lsn = recovered_lsn;
            next.next_page_id = recovered_next_page_id;
            next.freelist_page = recovered_freelist_page;
            status = sdb_generation_bump(&next.generation);
            if (status == SDB_OK) {
                status = sdb_superblock_store_update_file(
                    &pager_out->file, &next
                );
            }
            if (status == SDB_OK) {
                pager_out->superblock = next;
            }
        }
        if (status == SDB_OK) {
            status = sdb_pager_canonicalize_file_size(pager_out);
        }
        if (status == SDB_OK) {
            status = sdb_wal_clear(pager_out->wal_path);
        }
        (void)replayed;
    }
    if (status != SDB_OK) {
        sdb_secure_zero(pager_out->data_key, sizeof(pager_out->data_key));
        free(pager_out->wal_path);
        pager_out->wal_path = NULL;
        (void)sdb_file_close(&pager_out->file);
        return status;
    }
    status = sdb_page_cache_init(
        &pager_out->cache,
        (size_t)SDB_PAGER_CACHE_CAPACITY,
        (size_t)pager_out->superblock.page_size
    );
    if (status != SDB_OK) {
        sdb_secure_zero(pager_out->data_key, sizeof(pager_out->data_key));
        free(pager_out->wal_path);
        pager_out->wal_path = NULL;
        (void)sdb_file_close(&pager_out->file);
        return status;
    }
    pager_out->open = true;
    return SDB_OK;
}

sdb_status sdb_pager_open(const char *path, sdb_pager *pager_out)
{
    return sdb_pager_open_internal(path, NULL, 0U, pager_out);
}

sdb_status sdb_pager_open_encrypted(
    const char *path,
    const uint8_t *password,
    size_t password_size,
    sdb_pager *pager_out
)
{
    if (password == NULL || password_size == 0U) {
        return SDB_E_INVALID_ARGUMENT;
    }
    return sdb_pager_open_internal(
        path, password, password_size, pager_out
    );
}

sdb_status sdb_pager_create(
    const char *path,
    uint32_t page_size,
    const uint8_t salt[SDB_SALT_SIZE],
    const uint8_t file_id[SDB_FILE_ID_SIZE],
    sdb_pager *pager_out
)
{
    sdb_superblock_v1 superblock;
    sdb_status status;
    if (path == NULL || salt == NULL || file_id == NULL || pager_out == NULL) {
        return SDB_E_INVALID_ARGUMENT;
    }
    (void)memset(&superblock, 0, sizeof(superblock));
    superblock.page_size = page_size;
    superblock.generation = 1U;
    superblock.next_page_id = 1U;
    (void)memcpy(superblock.salt, salt, SDB_SALT_SIZE);
    (void)memcpy(superblock.file_id, file_id, SDB_FILE_ID_SIZE);
    status = sdb_superblock_store_create(path, &superblock);
    if (status != SDB_OK) {
        return status;
    }
    {
        char *wal_path;
        status = sdb_pager_make_wal_path(path, &wal_path);
        if (status != SDB_OK) {
            return status;
        }
        status = sdb_wal_clear(wal_path);
        free(wal_path);
        if (status != SDB_OK) {
            return status;
        }
    }
    return sdb_pager_open(path, pager_out);
}

sdb_status sdb_pager_create_encrypted(
    const char *path,
    uint32_t page_size,
    const uint8_t salt[SDB_SALT_SIZE],
    const uint8_t file_id[SDB_FILE_ID_SIZE],
    const uint8_t *password,
    size_t password_size,
    uint32_t kdf_iterations,
    sdb_pager *pager_out
)
{
    sdb_superblock_v1 superblock;
    uint8_t data_key[32];
    char *wal_path;
    sdb_status status;
    if (path == NULL || salt == NULL || file_id == NULL
        || password == NULL || password_size == 0U || pager_out == NULL
        || kdf_iterations < SDB_MIN_KDF_ITERATIONS
        || kdf_iterations > SDB_MAX_KDF_ITERATIONS) {
        return SDB_E_INVALID_ARGUMENT;
    }
    (void)memset(&superblock, 0, sizeof(superblock));
    superblock.page_size = page_size;
    superblock.flags = SDB_FLAG_ENCRYPTED;
    superblock.generation = 1U;
    superblock.next_page_id = 1U;
    superblock.key_wrap_id = 1U;
    superblock.kdf_iterations = kdf_iterations;
    (void)memcpy(superblock.salt, salt, SDB_SALT_SIZE);
    (void)memcpy(superblock.file_id, file_id, SDB_FILE_ID_SIZE);
    status = sdb_random_bytes(data_key, sizeof(data_key));
    if (status == SDB_OK) {
        status = sdb_key_wrap(
            &superblock, password, password_size, data_key
        );
    }
    if (status == SDB_OK) {
        status = sdb_superblock_store_create(path, &superblock);
    }
    sdb_secure_zero(data_key, sizeof(data_key));
    if (status != SDB_OK) {
        return status;
    }
    status = sdb_pager_make_wal_path(path, &wal_path);
    if (status == SDB_OK) {
        status = sdb_wal_clear(wal_path);
        free(wal_path);
    }
    if (status != SDB_OK) {
        return status;
    }
    return sdb_pager_open_encrypted(
        path, password, password_size, pager_out
    );
}

sdb_status sdb_pager_close(sdb_pager *pager)
{
    sdb_status status;
    if (pager == NULL || !pager->open || pager->transaction_active) {
        return SDB_E_INVALID_ARGUMENT;
    }
    status = sdb_file_close(&pager->file);
    sdb_page_cache_destroy(&pager->cache);
    free(pager->wal_path);
    pager->wal_path = NULL;
    sdb_secure_zero(pager->data_key, sizeof(pager->data_key));
    pager->encryption_enabled = false;
    pager->open = false;
    return status;
}

size_t sdb_pager_payload_capacity(const sdb_pager *pager)
{
    size_t capacity;
    if (pager == NULL
        || (size_t)pager->superblock.page_size < SDB_PAGE_HEADER_SIZE) {
        return 0U;
    }
    capacity = (size_t)pager->superblock.page_size - SDB_PAGE_HEADER_SIZE;
    if (pager->encryption_enabled) {
        if (capacity < SDB_ENCRYPTED_PAYLOAD_HEADER_SIZE) {
            return 0U;
        }
        capacity -= SDB_ENCRYPTED_PAYLOAD_HEADER_SIZE;
    }
    return capacity;
}

sdb_status sdb_pager_rotate_password(
    sdb_pager *pager,
    const uint8_t *new_password,
    size_t new_password_size,
    uint32_t new_kdf_iterations
)
{
    sdb_superblock_v1 next;
    sdb_status status;
    if (pager == NULL || !pager->open || !pager->encryption_enabled
        || pager->transaction_active || pager->needs_recovery
        || new_password == NULL || new_password_size == 0U
        || new_kdf_iterations < SDB_MIN_KDF_ITERATIONS
        || new_kdf_iterations > SDB_MAX_KDF_ITERATIONS
        || pager->superblock.key_wrap_id == UINT32_MAX) {
        return SDB_E_INVALID_ARGUMENT;
    }
    next = pager->superblock;
    ++next.key_wrap_id;
    next.kdf_iterations = new_kdf_iterations;
    status = sdb_generation_bump(&next.generation);
    if (status == SDB_OK) {
        status = sdb_key_wrap(
            &next, new_password, new_password_size, pager->data_key
        );
    }
    if (status == SDB_OK) {
        status = sdb_pager_advance_superblock(pager, &next);
    }
    return status;
}

sdb_status sdb_pager_write(
    sdb_pager *pager,
    uint64_t page_id,
    uint16_t type,
    uint64_t page_lsn,
    const uint8_t *payload,
    size_t payload_size
)
{
    uint8_t *page;
    uint64_t offset;
    sdb_status status;
    if (pager == NULL || !pager->open || pager->needs_recovery
        || pager->transaction_active
        || page_id == 0U || page_id >= pager->superblock.next_page_id) {
        return SDB_E_INVALID_ARGUMENT;
    }
    status = sdb_pager_offset(pager, page_id, &offset);
    if (status != SDB_OK) {
        return status;
    }
    page = (uint8_t *)malloc((size_t)pager->superblock.page_size);
    if (page == NULL) {
        return SDB_E_INTERNAL;
    }
    if (pager->encryption_enabled) {
        status = sdb_encrypted_page_encode(
            &pager->superblock,
            pager->data_key,
            page,
            (size_t)pager->superblock.page_size,
            type,
            page_id,
            page_lsn,
            payload,
            payload_size
        );
    } else {
        status = sdb_page_encode(
            page,
            (size_t)pager->superblock.page_size,
            type,
            page_id,
            page_lsn,
            payload,
            payload_size
        );
    }
    if (status == SDB_OK) {
        status = sdb_file_write_full(
            &pager->file, offset, page, (size_t)pager->superblock.page_size
        );
    }
    if (status == SDB_OK) {
        status = sdb_file_sync(&pager->file);
    }
    if (status == SDB_OK) {
        /*
         * Cache-content invariant (C.1 in PAGER_INVARIANTS.md): the
         * page cache stores PLAINTEXT-envelope pages, never
         * ciphertext. On plaintext databases `page` is already
         * plaintext, so we cache it as-is. On encrypted databases
         * `page` at this point holds the SEN1 envelope; caching it
         * would force every subsequent read of this page to re-run
         * the XChaCha20-Poly1305 decrypt, making the cache useless
         * for encrypted DBs. Build a scratch plaintext-envelope
         * page from the same (type, page_id, page_lsn, payload)
         * inputs and cache THAT instead — subsequent reads then
         * hit the cache and skip the decrypt entirely.
         */
        if (pager->encryption_enabled) {
            uint8_t *plain = (uint8_t *)malloc(
                (size_t)pager->superblock.page_size
            );
            if (plain != NULL) {
                if (sdb_page_encode(
                        plain,
                        (size_t)pager->superblock.page_size,
                        type,
                        page_id,
                        page_lsn,
                        payload,
                        payload_size
                    ) == SDB_OK) {
                    sdb_page_cache_put(&pager->cache, page_id, plain);
                }
                sdb_secure_zero(
                    plain, (size_t)pager->superblock.page_size
                );
                free(plain);
            }
            /* An OOM here is not fatal — the cache stays cold for
             * this page and the next read repopulates from disk. */
        } else {
            sdb_page_cache_put(&pager->cache, page_id, page);
        }
    }
    free(page);
    return status;
}

sdb_status sdb_pager_read(
    sdb_pager *pager,
    uint64_t page_id,
    uint8_t *page_buffer,
    size_t page_buffer_size,
    sdb_page_view *view_out
)
{
    uint64_t offset;
    sdb_status status;
    if (pager == NULL || !pager->open || pager->needs_recovery
        || page_buffer == NULL || view_out == NULL
        || page_id == 0U || page_id >= pager->superblock.next_page_id) {
        return SDB_E_INVALID_ARGUMENT;
    }
    if (page_buffer_size < (size_t)pager->superblock.page_size) {
        return SDB_E_BUFFER_TOO_SMALL;
    }
    status = sdb_pager_offset(pager, page_id, &offset);
    if (status != SDB_OK) {
        return status;
    }
    /*
     * Track whether the page came from cache. The cache always
     * holds PLAINTEXT-envelope pages (invariant C.1); a cache hit
     * skips the decrypt path entirely. A cache miss reads
     * ciphertext from disk (encrypted DB) or plaintext (plain DB)
     * and, in the encrypted case, decrypts, rebuilds a plaintext
     * envelope in page_buffer, and installs THAT into the cache
     * so the next read hits.
     */
    const bool from_cache = sdb_page_cache_get(
        &pager->cache, page_id, page_buffer
    );
    if (!from_cache) {
        status = sdb_file_read_full(
            &pager->file,
            offset,
            page_buffer,
            (size_t)pager->superblock.page_size
        );
        if (status != SDB_OK) {
            return status;
        }
    }
    status = sdb_page_decode(
        page_buffer, (size_t)pager->superblock.page_size, page_id, view_out
    );
    if (status == SDB_OK && !from_cache && pager->encryption_enabled) {
        uint8_t *plaintext;
        const size_t plaintext_capacity =
            sdb_pager_payload_capacity(pager);
        size_t plaintext_size;
        uint16_t plaintext_type;
        const uint64_t page_lsn = view_out->page_lsn;
        /*
         * On disk in encrypted mode every page (except superblock
         * mirrors) must carry the SEN1 envelope. A plaintext-type
         * page read from disk in encrypted mode is corruption.
         */
        if (view_out->type != (uint16_t)SDB_PAGE_TYPE_ENCRYPTED) {
            return SDB_E_CORRUPT;
        }
        if (plaintext_capacity == 0U) {
            return SDB_E_CORRUPT;
        }
        plaintext = (uint8_t *)malloc(plaintext_capacity);
        if (plaintext == NULL) {
            return SDB_E_INTERNAL;
        }
        status = sdb_encrypted_page_decrypt(
            &pager->superblock,
            pager->data_key,
            view_out,
            plaintext,
            plaintext_capacity,
            &plaintext_type,
            &plaintext_size
        );
        if (status == SDB_OK) {
            /* Rebuild page_buffer as a plaintext envelope so both
             * the caller AND the cache below see the same shape. */
            status = sdb_page_encode(
                page_buffer,
                (size_t)pager->superblock.page_size,
                plaintext_type,
                page_id,
                page_lsn,
                plaintext,
                plaintext_size
            );
        }
        sdb_secure_zero(plaintext, plaintext_capacity);
        free(plaintext);
        if (status == SDB_OK) {
            status = sdb_page_decode(
                page_buffer,
                (size_t)pager->superblock.page_size,
                page_id,
                view_out
            );
        }
    } else if (status == SDB_OK && !from_cache
        && view_out->type == (uint16_t)SDB_PAGE_TYPE_ENCRYPTED) {
        /* A plaintext-mode DB reading a page whose disk copy is
         * an SEN1 envelope is corruption. Cache-hit path is
         * guaranteed-plaintext by construction. */
        status = SDB_E_CORRUPT;
    }
    if (status == SDB_OK && !from_cache) {
        /* Install the FINAL (plaintext-envelope) page_buffer into
         * the cache. On the cache-hit path we deliberately do NOT
         * re-put — the cache already has this exact bytes and put
         * would just refresh the LRU stamp. */
        sdb_page_cache_put(&pager->cache, page_id, page_buffer);
    }
    if (status != SDB_OK) {
        sdb_page_cache_remove(&pager->cache, page_id);
    }
    return status;
}

sdb_status sdb_pager_allocate(sdb_pager *pager, uint64_t *page_id_out)
{
    sdb_superblock_v1 next;
    uint64_t page_id;
    sdb_status status;
    if (pager == NULL || !pager->open || pager->needs_recovery
        || pager->transaction_active
        || page_id_out == NULL) {
        return SDB_E_INVALID_ARGUMENT;
    }
    next = pager->superblock;
    if (next.freelist_page != 0U) {
        uint8_t *page = (uint8_t *)malloc((size_t)next.page_size);
        sdb_page_view view;
        if (page == NULL) {
            return SDB_E_INTERNAL;
        }
        page_id = next.freelist_page;
        status = sdb_pager_read(
            pager, page_id, page, (size_t)next.page_size, &view
        );
        if (status == SDB_OK
            && (view.type != (uint16_t)SDB_PAGE_TYPE_FREE
                || view.payload_size != sizeof(uint64_t))) {
            status = SDB_E_CORRUPT;
        }
        if (status == SDB_OK) {
            next.freelist_page = sdb_read_u64_le(view.payload);
        }
        free(page);
        if (status != SDB_OK) {
            return status;
        }
    } else {
        uint64_t offset;
        uint64_t end;
        page_id = next.next_page_id;
        if (page_id == UINT64_MAX) {
            return SDB_E_OVERFLOW;
        }
        status = sdb_pager_offset(pager, page_id, &offset);
        if (status != SDB_OK
            || offset > UINT64_MAX - (uint64_t)next.page_size) {
            return status == SDB_OK ? SDB_E_OVERFLOW : status;
        }
        end = offset + (uint64_t)next.page_size;
        status = sdb_file_resize(&pager->file, end);
        if (status != SDB_OK) {
            return status;
        }
        status = sdb_file_sync(&pager->file);
        if (status != SDB_OK) {
            return status;
        }
        next.next_page_id = page_id + 1U;
    }
    status = sdb_generation_bump(&next.generation);
    if (status != SDB_OK) {
        return status;
    }
    status = sdb_pager_advance_superblock(pager, &next);
    if (status == SDB_OK) {
        status = sdb_pager_write(
            pager,
            page_id,
            (uint16_t)SDB_PAGE_TYPE_DATA,
            next.generation,
            NULL,
            0U
        );
    }
    if (status == SDB_OK) {
        *page_id_out = page_id;
    }
    return status;
}

sdb_status sdb_pager_free(sdb_pager *pager, uint64_t page_id)
{
    uint8_t payload[sizeof(uint64_t)];
    uint8_t *page;
    sdb_page_view view;
    sdb_superblock_v1 next;
    sdb_status status;
    if (pager == NULL || !pager->open || pager->needs_recovery
        || pager->transaction_active
        || page_id == 0U
        || page_id >= pager->superblock.next_page_id) {
        return SDB_E_INVALID_ARGUMENT;
    }
    page = (uint8_t *)malloc((size_t)pager->superblock.page_size);
    if (page == NULL) {
        return SDB_E_INTERNAL;
    }
    status = sdb_pager_read(
        pager, page_id, page, (size_t)pager->superblock.page_size, &view
    );
    if (status == SDB_OK && view.type == (uint16_t)SDB_PAGE_TYPE_FREE) {
        status = SDB_E_INVALID_ARGUMENT;
    }
    free(page);
    if (status != SDB_OK) {
        return status;
    }
    next = pager->superblock;
    status = sdb_generation_bump(&next.generation);
    if (status != SDB_OK) {
        return status;
    }
    sdb_write_u64_le(payload, next.freelist_page);
    status = sdb_pager_write(
        pager,
        page_id,
        (uint16_t)SDB_PAGE_TYPE_FREE,
        next.generation,
        payload,
        sizeof(payload)
    );
    if (status != SDB_OK) {
        return status;
    }
    next.freelist_page = page_id;
    return sdb_pager_advance_superblock(pager, &next);
}

static void sdb_txn_release(sdb_txn *txn)
{
    size_t index;
    if (txn == NULL) {
        return;
    }
    for (index = 0U; index < txn->page_count; ++index) {
        /*
         * Zero payloads unconditionally on release so unencrypted callers do
         * not leave their last-written plaintext lingering in freed heap
         * (A2.5). Cost is a single memset per staged page — negligible next
         * to WAL I/O.
         */
        if (txn->pages[index].payload_size != 0U) {
            sdb_secure_zero(
                txn->pages[index].payload,
                txn->pages[index].payload_size
            );
        }
        free(txn->pages[index].payload);
    }
    if (txn->pager != NULL) {
        txn->pager->transaction_active = false;
    }
    free(txn->pages);
    free(txn->allocated_pages);
    (void)memset(txn, 0, sizeof(*txn));
}

sdb_status sdb_txn_begin(sdb_pager *pager, sdb_txn *txn_out)
{
    if (pager == NULL || !pager->open || pager->needs_recovery
        || pager->transaction_active
        || txn_out == NULL) {
        return SDB_E_INVALID_ARGUMENT;
    }
    (void)memset(txn_out, 0, sizeof(*txn_out));
    txn_out->pager = pager;
    txn_out->target_superblock = pager->superblock;
    txn_out->active = true;
    pager->transaction_active = true;
    return SDB_OK;
}

sdb_status sdb_txn_put(
    sdb_txn *txn,
    uint64_t page_id,
    uint16_t type,
    const uint8_t *payload,
    size_t payload_size
)
{
    sdb_txn_page *grown;
    uint8_t *copy = NULL;
    size_t next_capacity;
    size_t index;
    if (txn == NULL || !txn->active || txn->pager == NULL
        || page_id == 0U
        || page_id >= txn->target_superblock.next_page_id
        || (type != (uint16_t)SDB_PAGE_TYPE_DATA
            && type != (uint16_t)SDB_PAGE_TYPE_FREE)
        || (type == (uint16_t)SDB_PAGE_TYPE_FREE
            && payload_size != sizeof(uint64_t))
        || payload_size > sdb_pager_payload_capacity(txn->pager)
        || (payload == NULL && payload_size != 0U)) {
        return SDB_E_INVALID_ARGUMENT;
    }
    if (type == (uint16_t)SDB_PAGE_TYPE_FREE
        && (sdb_read_u64_le(payload) >= txn->target_superblock.next_page_id
            || sdb_read_u64_le(payload) == page_id)) {
        return SDB_E_INVALID_ARGUMENT;
    }
    for (index = 0U; index < txn->page_count; ++index) {
        if (txn->pages[index].page_id == page_id) {
            return SDB_E_INVALID_ARGUMENT;
        }
    }
    if (payload_size != 0U) {
        copy = (uint8_t *)malloc(payload_size);
        if (copy == NULL) {
            return SDB_E_INTERNAL;
        }
        (void)memcpy(copy, payload, payload_size);
    }
    if (txn->page_count == txn->page_capacity) {
        if (txn->page_capacity == 0U) {
            next_capacity = 4U;
        } else if (txn->page_capacity > SIZE_MAX / 2U) {
            free(copy);
            return SDB_E_OVERFLOW;
        } else {
            next_capacity = txn->page_capacity * 2U;
        }
        if (next_capacity > SIZE_MAX / sizeof(*grown)) {
            free(copy);
            return SDB_E_OVERFLOW;
        }
        grown = (sdb_txn_page *)realloc(
            txn->pages, next_capacity * sizeof(*grown)
        );
        if (grown == NULL) {
            free(copy);
            return SDB_E_INTERNAL;
        }
        txn->pages = grown;
        txn->page_capacity = next_capacity;
    }
    txn->pages[txn->page_count].page_id = page_id;
    txn->pages[txn->page_count].type = type;
    txn->pages[txn->page_count].payload = copy;
    txn->pages[txn->page_count].payload_size = payload_size;
    ++txn->page_count;
    return SDB_OK;
}

sdb_status sdb_txn_allocate(sdb_txn *txn, uint64_t *page_id_out)
{
    uint64_t page_id;
    uint64_t next_freelist;
    uint64_t next_page_id;
    uint64_t *grown;
    if (txn == NULL || !txn->active || txn->pager == NULL
        || page_id_out == NULL) {
        return SDB_E_INVALID_ARGUMENT;
    }
    next_freelist = txn->target_superblock.freelist_page;
    next_page_id = txn->target_superblock.next_page_id;
    if (next_freelist != 0U) {
        size_t staged_index;
        bool staged = false;
        /*
         * If the freelist head was just freed inside this same txn, the disk
         * still carries its old DATA type — reading from disk fails
         * SDB_E_CORRUPT. Resolve the freelist-head from the staged FREE record
         * instead and unstage it (we are about to re-allocate the slot).
         */
        page_id = next_freelist;
        for (staged_index = 0U; staged_index < txn->page_count;
             ++staged_index) {
            if (txn->pages[staged_index].page_id == page_id) {
                if (txn->pages[staged_index].type
                        != (uint16_t)SDB_PAGE_TYPE_FREE
                    || txn->pages[staged_index].payload_size
                        != sizeof(uint64_t)) {
                    return SDB_E_INTERNAL;
                }
                next_freelist = sdb_read_u64_le(
                    txn->pages[staged_index].payload
                );
                free(txn->pages[staged_index].payload);
                (void)memmove(
                    &txn->pages[staged_index],
                    &txn->pages[staged_index + 1U],
                    (txn->page_count - staged_index - 1U)
                        * sizeof(*txn->pages)
                );
                --txn->page_count;
                staged = true;
                break;
            }
        }
        if (!staged) {
            uint8_t *page = (uint8_t *)malloc(
                (size_t)txn->target_superblock.page_size
            );
            sdb_page_view view;
            sdb_status status;
            if (page == NULL) {
                return SDB_E_INTERNAL;
            }
            status = sdb_pager_read(
                txn->pager,
                page_id,
                page,
                (size_t)txn->target_superblock.page_size,
                &view
            );
            if (status == SDB_OK
                && (view.type != (uint16_t)SDB_PAGE_TYPE_FREE
                    || view.payload_size != sizeof(uint64_t))) {
                status = SDB_E_CORRUPT;
            }
            if (status == SDB_OK) {
                next_freelist = sdb_read_u64_le(view.payload);
            }
            free(page);
            if (status != SDB_OK) {
                return status;
            }
        }
    } else {
        page_id = next_page_id;
        if (page_id == UINT64_MAX) {
            return SDB_E_OVERFLOW;
        }
        next_page_id = page_id + 1U;
    }
    if (txn->allocated_page_count == txn->allocated_page_capacity) {
        size_t next_capacity = txn->allocated_page_capacity == 0U
            ? 8U : txn->allocated_page_capacity * 2U;
        if (next_capacity < txn->allocated_page_capacity
            || next_capacity > SIZE_MAX / sizeof(*grown)) {
            return SDB_E_OVERFLOW;
        }
        grown = (uint64_t *)realloc(
            txn->allocated_pages, next_capacity * sizeof(*grown)
        );
        if (grown == NULL) {
            return SDB_E_INTERNAL;
        }
        txn->allocated_pages = grown;
        txn->allocated_page_capacity = next_capacity;
    }
    txn->allocated_pages[txn->allocated_page_count] = page_id;
    ++txn->allocated_page_count;
    txn->target_superblock.freelist_page = next_freelist;
    txn->target_superblock.next_page_id = next_page_id;
    *page_id_out = page_id;
    return SDB_OK;
}

sdb_status sdb_txn_free(sdb_txn *txn, uint64_t page_id)
{
    uint8_t payload[sizeof(uint64_t)];
    size_t index;
    sdb_status status;
    if (txn == NULL || !txn->active || txn->pager == NULL
        || page_id == 0U
        || page_id >= txn->target_superblock.next_page_id) {
        return SDB_E_INVALID_ARGUMENT;
    }
    for (index = 0U; index < txn->allocated_page_count; ++index) {
        if (txn->allocated_pages[index] == page_id) {
            /*
             * Cancel an in-batch allocation instead of routing it through the
             * on-disk freelist — the page never reached durable storage, so
             * FREE-record bookkeeping is pointless and would violate T.4
             * (allocated_pages must appear in pages). The slot becomes an
             * orphan in the file (compact reclaims it). Removes the batch's
             * dependency on knowing whether allocate grew next_page_id or
             * popped from freelist, at the cost of one wasted page slot.
             */
            size_t staged;
            (void)memmove(
                &txn->allocated_pages[index],
                &txn->allocated_pages[index + 1U],
                (txn->allocated_page_count - index - 1U)
                    * sizeof(*txn->allocated_pages)
            );
            --txn->allocated_page_count;
            for (staged = 0U; staged < txn->page_count; ++staged) {
                if (txn->pages[staged].page_id == page_id) {
                    free(txn->pages[staged].payload);
                    (void)memmove(
                        &txn->pages[staged],
                        &txn->pages[staged + 1U],
                        (txn->page_count - staged - 1U)
                            * sizeof(*txn->pages)
                    );
                    --txn->page_count;
                    break;
                }
            }
            return SDB_OK;
        }
    }
    /*
     * Mirror the double-free guard in sdb_pager_free (pager.c:670-675):
     * refuse to add a page to the freelist if the current on-disk type
     * is already FREE. Without this, a caller-side bug that frees the
     * same page twice bakes a durable freelist cycle
     * (freelist_page → A → B → A → ...) into the on-disk graph, which
     * subsequent allocate calls hit as a spinning read loop. Public
     * APIs cannot reach this state on a healthy DB, so this is
     * defense-in-depth — but the diagnostic beats a silent cycle.
     */
    {
        uint8_t *page = (uint8_t *)malloc(
            (size_t)txn->pager->superblock.page_size
        );
        sdb_page_view view;
        if (page == NULL) {
            return SDB_E_INTERNAL;
        }
        status = sdb_pager_read(
            txn->pager, page_id, page,
            (size_t)txn->pager->superblock.page_size, &view
        );
        if (status == SDB_OK && view.type == (uint16_t)SDB_PAGE_TYPE_FREE) {
            status = SDB_E_INVALID_ARGUMENT;
        }
        free(page);
        if (status != SDB_OK) {
            return status;
        }
    }
    sdb_write_u64_le(payload, txn->target_superblock.freelist_page);
    status = sdb_txn_put(
        txn,
        page_id,
        (uint16_t)SDB_PAGE_TYPE_FREE,
        payload,
        sizeof(payload)
    );
    if (status == SDB_OK) {
        txn->target_superblock.freelist_page = page_id;
    }
    return status;
}

sdb_status sdb_txn_commit(sdb_txn *txn)
{
    sdb_pager *pager;
    sdb_wal_page *wal_pages;
    uint8_t *storage;
    uint64_t txn_id;
    size_t index;
    sdb_status status = SDB_OK;
    if (txn == NULL || !txn->active || txn->pager == NULL) {
        return SDB_E_INVALID_ARGUMENT;
    }
    if (txn->page_count == 0U) {
        if (txn->allocated_page_count != 0U) {
            sdb_txn_release(txn);
        }
        return SDB_E_INVALID_ARGUMENT;
    }
    pager = txn->pager;
    if (pager->superblock.checkpoint_lsn == UINT64_MAX
        || txn->page_count > SIZE_MAX / sizeof(*wal_pages)
        || txn->page_count
            > SIZE_MAX / (size_t)pager->superblock.page_size) {
        sdb_txn_release(txn);
        return SDB_E_OVERFLOW;
    }
    for (index = 0U; index < txn->allocated_page_count; ++index) {
        size_t page_index;
        bool staged = false;
        for (page_index = 0U; page_index < txn->page_count; ++page_index) {
            if (txn->pages[page_index].page_id
                == txn->allocated_pages[index]) {
                staged = true;
                break;
            }
        }
        if (!staged) {
            sdb_txn_release(txn);
            return SDB_E_INVALID_ARGUMENT;
        }
    }
    txn_id = pager->superblock.checkpoint_lsn + 1U;
    wal_pages = (sdb_wal_page *)malloc(
        txn->page_count * sizeof(*wal_pages)
    );
    storage = (uint8_t *)malloc(
        txn->page_count * (size_t)pager->superblock.page_size
    );
    if (wal_pages == NULL || storage == NULL) {
        free(wal_pages);
        free(storage);
        sdb_txn_release(txn);
        return SDB_E_INTERNAL;
    }
    for (index = 0U; index < txn->page_count; ++index) {
        wal_pages[index].page_id = txn->pages[index].page_id;
        wal_pages[index].bytes =
            storage + (index * (size_t)pager->superblock.page_size);
        if (pager->encryption_enabled) {
            status = sdb_encrypted_page_encode(
                &txn->target_superblock,
                pager->data_key,
                (uint8_t *)wal_pages[index].bytes,
                (size_t)pager->superblock.page_size,
                txn->pages[index].type,
                txn->pages[index].page_id,
                txn_id,
                txn->pages[index].payload,
                txn->pages[index].payload_size
            );
        } else {
            status = sdb_page_encode(
                (uint8_t *)wal_pages[index].bytes,
                (size_t)pager->superblock.page_size,
                txn->pages[index].type,
                txn->pages[index].page_id,
                txn_id,
                txn->pages[index].payload,
                txn->pages[index].payload_size
            );
        }
        if (status != SDB_OK) {
            break;
        }
    }
    if (status == SDB_OK) {
        status = sdb_wal_write_committed(
            pager->wal_path,
            &txn->target_superblock,
            txn_id,
            wal_pages,
            txn->page_count
        );
    }
    if (status == SDB_OK) {
        for (index = 0U; index < txn->page_count; ++index) {
            uint64_t offset;
            status = sdb_pager_offset(
                pager, wal_pages[index].page_id, &offset
            );
            if (status == SDB_OK) {
                status = sdb_file_write_full(
                    &pager->file,
                    offset,
                    wal_pages[index].bytes,
                    (size_t)pager->superblock.page_size
                );
            }
            if (status != SDB_OK) {
                break;
            }
        }
    }
    if (status == SDB_OK) {
        status = sdb_file_sync(&pager->file);
    }
    if (status == SDB_OK) {
        sdb_superblock_v1 next = txn->target_superblock;
        next.checkpoint_lsn = txn_id;
        next.generation = pager->superblock.generation;
        status = sdb_generation_bump(&next.generation);
        if (status == SDB_OK) {
            status = sdb_pager_advance_superblock(pager, &next);
        }
    }
    if (status == SDB_OK) {
        /*
         * Cache invariant (C.1): store plaintext-envelope pages,
         * never ciphertext. wal_pages[i].bytes at this point is the
         * SEN1 envelope on encrypted DBs; caching it would force
         * every subsequent read of this page to re-run the
         * XChaCha20-Poly1305 decrypt. Rebuild the plaintext
         * envelope from the (type, page_id, page_lsn, payload) that
         * are still available on the txn->pages[] side and cache
         * THAT. On plaintext DBs, wal_pages[i].bytes IS the
         * plaintext envelope, so the extra rebuild is skipped.
         */
        uint8_t *plain_scratch = NULL;
        if (pager->encryption_enabled) {
            plain_scratch = (uint8_t *)malloc(
                (size_t)pager->superblock.page_size
            );
            /* An OOM here is not fatal — the cache stays cold and
             * subsequent reads repopulate from disk. */
        }
        for (index = 0U; index < txn->page_count; ++index) {
            if (pager->encryption_enabled) {
                if (plain_scratch != NULL
                    && sdb_page_encode(
                        plain_scratch,
                        (size_t)pager->superblock.page_size,
                        txn->pages[index].type,
                        txn->pages[index].page_id,
                        txn_id,
                        txn->pages[index].payload,
                        txn->pages[index].payload_size
                    ) == SDB_OK) {
                    sdb_page_cache_put(
                        &pager->cache,
                        wal_pages[index].page_id,
                        plain_scratch
                    );
                }
            } else {
                sdb_page_cache_put(
                    &pager->cache,
                    wal_pages[index].page_id,
                    wal_pages[index].bytes
                );
            }
        }
        if (plain_scratch != NULL) {
            sdb_secure_zero(
                plain_scratch, (size_t)pager->superblock.page_size
            );
            free(plain_scratch);
        }
        status = sdb_wal_clear(pager->wal_path);
    }
    if (status != SDB_OK) {
        pager->needs_recovery = true;
    }
    free(storage);
    free(wal_pages);
    sdb_txn_release(txn);
    return status;
}

void sdb_txn_abort(sdb_txn *txn)
{
    sdb_txn_release(txn);
}
