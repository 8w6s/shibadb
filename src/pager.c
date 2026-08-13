#include "pager.h"

#include "encrypted_page.h"
#include "internal.h"
#include "key_manager.h"
#include "replace.h"
#include "superblock_store.h"

#include <stdlib.h>
#include <string.h>

#if SDB_TESTING
static sdb_checkpoint_fail_stage sdb_checkpoint_fail_stage_for_testing =
    SDB_CHECKPOINT_FAIL_NONE;
#endif

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
        return SDB_E_OUT_OF_MEMORY;
    }
    (void)memcpy(result, path, path_size);
    (void)memcpy(result + path_size, suffix, sizeof(suffix));
    *wal_path_out = result;
    return SDB_OK;
}

/*
 * Ensure the persistent WAL fd (R2a) is open, creating the .wal file if it
 * does not exist yet. Called lazily by the commit path so a read-only or
 * never-committed pager never materialises a WAL sidecar. On first creation
 * we fsync the parent directory once so the new directory entry is durable
 * (matching the former open-per-commit behaviour). A half-created fd is
 * closed on error so a later commit never writes/fsyncs through a broken
 * handle. Idempotent when already open.
 */
static sdb_status sdb_pager_ensure_wal_open(sdb_pager *pager)
{
    sdb_status status;
    if (pager == NULL || pager->wal_path == NULL) {
        return SDB_E_INVALID_ARGUMENT;
    }
    if (pager->wal_file_open) {
        return SDB_OK;
    }
    status = sdb_file_open_existing(pager->wal_path, true, &pager->wal_file);
    if (status == SDB_E_NOT_FOUND) {
        status = sdb_file_create_new(pager->wal_path, &pager->wal_file);
        if (status == SDB_OK) {
            status = sdb_file_sync_parent_directory(pager->wal_path);
            if (status != SDB_OK) {
                (void)sdb_file_close(&pager->wal_file);
                return status;
            }
        }
    }
    if (status == SDB_OK) {
        pager->wal_file_open = true;
    }
    return status;
}

/*
 * Close the persistent WAL fd if open. Safe to call when it was never
 * opened. Used by sdb_pager_close and by the compact swap, which must drop
 * the fd pointing at the about-to-be-unlinked temporary WAL before the
 * swapped-in pager adopts the source WAL path.
 */
static sdb_status sdb_pager_close_wal(sdb_pager *pager)
{
    sdb_status status = SDB_OK;
    if (pager != NULL && pager->wal_file_open) {
        status = sdb_file_close(&pager->wal_file);
        pager->wal_file_open = false;
    }
    return status;
}

/*
 * Truncate the WAL back to empty at a checkpoint. When the persistent fd is
 * open we resize + fsync THROUGH that live fd (and fsync the parent dir),
 * so the reset lands on the very inode the next commit will append to — no
 * second descriptor, no risk of a stale fd. When no fd is open yet (e.g. a
 * checkpoint driven before any commit opened one) we fall back to the
 * path-based clear. The caller resets wal_tail / wal_index separately.
 */
static sdb_status sdb_pager_wal_reset(sdb_pager *pager)
{
    sdb_status status;
    if (pager == NULL || pager->wal_path == NULL) {
        return SDB_E_INVALID_ARGUMENT;
    }
    if (!pager->wal_file_open) {
        return sdb_wal_clear(pager->wal_path);
    }
    status = sdb_file_resize(&pager->wal_file, 0U);
    if (status == SDB_OK) {
        status = sdb_file_sync(&pager->wal_file);
    }
    if (status == SDB_OK) {
        status = sdb_file_sync_parent_directory(pager->wal_path);
    }
    return status;
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

sdb_status sdb_pager_store_superblock(
    sdb_pager *pager, const sdb_superblock_v1 *next
)
{
    sdb_status status;
    if (pager == NULL || next == NULL) {
        return SDB_E_INVALID_ARGUMENT;
    }
    status = (next->flags & SDB_FLAG_HEADER_AUTH) != 0U
        ? sdb_superblock_store_update_file_authenticated(
            &pager->file, next, pager->data_key
        )
        : sdb_superblock_store_update_file(&pager->file, next);
    if (status == SDB_OK) {
        pager->superblock = *next;
    }
    return status;
}

static sdb_status sdb_generation_bump(uint64_t *generation)
{
    if (generation == NULL || *generation == UINT64_MAX) {
        return SDB_E_OVERFLOW;
    }
    ++(*generation);
    return SDB_OK;
}

static sdb_status sdb_pager_canonicalize_file_size(
    sdb_pager *pager, bool geometry_unauthenticated
)
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
    if (geometry_unauthenticated) {
        /*
         * Legacy first open (ENCRYPTED, HEADER_AUTH clear): next_page_id is
         * only CRC32-protected, so an offline attacker can lower it below the
         * real page count. The trailing region [expected_size, actual_size) is
         * therefore ambiguous:
         *   - a lowered next_page_id leaves the victim's real pages (encrypted,
         *     non-zero) there, and truncating would destroy them, whereas
         *   - an abandoned trailing allocation (invariant R.3: a crash after
         *     ftruncate but before the superblock advance) leaves that region
         *     zero-filled, since no page image was ever written.
         * Reclaim the region only when it is entirely zero — the single shape a
         * legitimate orphan can take before any page write. Any non-zero byte
         * means real data may live there, so refuse rather than trust the
         * unauthenticated geometry. This runs before the deferred seal, so a
         * refusal never writes HEADER_AUTH over forged geometry. (An attacker
         * who first zeroes the tail and then lowers next_page_id has already
         * destroyed that plaintext by direct tamper, which is out of scope per
         * docs/AUDIT.md; this never makes matters worse.)
         */
        uint64_t offset = expected_size;
        uint8_t *scratch =
            (uint8_t *)malloc((size_t)pager->superblock.page_size);
        if (scratch == NULL) {
            return SDB_E_OUT_OF_MEMORY;
        }
        while (offset < actual_size) {
            const uint64_t remaining = actual_size - offset;
            const size_t chunk =
                remaining < (uint64_t)pager->superblock.page_size
                    ? (size_t)remaining
                    : (size_t)pager->superblock.page_size;
            size_t i;
            status = sdb_file_read_full(&pager->file, offset, scratch, chunk);
            if (status != SDB_OK) {
                free(scratch);
                return status;
            }
            for (i = 0U; i < chunk; ++i) {
                if (scratch[i] != 0U) {
                    free(scratch);
                    return SDB_E_CORRUPT;
                }
            }
            offset += (uint64_t)chunk;
        }
        free(scratch);
        /*
         * Trailing region is entirely zero: a safe-to-reclaim orphan. Fall
         * through to the truncate below, exactly as the authenticated path.
         */
    }
    status = sdb_file_resize(&pager->file, expected_size);
    if (status == SDB_OK) {
        status = sdb_file_sync(&pager->file);
    }
    return status;
}

