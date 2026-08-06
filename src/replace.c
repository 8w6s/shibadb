#include "replace.h"

#include "file.h"
#include "internal.h"

#include <stdlib.h>
#include <string.h>

#define SDB_REPLACE_MARKER_SIZE ((size_t)32)
#define SDB_REPLACE_CHECKSUM_OFFSET ((size_t)24)

static const uint8_t sdb_replace_magic[4] = {
    (uint8_t)'S', (uint8_t)'B', (uint8_t)'R', (uint8_t)'1'
};

static sdb_status sdb_replace_path(
    const char *database_path, const char *suffix, char **path_out
)
{
    size_t database_size;
    size_t suffix_size;
    char *path;
    if (database_path == NULL || suffix == NULL || path_out == NULL) {
        return SDB_E_INVALID_ARGUMENT;
    }
    database_size = strlen(database_path);
    suffix_size = strlen(suffix);
    if (database_size > SIZE_MAX - suffix_size - 1U) {
        return SDB_E_OVERFLOW;
    }
    path = (char *)malloc(database_size + suffix_size + 1U);
    if (path == NULL) {
        return SDB_E_OUT_OF_MEMORY;
    }
    (void)memcpy(path, database_path, database_size);
    (void)memcpy(path + database_size, suffix, suffix_size + 1U);
    *path_out = path;
    return SDB_OK;
}

static void sdb_replace_marker_encode(
    const uint8_t file_id[SDB_FILE_ID_SIZE],
    uint8_t marker[SDB_REPLACE_MARKER_SIZE]
)
{
    (void)memset(marker, 0, SDB_REPLACE_MARKER_SIZE);
    (void)memcpy(marker, sdb_replace_magic, sizeof(sdb_replace_magic));
    sdb_write_u16_le(marker + 4U, UINT16_C(1));
    sdb_write_u16_le(marker + 6U, (uint16_t)SDB_REPLACE_MARKER_SIZE);
    (void)memcpy(marker + 8U, file_id, SDB_FILE_ID_SIZE);
    sdb_write_u32_le(
        marker + SDB_REPLACE_CHECKSUM_OFFSET,
        sdb_crc32_zeroed_range(
            marker,
            SDB_REPLACE_MARKER_SIZE,
            SDB_REPLACE_CHECKSUM_OFFSET,
            sizeof(uint32_t)
        )
    );
}

static sdb_status sdb_replace_marker_decode(
    const uint8_t marker[SDB_REPLACE_MARKER_SIZE],
    uint8_t file_id_out[SDB_FILE_ID_SIZE]
)
{
    size_t index;
    if (memcmp(marker, sdb_replace_magic, sizeof(sdb_replace_magic)) != 0
        || sdb_read_u16_le(marker + 4U) != UINT16_C(1)
        || sdb_read_u16_le(marker + 6U)
            != (uint16_t)SDB_REPLACE_MARKER_SIZE
        || sdb_read_u32_le(marker + SDB_REPLACE_CHECKSUM_OFFSET)
            != sdb_crc32_zeroed_range(
                marker,
                SDB_REPLACE_MARKER_SIZE,
                SDB_REPLACE_CHECKSUM_OFFSET,
                sizeof(uint32_t)
            )) {
        return SDB_E_CORRUPT;
    }
    for (index = 28U; index < SDB_REPLACE_MARKER_SIZE; ++index) {
        if (marker[index] != 0U) {
            return SDB_E_CORRUPT;
        }
    }
    (void)memcpy(file_id_out, marker + 8U, SDB_FILE_ID_SIZE);
    return SDB_OK;
}

sdb_status sdb_replace_prepare(
    const char *database_path,
    const uint8_t replacement_file_id[SDB_FILE_ID_SIZE]
)
{
    sdb_file file;
    char *marker_path = NULL;
    char *temporary_path = NULL;
    uint8_t marker[SDB_REPLACE_MARKER_SIZE];
    bool temporary_exists = false;
    bool file_open = false;
    sdb_status status;
    if (replacement_file_id == NULL) {
        return SDB_E_INVALID_ARGUMENT;
    }
    status = sdb_replace_path(database_path, ".replace", &marker_path);
    if (status == SDB_OK) {
        status = sdb_replace_path(
            database_path, ".replace.tmp", &temporary_path
        );
    }
    if (status == SDB_OK) {

        (void)sdb_file_remove(temporary_path, true);
        status = sdb_file_create_new(temporary_path, &file);
        temporary_exists = status == SDB_OK;
        file_open = status == SDB_OK;
    }
    if (status == SDB_OK) {
        sdb_replace_marker_encode(replacement_file_id, marker);
        status = sdb_file_write_full(
            &file, 0U, marker, sizeof(marker)
        );
    }
    if (status == SDB_OK) {
        status = sdb_file_sync(&file);
    }
    if (file_open) {
        const sdb_status close_status = sdb_file_close(&file);
        if (status == SDB_OK) {
            status = close_status;
        }
    }
    if (status == SDB_OK) {
        status = sdb_file_replace(
            temporary_path, marker_path, true
        );
        if (status == SDB_OK) {
            temporary_exists = false;
        }
    }
    if (status == SDB_OK) {
        status = sdb_file_sync_parent_directory(database_path);
    }
    if (temporary_exists) {
        (void)sdb_file_remove(temporary_path, true);
    }
    free(marker_path);
    free(temporary_path);
    return status;
}

