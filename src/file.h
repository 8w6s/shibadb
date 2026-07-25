#ifndef SHIBADB_FILE_H
#define SHIBADB_FILE_H

#include "shibadb.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef _WIN32
#include <windows.h>
typedef struct sdb_file {
    HANDLE handle;
#if SDB_TESTING
    size_t io_limit;
    size_t test_operation_count;
    size_t test_fail_after;
#endif
} sdb_file;
#else
typedef struct sdb_file {
    int descriptor;
#if SDB_TESTING
    size_t io_limit;
    size_t test_operation_count;
    size_t test_fail_after;
#endif
} sdb_file;
#endif

sdb_status sdb_file_create_new(const char *path, sdb_file *file_out);
sdb_status sdb_file_open_existing(const char *path, bool writable, sdb_file *file_out);
sdb_status sdb_file_close(sdb_file *file);
sdb_status sdb_file_read_full(
    sdb_file *file, uint64_t offset, uint8_t *output, size_t size
);
sdb_status sdb_file_write_full(
    sdb_file *file, uint64_t offset, const uint8_t *input, size_t size
);
sdb_status sdb_file_sync(sdb_file *file);
sdb_status sdb_file_resize(sdb_file *file, uint64_t size);
sdb_status sdb_file_size(sdb_file *file, uint64_t *size_out);
sdb_status sdb_file_sync_parent_directory(const char *path);
sdb_status sdb_file_replace(
    const char *source, const char *destination, bool replace_existing
);
sdb_status sdb_file_remove(const char *path, bool missing_ok);
sdb_status sdb_file_path_exists(const char *path, bool *exists_out);
sdb_status sdb_file_resolve_database_path(
    const char *path, bool must_exist, char **resolved_path_out
);

#if SDB_TESTING
/* Internal deterministic short-I/O hook used by the reliability tests.
 * Gated behind SDB_TESTING so release builds neither carry the mutator
 * symbols in the public library nor pay the runtime branch on every I/O.
 */
void sdb_file_set_io_limit_for_testing(sdb_file *file, size_t limit);
void sdb_file_fail_after_for_testing(sdb_file *file, size_t successful_operations);
void sdb_file_clear_failure_for_testing(sdb_file *file);
#endif

#endif