size_t sdb_pager_cache_capacity(size_t cache_bytes, size_t page_size)
{
    size_t pages;
    if (page_size == 0U || cache_bytes == 0U) {
        return (size_t)SDB_PAGER_CACHE_CAPACITY;
    }
    pages = cache_bytes / page_size;
    if (pages < SDB_PAGER_CACHE_CAPACITY_MIN) {
        pages = SDB_PAGER_CACHE_CAPACITY_MIN;
    } else if (pages > SDB_PAGER_CACHE_CAPACITY_MAX) {
        pages = SDB_PAGER_CACHE_CAPACITY_MAX;
    }
    return pages;
}

static sdb_status sdb_pager_open_internal(
    const char *path,
    const uint8_t *password,
    size_t password_size,
    size_t cache_bytes,
    sdb_pager *pager_out
)
{
    sdb_superblock_read_result result;
    sdb_status status;
    bool legacy_upgrade_pending = false;
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
        if ((pager_out->superblock.flags & SDB_FLAG_HEADER_AUTH) != 0U) {
            status = sdb_superblock_store_read_authenticated_file(
                &pager_out->file, pager_out->data_key, &result
            );
            if (status == SDB_OK) {
                pager_out->superblock = result.superblock;
            }
        } else {
            /*
             * Legacy header (ENCRYPTED, HEADER_AUTH clear): the geometry
             * (next_page_id/root_page/freelist_page/…) is only CRC32-protected
             * — the wrapped-key AAD does not cover it — so it is NOT yet
             * trustworthy. Defer the HEADER_AUTH upgrade seal until AFTER WAL
             * recovery and canonicalize have run and validated the geometry
             * against the real file size. Sealing here (as the code used to)
             * would authenticate a tampered next_page_id and let canonicalize
             * shrink the file, destroying trailing pages.
             */
            legacy_upgrade_pending = true;
        }
        if (status != SDB_OK) {
            sdb_secure_zero(pager_out->data_key, sizeof(pager_out->data_key));
            (void)sdb_file_close(&pager_out->file);
            return status;
        }
    } else if (password != NULL || password_size != 0U) {
        (void)sdb_file_close(&pager_out->file);
        return SDB_E_INVALID_ARGUMENT;
    }
    status = sdb_replace_recover(path, pager_out->superblock.file_id);
    if (status != SDB_OK) {
        sdb_secure_zero(pager_out->data_key, sizeof(pager_out->data_key));
        (void)sdb_file_close(&pager_out->file);
        return status;
    }
    /*
     * Authentication (when enabled) has established that the selected mirror
     * belongs to this database. Restore a damaged peer before exposing a
     * usable handle, so a read-only workload cannot remain one crash away
     * from losing both superblock copies.
     */
    status = sdb_superblock_store_heal_file(
        &pager_out->file,
        &result,
        (pager_out->superblock.flags & SDB_FLAG_HEADER_AUTH) != 0U
            ? pager_out->data_key : NULL
    );
    if (status != SDB_OK) {
        sdb_secure_zero(pager_out->data_key, sizeof(pager_out->data_key));
        (void)sdb_file_close(&pager_out->file);
        return status;
    }
    status = sdb_pager_make_wal_path(path, &pager_out->wal_path);
    if (status != SDB_OK) {
        sdb_secure_zero(pager_out->data_key, sizeof(pager_out->data_key));
        (void)sdb_file_close(&pager_out->file);
        return status;
    }
    {
        uint64_t recovered_lsn = pager_out->superblock.checkpoint_lsn;
        uint64_t recovered_next_page_id = pager_out->superblock.next_page_id;
        uint64_t recovered_freelist_page =
            pager_out->superblock.freelist_page;
        bool replayed = false;
        /*
         * v3 multi-txn recovery (Task 4). If it returns non-OK we ABORT the
         * open without advancing checkpoint_lsn — recovery may have applied
         * some txns idempotently, and the next open will re-scan the WAL
         * from the current checkpoint_lsn and continue. That's only safe if
         * we don't move the durable checkpoint forward on error.
         */
        status = sdb_wal_recover_all(
            pager_out->wal_path,
            &pager_out->file,
            &pager_out->superblock,
            pager_out->encryption_enabled ? pager_out->data_key : NULL,
            &recovered_lsn,
            &recovered_next_page_id,
            &recovered_freelist_page,
            &replayed
        );
        /*
         * v1/v2 legacy fallback: if v3 recovery found nothing to replay,
         * try the single-record path. This keeps pre-WAL-mode WAL files
         * (written by sdb_wal_write_committed) recoverable during the
         * migration window. When the WAL is empty or the recover_all path
         * already applied v3 txns, this is a cheap no-op.
         */
        if (status == SDB_OK && !replayed) {
            uint64_t legacy_lsn = pager_out->superblock.checkpoint_lsn;
            uint64_t legacy_next_page_id =
                pager_out->superblock.next_page_id;
            uint64_t legacy_freelist_page =
                pager_out->superblock.freelist_page;
            bool legacy_replayed = false;
            status = sdb_wal_recover(
                pager_out->wal_path,
                &pager_out->file,
                &pager_out->superblock,
                pager_out->encryption_enabled
                    ? pager_out->data_key : NULL,
                &legacy_lsn,
                &legacy_next_page_id,
                &legacy_freelist_page,
                &legacy_replayed
            );
            if (status == SDB_OK && legacy_replayed) {
                recovered_lsn = legacy_lsn;
                recovered_next_page_id = legacy_next_page_id;
                recovered_freelist_page = legacy_freelist_page;
                replayed = true;
            }
        }
        if (status == SDB_OK
            && recovered_lsn > pager_out->superblock.checkpoint_lsn) {
            sdb_superblock_v1 next = pager_out->superblock;
            next.checkpoint_lsn = recovered_lsn;
            next.next_page_id = recovered_next_page_id;
            next.freelist_page = recovered_freelist_page;
            status = sdb_generation_bump(&next.generation);
            if (status == SDB_OK) {
                status = sdb_pager_store_superblock(pager_out, &next);
            }
        }
        if (status == SDB_OK) {
            status = sdb_pager_canonicalize_file_size(
                pager_out, legacy_upgrade_pending
            );
        }
        if (status == SDB_OK && legacy_upgrade_pending) {
            /*
             * Deferred legacy upgrade seal. The geometry has now survived WAL
             * recovery + canonicalize (which refuses to shrink an
             * unauthenticated header), so it is safe to bind it under
             * HEADER_AUTH. key_wrap_id / generation were left intact above, so
             * the monotonicity guard in update_file_authenticated still holds.
             */
            sdb_superblock_v1 upgraded = pager_out->superblock;
            if (upgraded.key_wrap_id == UINT32_MAX
                || upgraded.generation == UINT64_MAX) {
                status = SDB_E_OVERFLOW;
            } else {
                upgraded.flags |= SDB_FLAG_HEADER_AUTH;
                ++upgraded.key_wrap_id;
                ++upgraded.generation;
                status = sdb_key_wrap(
                    &upgraded, password, password_size, pager_out->data_key
                );
                if (status == SDB_OK) {
                    status = sdb_superblock_store_update_file_authenticated(
                        &pager_out->file, &upgraded, pager_out->data_key
                    );
                }
                if (status == SDB_OK) {
                    pager_out->superblock = upgraded;
                }
            }
        }
        if (status == SDB_OK) {
            /*
             * WAL has been applied to the data file; safe to reset.
             * On the next commit sdb_wal_append_txn will start at
             * SDB_WAL_HEADER_SIZE and grow the WAL from empty.
             */
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
        sdb_pager_cache_capacity(
            cache_bytes, (size_t)pager_out->superblock.page_size
        ),
        (size_t)pager_out->superblock.page_size
    );
    if (status != SDB_OK) {
        sdb_secure_zero(pager_out->data_key, sizeof(pager_out->data_key));
        free(pager_out->wal_path);
        pager_out->wal_path = NULL;
        (void)sdb_file_close(&pager_out->file);
        return status;
    }
    status = sdb_wal_index_init(&pager_out->wal_index);
    if (status != SDB_OK) {
        sdb_page_cache_destroy(&pager_out->cache);
        sdb_secure_zero(pager_out->data_key, sizeof(pager_out->data_key));
        free(pager_out->wal_path);
        pager_out->wal_path = NULL;
        (void)sdb_file_close(&pager_out->file);
        return status;
    }
    /*
     * WAL just got cleared above, so the next append starts at
     * SDB_WAL_HEADER_SIZE (bytes [0, 64) are a sparse zero-hole created
     * on first append — sdb_wal_recover_all ignores them). current_lsn
     * mirrors the durable checkpoint_lsn until the next commit bumps it.
     */
    pager_out->wal_tail = (uint64_t)SDB_WAL_HEADER_SIZE;
    pager_out->current_lsn = pager_out->superblock.checkpoint_lsn;
    /*
     * Default threshold: ~1000 pages of raw WAL bytes. Task 5 will wire
     * the trigger; this dispatch only tracks the value.
     */
    pager_out->checkpoint_threshold =
        (uint64_t)SDB_PAGER_CHECKPOINT_THRESHOLD_PAGES
        * (uint64_t)pager_out->superblock.page_size;
    /*
     * R4 group-commit coordinator. durable_lsn starts at the checkpoint LSN:
     * everything up to and including the last checkpoint is already on disk,
     * so a follower whose txn_id is <= this never needs to wait.
     */
    pager_out->durable_lsn = pager_out->current_lsn;
    pager_out->written_lsn = pager_out->current_lsn;
    status = sdb_mutex_init(&pager_out->commit_mutex);
    if (status == SDB_OK) {
        status = sdb_cond_init(&pager_out->commit_cond);
        if (status != SDB_OK) {
            sdb_mutex_destroy(&pager_out->commit_mutex);
        }
    }
    if (status != SDB_OK) {
        sdb_wal_index_free(&pager_out->wal_index);
        sdb_page_cache_destroy(&pager_out->cache);
        sdb_secure_zero(pager_out->data_key, sizeof(pager_out->data_key));
        free(pager_out->wal_path);
        pager_out->wal_path = NULL;
        (void)sdb_file_close(&pager_out->file);
        return status;
    }
    pager_out->commit_coord_ready = true;
    pager_out->open = true;
    return SDB_OK;
}