sdb_status sdb_replace_finish(const char *database_path)
{
    char *wal_path = NULL;
    char *marker_path = NULL;
    sdb_status status = sdb_replace_path(
        database_path, ".wal", &wal_path
    );
    if (status == SDB_OK) {
        status = sdb_replace_path(
            database_path, ".replace", &marker_path
        );
    }

    /*
     * Remove the stale WAL BEFORE the .replace marker (each with a dir fsync),
     * so the marker always outlives the WAL. The marker is what
     * sdb_replace_recover keys on: if we crash after removing the WAL but
     * before the marker, recover still finds the marker, matches the (already
     * swapped) data file's file_id, and re-runs finish — whose WAL removal is a
     * harmless no-op (sdb_file_remove uses missing_ok). The reverse order left
     * a window where a crash between the two unlinks stranded a swapped data
     * file next to a stale source WAL with no marker: recover returned OK, the
     * foreign WAL survived into recovery and either wedged the DB
     * (identity-gate SDB_E_CORRUPT) or silently replayed over the swapped tree
     * on the legacy path.
     */
    if (status == SDB_OK) {
        status = sdb_file_remove(wal_path, true);
    }
    if (status == SDB_OK) {
        status = sdb_file_sync_parent_directory(database_path);
    }
    if (status == SDB_OK) {
        status = sdb_file_remove(marker_path, true);
    }
    if (status == SDB_OK) {
        status = sdb_file_sync_parent_directory(database_path);
    }
    free(wal_path);
    free(marker_path);
    return status;
}

sdb_status sdb_replace_abort(const char *database_path)
{
    char *marker_path = NULL;
    sdb_status status = sdb_replace_path(
        database_path, ".replace", &marker_path
    );
    if (status == SDB_OK) {
        status = sdb_file_remove(marker_path, true);
    }
    if (status == SDB_OK) {
        status = sdb_file_sync_parent_directory(database_path);
    }
    free(marker_path);
    return status;
}

sdb_status sdb_replace_recover(
    const char *database_path,
    const uint8_t current_file_id[SDB_FILE_ID_SIZE]
)
{
    sdb_file file;
    char *marker_path = NULL;
    char *temporary_path = NULL;
    uint8_t marker[SDB_REPLACE_MARKER_SIZE];
    uint8_t replacement_file_id[SDB_FILE_ID_SIZE];
    sdb_status status;
    if (current_file_id == NULL) {
        return SDB_E_INVALID_ARGUMENT;
    }

    if (sdb_replace_path(
            database_path, ".replace.tmp", &temporary_path
        ) == SDB_OK) {
        (void)sdb_file_remove(temporary_path, true);
        free(temporary_path);
    }
    status = sdb_replace_path(database_path, ".replace", &marker_path);
    if (status != SDB_OK) {
        return status;
    }
    status = sdb_file_open_existing(marker_path, false, &file);
    if (status != SDB_OK) {
        free(marker_path);
        return status == SDB_E_NOT_FOUND ? SDB_OK : status;
    }
    status = sdb_file_read_full(&file, 0U, marker, sizeof(marker));
    (void)sdb_file_close(&file);
    if (status == SDB_OK) {
        status = sdb_replace_marker_decode(
            marker, replacement_file_id
        );
    }
    if (status == SDB_OK
        && memcmp(
            replacement_file_id, current_file_id, SDB_FILE_ID_SIZE
        ) == 0) {
        status = sdb_replace_finish(database_path);
    } else if (status == SDB_OK || status == SDB_E_CORRUPT
        || status == SDB_E_TRUNCATED || status == SDB_E_IO) {

        const sdb_status remove_status =
            sdb_file_remove(marker_path, true);
        if (remove_status != SDB_OK) {
            status = remove_status;
        } else {
            status = sdb_file_sync_parent_directory(database_path);
        }
    }
    free(marker_path);
    return status;
}