sdb_status sdb_pager_open(const char *path, sdb_pager *pager_out)
{
    return sdb_pager_open_internal(path, NULL, 0U, 0U, pager_out);
}

sdb_status sdb_pager_open_ex(
    const char *path, size_t cache_bytes, sdb_pager *pager_out
)
{
    return sdb_pager_open_internal(path, NULL, 0U, cache_bytes, pager_out);
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
        path, password, password_size, 0U, pager_out
    );
}

sdb_status sdb_pager_open_encrypted_ex(
    const char *path,
    const uint8_t *password,
    size_t password_size,
    size_t cache_bytes,
    sdb_pager *pager_out
)
{
    if (password == NULL || password_size == 0U) {
        return SDB_E_INVALID_ARGUMENT;
    }
    return sdb_pager_open_internal(
        path, password, password_size, cache_bytes, pager_out
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
    return sdb_pager_create_ex(path, page_size, salt, file_id, 0U, pager_out);
}

sdb_status sdb_pager_create_ex(
    const char *path,
    uint32_t page_size,
    const uint8_t salt[SDB_SALT_SIZE],
    const uint8_t file_id[SDB_FILE_ID_SIZE],
    size_t cache_bytes,
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
    return sdb_pager_open_ex(path, cache_bytes, pager_out);
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
    return sdb_pager_create_encrypted_ex(
        path, page_size, salt, file_id, password, password_size,
        kdf_iterations, 0U, pager_out
    );
}

sdb_status sdb_pager_create_encrypted_ex(
    const char *path,
    uint32_t page_size,
    const uint8_t salt[SDB_SALT_SIZE],
    const uint8_t file_id[SDB_FILE_ID_SIZE],
    const uint8_t *password,
    size_t password_size,
    uint32_t kdf_iterations,
    size_t cache_bytes,
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
    superblock.flags = SDB_FLAG_ENCRYPTED | SDB_FLAG_HEADER_AUTH;
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
        status = sdb_superblock_store_create_authenticated(
            path, &superblock, data_key
        );
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
    return sdb_pager_open_encrypted_ex(
        path, password, password_size, cache_bytes, pager_out
    );
}

/*
 * Inline checkpoint. Applies every WAL-resident page to the data file,
 * persists an updated superblock (checkpoint_lsn = current_lsn), and
 * clears the WAL. After this call the data file is authoritative for
 * every committed page; wal_index is empty; wal_tail is reset to the
 * fresh-append offset. Called by paths that need a self-contained
 * data-file snapshot (backup, migrate/compact) — Task 5 will wire the
 * automatic threshold trigger; this helper is the primitive both dispatches
 * lean on.
 *
 * No-op if wal_index is empty (nothing to flush).
 */
sdb_status sdb_pager_checkpoint(sdb_pager *pager)
{
    const uint64_t data_offset =
        (uint64_t)(SDB_SUPERBLOCK_SLOT_SIZE * SDB_SUPERBLOCK_SLOT_COUNT);
    sdb_file wal;
    sdb_file *wal_ptr;
    uint8_t *frame_page;
    size_t page_size;
    size_t slot;
    bool wal_opened = false;
    bool anything_applied = false;
    bool superblock_advanced = false;
    sdb_status status = SDB_OK;
    sdb_status close_status;
    if (pager == NULL || !pager->open || pager->needs_recovery
        || pager->transaction_active || pager->wal_path == NULL) {
        return SDB_E_INVALID_ARGUMENT;
    }
    if (pager->wal_index.size == 0U) {
        return SDB_OK;
    }
    page_size = (size_t)pager->superblock.page_size;
    frame_page = (uint8_t *)malloc(page_size);
    if (frame_page == NULL) {
        return SDB_E_OUT_OF_MEMORY;
    }
    /*
     * R2a Windows-safety: read frames through the ALREADY-OPEN persistent WAL
     * fd, not a second handle. wal_index.size>0 here means a txn was committed,
     * so pager->wal_file is open (GENERIC_READ|WRITE -> readable). Opening a
     * second read handle to the same .wal while wal_file holds it for write
     * trips ERROR_SHARING_VIOLATION on Windows (the WAL open share mask lacks
     * FILE_SHARE_WRITE), which would permanently fail checkpoint. Fall back to
     * a private read handle only in the unexpected case the persistent fd is
     * not open.
     */
    if (pager->wal_file_open) {
        wal_ptr = &pager->wal_file;
    } else {
        status = sdb_file_open_existing(pager->wal_path, false, &wal);
        if (status != SDB_OK) {
            free(frame_page);
            return status;
        }
        wal_opened = true;
        wal_ptr = &wal;
    }
    /*
     * Write-ahead invariant: the WAL frames about to be copied into the data
     * file MUST be durable before the data file (then the superblock) starts
     * reflecting them. In SDB_SYNCHRONOUS_FULL the WAL was already fsynced at
     * each commit, so this is a cheap near-no-op. In SDB_SYNCHRONOUS_NORMAL,
     * where per-commit fsync is skipped, this is the barrier that makes
     * checkpointed commits durable and stops the data file from getting ahead
     * of a recoverable WAL — without it a power loss between the data-file fsync
     * and the superblock update could leave a page holding a future (yet
     * CRC/tag-valid) version that the recovered btree never references, i.e.
     * silent structural corruption rather than the promised clean tail loss.
     */
    if (pager->wal_file_open) {
        status = sdb_file_sync(&pager->wal_file);
    }
    for (slot = 0U; status == SDB_OK && slot < pager->wal_index.capacity;
         ++slot) {
        const uint64_t page_id = pager->wal_index.slots[slot].page_id;
        const uint64_t wal_offset = pager->wal_index.slots[slot].wal_offset;
        uint64_t page_offset;
        sdb_page_view view;
        if (page_id == 0U) {
            continue;
        }
        if (page_id >= pager->superblock.next_page_id
            || wal_offset > pager->wal_tail
            || (uint64_t)SDB_WAL_RECORD_HEADER_SIZE_V5
                > pager->wal_tail - wal_offset
            || (uint64_t)page_size
                > pager->wal_tail - wal_offset
                    - (uint64_t)SDB_WAL_RECORD_HEADER_SIZE_V5) {
            status = SDB_E_CORRUPT;
            break;
        }
        status = sdb_file_read_full(
            wal_ptr,
            wal_offset + (uint64_t)SDB_WAL_RECORD_HEADER_SIZE_V5,
            frame_page,
            page_size
        );
        if (status != SDB_OK) {
            break;
        }
        /* Sanity-check: the frame's page_id must match the wal_index key. */
        if (sdb_page_decode(
                frame_page, page_size, page_id, &view
            ) != SDB_OK) {
            status = SDB_E_CORRUPT;
            break;
        }
        page_offset = data_offset
            + (page_id - 1U) * (uint64_t)page_size;
        status = sdb_file_write_full(
            &pager->file, page_offset, frame_page, page_size
        );
        if (status != SDB_OK) {
            break;
        }
        anything_applied = true;
    }
    if (wal_opened) {
        close_status = sdb_file_close(&wal);
        if (status == SDB_OK) {
            status = close_status;
        }
    }
    sdb_secure_zero(frame_page, page_size);
    free(frame_page);
#if SDB_TESTING
    if (status == SDB_OK && anything_applied
        && sdb_checkpoint_fail_stage_for_testing
            == SDB_CHECKPOINT_FAIL_BEFORE_DATA_SYNC) {
        status = SDB_E_IO;
    }
#endif
    if (status == SDB_OK && anything_applied) {
        status = sdb_file_sync(&pager->file);
    }
#if SDB_TESTING
    if (status == SDB_OK && anything_applied
        && sdb_checkpoint_fail_stage_for_testing
            == SDB_CHECKPOINT_FAIL_BEFORE_SUPERBLOCK) {
        status = SDB_E_IO;
    }
#endif
    if (status == SDB_OK && anything_applied) {
        /*
         * Persist the checkpoint: on-disk superblock now advertises
         * checkpoint_lsn = current_lsn, so open recovery will scan the
         * WAL for txn_ids > current_lsn only. We bump generation so
         * the two-slot rotation writes the fresh slot.
         */
        sdb_superblock_v1 next = pager->superblock;
        next.checkpoint_lsn = pager->current_lsn;
        status = sdb_generation_bump(&next.generation);
        if (status == SDB_OK) {
            status = sdb_pager_store_superblock(pager, &next);
        }
        if (status == SDB_OK) {
            superblock_advanced = true;
        }
    }
#if SDB_TESTING
    if (status == SDB_OK && anything_applied
        && sdb_checkpoint_fail_stage_for_testing
            == SDB_CHECKPOINT_FAIL_BEFORE_WAL_CLEAR) {
        status = SDB_E_IO;
    }
#endif
    if (superblock_advanced) {
        /*
         * The superblock now durably advertises checkpoint_lsn =
         * current_lsn, so every WAL frame belongs to a txn_id <=
         * checkpoint_lsn and is already fsynced into the data file. Reset
         * the in-memory WAL position to the checkpointed state (empty
         * index, tail back at the header) BEFORE — and regardless of the
         * outcome of — sdb_wal_clear. This keeps in-memory state self-
         * consistent with the durable superblock even if wal_clear fails
         * (real EIO/ENOSPC): a leftover WAL file is then harmless because
         * recover_all skips txn_ids <= checkpoint_lsn and the next commit
         * appends from SDB_WAL_HEADER_SIZE, overwriting the stale frames.
         *
         * Without this reset a post-superblock wal_clear failure would
         * leave wal_tail pointing past already-checkpointed frames, so the
         * next commit would append at that stale tail (txn_id
         * current_lsn+1) and be silently lost on crash+reopen: the durable
         * superblock says checkpoint_lsn = N, recover_all expects txn N+1
         * at the WAL head but finds the old checkpointed frames there and
         * stops at the torn tail before ever reaching the new txn.
         */
        sdb_wal_index_clear(&pager->wal_index);
        pager->wal_tail = (uint64_t)SDB_WAL_HEADER_SIZE;
        if (status == SDB_OK) {
            /*
             * Reset through the live persistent fd (R2a) so the truncation
             * lands on the exact inode the next commit re-stamps its header
             * into, back at SDB_WAL_HEADER_SIZE.
             */
            status = sdb_pager_wal_reset(pager);
        }
        /*
         * Deliberately do NOT set needs_recovery here even on a wal_clear
         * failure: the post-superblock state is fully self-consistent and
         * the database may keep accepting commits safely. The failure is
         * still surfaced to the caller via `status`.
         */
    } else if (status != SDB_OK) {
        /*
         * Failure before the superblock was advanced. The WAL, wal_index
         * and wal_tail are untouched and remain authoritative, so flag
         * needs_recovery to force the next open to re-run recovery from
         * the (unchanged) checkpoint_lsn.
         */
        pager->needs_recovery = true;
    }
    return status;
}

sdb_status sdb_pager_close(sdb_pager *pager)
{
    sdb_status status;
    sdb_status wal_close_status;
    if (pager == NULL || !pager->open || pager->transaction_active) {
        return SDB_E_INVALID_ARGUMENT;
    }
    /*
     * Close the persistent WAL fd (R2a) first; surface its error if the
     * data-file close succeeds.
     */
    wal_close_status = sdb_pager_close_wal(pager);
    status = sdb_file_close(&pager->file);
    if (status == SDB_OK) {
        status = wal_close_status;
    }
    sdb_page_cache_destroy(&pager->cache);
    sdb_wal_index_free(&pager->wal_index);
    /*
     * Tear down the group-commit coordinator. sdb_pager_close already refuses
     * while a txn is active, and every deferred commit relocks the engine
     * mutex before returning, so no leader/follower can still be parked on
     * commit_cond here.
     */
    if (pager->commit_coord_ready) {
        sdb_cond_destroy(&pager->commit_cond);
        sdb_mutex_destroy(&pager->commit_mutex);
        pager->commit_coord_ready = false;
    }
    free(pager->wal_path);
    pager->wal_path = NULL;
    sdb_secure_zero(pager->data_key, sizeof(pager->data_key));
    pager->encryption_enabled = false;
    pager->wal_tail = 0U;
    pager->current_lsn = 0U;
    pager->checkpoint_threshold = 0U;
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
        || pager->wal_index.size != 0U
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
        status = sdb_pager_store_superblock(pager, &next);
    }
    return status;
}

/*
 * Core page writer, factored out of sdb_pager_write so sdb_pager_allocate can
 * write a freshly-grown page image at its computed offset BEFORE the superblock
 * publishes the incremented next_page_id (write-then-publish, invariant A.2).
 * The caller supplies the already-validated offset; this routine performs no
 * next_page_id guard, so it must only be reached with an offset the caller has
 * bounds-checked via sdb_pager_offset.
 */
static sdb_status sdb_pager_write_core(
    sdb_pager *pager,
    uint64_t page_id,
    uint64_t offset,
    uint16_t type,
    uint64_t page_lsn,
    const uint8_t *payload,
    size_t payload_size
)
{
    uint8_t *page;
    sdb_status status;
    page = (uint8_t *)malloc((size_t)pager->superblock.page_size);
    if (page == NULL) {
        return SDB_E_OUT_OF_MEMORY;
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

        if (pager->encryption_enabled) {
            uint8_t *plain = (uint8_t *)malloc(
                (size_t)pager->superblock.page_size
            );
            bool cached = false;
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
                    cached = true;
                }
                sdb_secure_zero(
                    plain, (size_t)pager->superblock.page_size
                );
                free(plain);
            }
            if (!cached) {
                sdb_page_cache_remove(&pager->cache, page_id);
            }
        } else {
            sdb_page_cache_put(&pager->cache, page_id, page);
        }
    }
    free(page);
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
    return sdb_pager_write_core(
        pager, page_id, offset, type, page_lsn, payload, payload_size
    );
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
    uint64_t wal_offset;
    sdb_status status;
    if (pager == NULL || !pager->open || pager->needs_recovery
        || page_buffer == NULL || view_out == NULL
        || page_id == 0U || page_id >= pager->superblock.next_page_id) {
        return SDB_E_INVALID_ARGUMENT;
    }
    if (page_buffer_size < (size_t)pager->superblock.page_size) {
        return SDB_E_BUFFER_TOO_SMALL;
    }

    const bool from_cache = sdb_page_cache_get(
        &pager->cache, page_id, page_buffer
    );
    if (!from_cache
        && sdb_wal_index_get(&pager->wal_index, page_id, &wal_offset)) {
        /*
         * Read-through WAL: the newest copy of this page lives in a WAL
         * frame at wal_offset. Frame layout is the v5 self-describing header
         * [page_id | txn_id | frame_index | frame_count | file_id] followed by
         * [page bytes (page_size, encrypted if enabled)]. We only need
         * the page bytes — they're byte-identical to what would land in
         * the data file after checkpoint, so the existing decode/decrypt
         * path below works unchanged.
         */
        const size_t page_size = (size_t)pager->superblock.page_size;
        sdb_file wal;
        sdb_file *wal_ptr;
        bool wal_opened = false;
        sdb_status close_status;
        if (pager->wal_path == NULL
            || wal_offset > pager->wal_tail
            || (uint64_t)SDB_WAL_RECORD_HEADER_SIZE_V5
                > pager->wal_tail - wal_offset
            || (uint64_t)page_size
                > pager->wal_tail - wal_offset
                    - (uint64_t)SDB_WAL_RECORD_HEADER_SIZE_V5) {
            return SDB_E_CORRUPT;
        }
        /*
         * R2a Windows-safety, same as the checkpoint reader above: a wal_index
         * hit means a txn was committed, so pager->wal_file is open. Read the
         * frame through that persistent fd instead of a second OPEN_EXISTING —
         * a second handle while the writable fd holds the .wal trips
         * ERROR_SHARING_VIOLATION on Windows (the WAL share mask lacks
         * FILE_SHARE_WRITE), turning a valid committed-page read into
         * SDB_E_IO. Fall back to a private read handle only if the persistent
         * fd is somehow not open, and close only what we opened.
         */
        if (pager->wal_file_open) {
            wal_ptr = &pager->wal_file;
        } else {
            status = sdb_file_open_existing(pager->wal_path, false, &wal);
            if (status != SDB_OK) {
                return status;
            }
            wal_opened = true;
            wal_ptr = &wal;
        }
        status = sdb_file_read_full(
            wal_ptr,
            wal_offset + (uint64_t)SDB_WAL_RECORD_HEADER_SIZE_V5,
            page_buffer,
            page_size
        );
        if (wal_opened) {
            close_status = sdb_file_close(&wal);
            if (status == SDB_OK) {
                status = close_status;
            }
        }
        if (status != SDB_OK) {
            return status;
        }
    } else if (!from_cache) {
        status = sdb_pager_offset(pager, page_id, &offset);
        if (status != SDB_OK) {
            return status;
        }
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

        if (view_out->type != (uint16_t)SDB_PAGE_TYPE_ENCRYPTED) {
            return SDB_E_CORRUPT;
        }
        if (plaintext_capacity == 0U) {
            return SDB_E_CORRUPT;
        }
        plaintext = (uint8_t *)malloc(plaintext_capacity);
        if (plaintext == NULL) {
            return SDB_E_OUT_OF_MEMORY;
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

        status = SDB_E_CORRUPT;
    }
    if (status == SDB_OK && !from_cache) {

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
            return SDB_E_OUT_OF_MEMORY;
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
        /*
         * Write-then-publish (invariant A.2): initialize the freshly grown page
         * image BEFORE advancing next_page_id. If the process crashes after the
         * resize/write but before the superblock advance, the page sits in the
         * trailing region past the still-old next_page_id, which
         * sdb_pager_canonicalize_file_size reclaims on the next open — no
         * uninitialized orphan is left inside the committed page range.
         * next_page_id is published only once the page write and its fsync
         * have succeeded.
         */
        status = sdb_generation_bump(&next.generation);
        if (status != SDB_OK) {
            return status;
        }
        status = sdb_pager_write_core(
            pager,
            page_id,
            offset,
            (uint16_t)SDB_PAGE_TYPE_DATA,
            next.generation,
            NULL,
            0U
        );
        if (status != SDB_OK) {
            return status;
        }
        next.next_page_id = page_id + 1U;
        status = sdb_pager_store_superblock(pager, &next);
        if (status == SDB_OK) {
            *page_id_out = page_id;
        }
        return status;
    }
    status = sdb_generation_bump(&next.generation);
    if (status != SDB_OK) {
        return status;
    }
    status = sdb_pager_store_superblock(pager, &next);
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
        return SDB_E_OUT_OF_MEMORY;
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
    return sdb_pager_store_superblock(pager, &next);
}

static void sdb_txn_release(sdb_txn *txn)
{
    size_t index;
    if (txn == NULL) {
        return;
    }
    for (index = 0U; index < txn->page_count; ++index) {

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
            return SDB_E_OUT_OF_MEMORY;
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
            return SDB_E_OUT_OF_MEMORY;
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
                return SDB_E_OUT_OF_MEMORY;
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
            return SDB_E_OUT_OF_MEMORY;
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
                    /*
                     * Scrub the cancelled data-page payload before freeing so
                     * plaintext does not linger in freed heap (matches the
                     * secure-zero policy in sdb_txn_release and the commit
                     * cache path). The free/pairing is otherwise unchanged.
                     */
                    if (txn->pages[staged].payload_size != 0U) {
                        sdb_secure_zero(
                            txn->pages[staged].payload,
                            txn->pages[staged].payload_size
                        );
                    }
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
            /*
             * Return the cancelled allocation to the freelist so its page slot
             * is reusable instead of orphaned. allocate() advanced next_page_id
             * (extend) or consumed the freelist head; cancelling must undo that
             * at the allocator level, not merely drop the staged write, or the
             * slot leaks one on-disk page per cancel and the file grows without
             * bound. No read-back: the page was never committed, so its on-disk
             * bytes are irrelevant; stage a fresh FREE record linking the
             * current freelist head, exactly as freeing a committed page does.
             */
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
    }

    {
        uint8_t *page = (uint8_t *)malloc(
            (size_t)txn->pager->superblock.page_size
        );
        sdb_page_view view;
        if (page == NULL) {
            return SDB_E_OUT_OF_MEMORY;
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

/*
 * WAL-mode commit (Task 7):
 *
 *   - Encode every dirty page into wal_pages / storage (existing logic).
 *   - Ensure the persistent WAL fd (R2a) is open, call
 *     sdb_wal_append_txn(&pager->wal_file, pager->wal_tail, ...) — this
 *     writes N frames + one v4 commit-record with a SINGLE fsync,
 *     preserving the absolute-durability guarantee (running-CRC torn
 *     detection makes the collapsed fsync safe).
 *   - On success: bump each page's entry in wal_index to point at the
 *     frame we just wrote, update wal_tail + current_lsn, update the
 *     page cache with the fresh page bytes, mirror next_page_id /
 *     freelist_page / checkpoint_lsn into the in-memory superblock so
 *     later ops see the new state. The DATA FILE is NOT written; the
 *     on-disk superblock is NOT written. Recovery on next open replays
 *     the WAL into the data file.
 *   - Task 5 will add the "wal_tail >= checkpoint_threshold" trigger
 *     here; for this dispatch we leave a marker.
 */
sdb_status sdb_txn_commit(sdb_txn *txn)
{
    sdb_pager *pager;
    sdb_wal_page *wal_pages;
    uint8_t *storage;
    uint64_t txn_id;
    uint64_t new_wal_tail = 0U;
    size_t frame_size;
    size_t index;
    sdb_status status = SDB_OK;
    bool defer;
    uint8_t *pending_buffer = NULL;
    size_t pending_total_size = 0U;
    uint64_t pending_write_offset = 0U;
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
    /*
     * R4: snapshot the deferred-commit arm once. When set (auto-commit engine
     * wrapper, under database->mutex) this txn is staged into the group-commit
     * queue and made durable later by a leader; when clear (explicit txns,
     * internal btree_put/delete, compact) the historical synchronous
     * single-fsync path runs unchanged.
     */
    defer = pager->defer_commit;
    if (pager->current_lsn == UINT64_MAX
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
    txn_id = pager->current_lsn + 1U;
    frame_size = sdb_wal_frame_size_v5((size_t)pager->superblock.page_size);
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
        return SDB_E_OUT_OF_MEMORY;
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
        /*
         * Ensure the persistent WAL fd (R2a) is open — created lazily on the
         * first commit (fresh DB, or a WAL freshly cleared during open
         * recovery) and then reused by every subsequent commit rather than
         * reopened. Writes at wal_tail create a sparse zero-hole over bytes
         * [0, SDB_WAL_HEADER_SIZE) on first append; recover_all skips that
         * region unconditionally. sdb_pager_close closes the fd.
         */
        status = sdb_pager_ensure_wal_open(pager);
    }
    if (status == SDB_OK) {
        if (defer) {
            /*
             * R4 PREPARE: encode the txn and PWRITE its bytes to the WAL with
             * NO fsync. The pwrite only touches the OS page cache (cheap, no
             * disk flush), so it stays under the engine mutex; crucially the
             * bytes are now readable through the SAME fd, so when this txn's
             * wal_index entries are published below a following txn's
             * read-through-WAL (even after a page-cache eviction) finds them on
             * disk instead of past EOF. Only the fsync — the real durability
             * bottleneck — is deferred to the leader in
             * sdb_pager_commit_durable, which coalesces many txns behind ONE
             * fsync. durable_lsn (the "reported committed" watermark) advances
             * only after that fsync, so no txn is acknowledged before its bytes
             * are truly durable.
             */
            status = sdb_wal_encode_txn(
                &txn->target_superblock,
                pager->wal_tail,
                txn_id,
                wal_pages,
                txn->page_count,
                &pending_buffer,
                &pending_total_size,
                &pending_write_offset,
                &new_wal_tail
            );
            if (status == SDB_OK) {
                status = sdb_file_write_full(
                    &pager->wal_file,
                    pending_write_offset,
                    pending_buffer,
                    pending_total_size
                );
                free(pending_buffer);
                pending_buffer = NULL;
            }
        } else {
            status = sdb_wal_append_txn(
                &pager->wal_file,
                pager->wal_tail,
                &txn->target_superblock,
                txn_id,
                wal_pages,
                txn->page_count,
                &new_wal_tail
            );
        }
    }
    if (status == SDB_OK) {
        /*
         * Register each page's frame offset in the WAL index so Task 6
         * can serve read-through-WAL lookups. Newest-wins: upsert
         * overwrites the previous offset for a page. Frame i lives at
         * pager->wal_tail + i * frame_size (append_txn wrote frames
         * back-to-back starting at wal_tail).
         */
        for (index = 0U; index < txn->page_count; ++index) {
            const uint64_t frame_offset = pager->wal_tail
                + (uint64_t)index * (uint64_t)frame_size;
            status = sdb_wal_index_put(
                &pager->wal_index,
                wal_pages[index].page_id,
                frame_offset
            );
            if (status != SDB_OK) {
                break;
            }
        }
    }
    if (status == SDB_OK) {
        /*
         * Grow the data file to match the txn's new next_page_id. WAL-
         * mode commit does NOT write page contents to the data file —
         * they live in the WAL — but the file must still be large
         * enough that data-file offsets computed from page_ids are
         * valid slots (btree_verify_internal, sdb_pager_canonicalize_
         * file_size, and callers that mmap or size-check the DB all
         * assume file_size >= data_offset + (next_page_id-1)*page_size).
         * ftruncate to grow is O(1) — the fresh space stays as a hole
         * on disk. No fsync: crash before the resize is durable is
         * harmless because open recovery re-applies the WAL frames,
         * and sdb_file_write_full extends the file as needed while
         * writing them out.
         */
        const uint64_t data_offset =
            (uint64_t)(SDB_SUPERBLOCK_SLOT_SIZE * SDB_SUPERBLOCK_SLOT_COUNT);
        const uint64_t next_page_id = txn->target_superblock.next_page_id;
        if (next_page_id == 0U
            || next_page_id - 1U
                > (UINT64_MAX - data_offset)
                    / (uint64_t)pager->superblock.page_size) {
            status = SDB_E_OVERFLOW;
        } else {
            const uint64_t expected_size = data_offset
                + (next_page_id - 1U)
                    * (uint64_t)pager->superblock.page_size;
            uint64_t current_size = 0U;
            status = sdb_file_size(&pager->file, &current_size);
            if (status == SDB_OK && current_size < expected_size) {
                status = sdb_file_resize(&pager->file, expected_size);
            }
        }
    }
    if (status == SDB_OK) {
        /*
         * Update the in-memory superblock so subsequent operations see
         * the new next_page_id / freelist_page / checkpoint_lsn. This
         * mirrors what open-time recovery will persist on the next
         * pager open. The on-disk superblock is deliberately NOT
         * touched — that's the whole point of deferred checkpoint.
         */
        sdb_superblock_v1 next = txn->target_superblock;
        next.checkpoint_lsn = txn_id;
        next.generation = pager->superblock.generation;
        status = sdb_generation_bump(&next.generation);
        if (status == SDB_OK) {
            pager->superblock = next;
            pager->wal_tail = new_wal_tail;
            pager->current_lsn = txn_id;
        }
    }
    if (status == SDB_OK) {
        /*
         * Update the page cache with the fresh page bytes so intra-
         * session reads see committed data. For encrypted pages the
         * cache stores the plaintext form (matches sdb_pager_write).
         */
        uint8_t *plain_scratch = NULL;
        if (pager->encryption_enabled) {
            plain_scratch = (uint8_t *)malloc(
                (size_t)pager->superblock.page_size
            );
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
                } else {
                    sdb_page_cache_remove(
                        &pager->cache, wal_pages[index].page_id
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
    }
    if (status == SDB_OK && defer) {
        /*
         * R4 PREPARE tail: the txn's bytes are already pwritten; record that
         * this txn (and everything before it) now needs an fsync, and hand the
         * engine wrapper the pending ticket so it drives durability after
         * releasing the engine mutex. Re-check commit_failed under commit_mutex
         * so a batch poisoned by a concurrent leader fsync failure surfaces the
         * sticky error here instead of a false success.
         */
        sdb_mutex_lock(&pager->commit_mutex);
        if (pager->commit_failed) {
            status = pager->commit_error;
        } else {
            if (txn_id > pager->written_lsn) {
                pager->written_lsn = txn_id;
            }
            pager->pending_valid = true;
            pager->pending_txn_id = txn_id;
        }
        sdb_mutex_unlock(&pager->commit_mutex);
    }
    if (status != SDB_OK) {
        pager->needs_recovery = true;
    }
    if (pending_buffer != NULL) {
        /* Encode succeeded but pwrite failed before free, or defer was off. */
        free(pending_buffer);
        pending_buffer = NULL;
    }
    free(storage);
    free(wal_pages);
    sdb_txn_release(txn);
    if (!defer && status == SDB_OK
        && pager->wal_tail >= pager->checkpoint_threshold) {
        /*
         * Auto-checkpoint trigger (synchronous path only). Once the WAL grows
         * past the threshold, drain it into the data file so it cannot grow
         * without bound (sdb_wal_append_txn ultimately rejects appends at
         * SDB_WAL_MAX_BYTES). Run it AFTER sdb_txn_release, which clears
         * transaction_active — otherwise sdb_pager_checkpoint's guard rejects
         * the call. The commit is already durable here (the single fsync inside
         * sdb_wal_append_txn), so this is a space-reclaim step; on failure the
         * WAL is left intact and needs_recovery is flagged. The deferred
         * (group-commit) path defers the same trigger to the engine wrapper via
         * sdb_pager_group_checkpoint_if_due, which only fires once the
         * coordinator is fully drained so no leader can be mid-fsync against a
         * WAL being truncated.
         *
         * The commit is ALREADY durable, so a checkpoint failure must NOT be
         * reported as a commit failure: we flag needs_recovery (so the next op
         * is rejected until a reopen re-runs recovery from the intact WAL) but
         * deliberately keep `status` at SDB_OK. Overwriting it would tell the
         * caller a durably-committed write failed, tempting a non-idempotent
         * retry.
         */
        if (sdb_pager_checkpoint(pager) != SDB_OK) {
            pager->needs_recovery = true;
        }
    }
    return status;
}

void sdb_txn_abort(sdb_txn *txn)
{
    sdb_txn_release(txn);
}

#if SDB_TESTING
static bool sdb_pager_group_flush_fail_for_testing = false;
void sdb_pager_group_commit_fail_next_flush_for_testing(void)
{
    sdb_pager_group_flush_fail_for_testing = true;
}
void sdb_pager_group_commit_clear_failure_for_testing(void)
{
    sdb_pager_group_flush_fail_for_testing = false;
}
#endif

void sdb_pager_set_defer_commit(sdb_pager *pager, bool defer)
{
    if (pager != NULL) {
        pager->defer_commit = defer;
        if (defer) {
            pager->pending_valid = false;
        }
    }
}

void sdb_pager_set_sync_relaxed(sdb_pager *pager, bool relaxed)
{
    if (pager != NULL) {
        pager->sync_relaxed = relaxed;
    }
}

bool sdb_pager_take_pending(sdb_pager *pager, uint64_t *txn_id_out)
{
    if (pager == NULL || !pager->pending_valid) {
        return false;
    }
    if (txn_id_out != NULL) {
        *txn_id_out = pager->pending_txn_id;
    }
    pager->pending_valid = false;
    return true;
}

/*
 * R4 leader fsync. Runs with commit_mutex held and leader_active == true.
 * Every txn's WAL bytes were already pwritten (in-order, contiguous) during
 * PREPARE, so the leader only needs ONE fsync to make the whole pwritten range
 * durable. It snapshots `written_lsn` (the highest txn whose bytes are on
 * disk), releases the mutex for the slow fsync so other threads keep
 * PREPARING, then advances `durable_lsn` to that snapshot. Because fsync is an
 * fd-level barrier it flushes every byte written so far — including any bytes a
 * concurrent synchronous (explicit) commit appended after our snapshot — so
 * the WAL is never left with a durable-gap in front of an acknowledged txn. On
 * fsync error the coordinator is poisoned (sticky commit_failed); every waiter
 * then observes the error and no txn is ever reported durable falsely.
 */
static sdb_status sdb_pager_fsync_locked(sdb_pager *pager)
{
    const uint64_t target = pager->written_lsn;
    sdb_status io_status = SDB_OK;
    if (pager->durable_lsn >= target) {
        return SDB_OK;
    }
    /*
     * SDB_SYNCHRONOUS_NORMAL: the txn's WAL bytes are already pwritten (in the
     * OS page cache and visible to read-through-WAL). Acknowledge without the
     * fsync barrier and let durability fold into the next checkpoint's fsync.
     * durable_lsn is guarded by commit_mutex, which is held here, so this needs
     * no unlock/relock and does not touch the leader/follower protocol. Commits
     * survive a process crash (bytes remain in the OS cache) but the tail since
     * the last checkpoint can be lost on power loss; checkpoint fsyncs the WAL
     * before copying frames, so there is never corruption.
     */
    if (pager->sync_relaxed) {
        pager->durable_lsn = target;
        return SDB_OK;
    }
    sdb_mutex_unlock(&pager->commit_mutex);
#if SDB_TESTING
    if (sdb_pager_group_flush_fail_for_testing) {
        sdb_pager_group_flush_fail_for_testing = false;
        io_status = SDB_E_IO;
    }
#endif
    if (io_status == SDB_OK) {
        io_status = sdb_file_sync(&pager->wal_file);
    }
    sdb_mutex_lock(&pager->commit_mutex);
    if (io_status == SDB_OK) {
        if (target > pager->durable_lsn) {
            pager->durable_lsn = target;
        }
    } else {
        pager->commit_failed = true;
        pager->commit_error = io_status;
    }
    return pager->commit_failed ? pager->commit_error : SDB_OK;
}

sdb_status sdb_pager_commit_durable(sdb_pager *pager, uint64_t txn_id)
{
    sdb_status result;
    if (pager == NULL) {
        return SDB_E_INVALID_ARGUMENT;
    }
    sdb_mutex_lock(&pager->commit_mutex);
    for (;;) {
        if (pager->durable_lsn >= txn_id) {
            result = SDB_OK;
            break;
        }
        if (pager->commit_failed) {
            result = pager->commit_error;
            break;
        }
        if (!pager->leader_active) {
            /*
             * Become the leader and fsync. One fsync covers everything
             * pwritten so far (written_lsn >= txn_id, since our bytes were
             * pwritten before we got here). Re-check the predicate at the top
             * afterwards rather than trusting the return, so a txn pwritten
             * after our snapshot simply drives a fresh leader.
             */
            pager->leader_active = true;
            (void)sdb_pager_fsync_locked(pager);
            pager->leader_active = false;
            sdb_cond_broadcast(&pager->commit_cond);
            continue;
        }
        /* A leader is fsyncing; wait for it to advance durable_lsn/poison. */
        sdb_cond_wait(&pager->commit_cond, &pager->commit_mutex);
    }
    sdb_mutex_unlock(&pager->commit_mutex);
    return result;
}

sdb_status sdb_pager_group_checkpoint_if_due(sdb_pager *pager)
{
    sdb_status status;
    if (pager == NULL) {
        return SDB_E_INVALID_ARGUMENT;
    }
    if (pager->needs_recovery) {
        return SDB_OK;
    }
    /*
     * Once the WAL crosses the checkpoint threshold we MUST drain and
     * checkpoint — not merely skip when a committer happens to be in flight.
     * Under sustained concurrent load the coordinator is almost never idle at
     * this exact point, so the old "only if fully drained, else skip" check
     * meant checkpoint effectively never ran: the WAL then grew without bound
     * to SDB_WAL_MAX_BYTES and every commit began to fail. Instead, wait for
     * the coordinator to drain, then checkpoint.
     *
     * This wait terminates and cannot deadlock: the caller holds
     * database->mutex, so no NEW txn can PREPARE (they block on the engine
     * mutex, so written_lsn is frozen here). The finite set of already-pwritten
     * txns runs its off-mutex durability round (which needs only commit_mutex,
     * released by sdb_cond_wait), advancing durable_lsn and broadcasting until
     * durable_lsn == written_lsn with no active leader. Draining first
     * guarantees no leader is mid-fsync against the WAL we are about to
     * truncate.
     */
    sdb_mutex_lock(&pager->commit_mutex);
    if (pager->wal_tail < pager->checkpoint_threshold || pager->commit_failed) {
        sdb_mutex_unlock(&pager->commit_mutex);
        return SDB_OK;
    }
    while (!pager->commit_failed
        && (pager->leader_active
            || pager->durable_lsn != pager->written_lsn)) {
        sdb_cond_wait(&pager->commit_cond, &pager->commit_mutex);
    }
    if (pager->commit_failed) {
        sdb_mutex_unlock(&pager->commit_mutex);
        return SDB_OK;
    }
    sdb_mutex_unlock(&pager->commit_mutex);
    status = sdb_pager_checkpoint(pager);
    if (status != SDB_OK) {
        pager->needs_recovery = true;
    }
    return status;
}

#if SDB_TESTING
void sdb_pager_checkpoint_fail_at_for_testing(
    sdb_checkpoint_fail_stage stage
)
{
    sdb_checkpoint_fail_stage_for_testing = stage;
}

void sdb_pager_checkpoint_clear_failure_for_testing(void)
{
    sdb_checkpoint_fail_stage_for_testing = SDB_CHECKPOINT_FAIL_NONE;
}
#endif
