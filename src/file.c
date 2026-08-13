#include "file.h"

#include "internal.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if SDB_TESTING
#define SDB_FILE_IO_LIMIT(f) ((f)->io_limit)
#else
#define SDB_FILE_IO_LIMIT(f) ((void)(f), SIZE_MAX)
#endif

#if SDB_TESTING
static void sdb_file_initialize_test_state(sdb_file *file)
{
    file->io_limit = SIZE_MAX;
    file->test_operation_count = 0U;
    file->test_fail_after = SIZE_MAX;
    file->test_sync_count = 0U;
}

static bool sdb_file_should_fail_for_testing(sdb_file *file)
{
    /*
     * Fault injection OFF (default): touch nothing. R4 group commit issues
     * pwrite (under the engine mutex) and the leader's fsync (off the mutex)
     * on the SAME sdb_file from different threads; reading/incrementing the
     * counter unconditionally would be a data race in the test instrumentation
     * itself and trip the TSan gate even though no fault is armed. Arming and
     * clearing fault injection is single-threaded test setup done before any
     * concurrent commit, so short-circuiting on SIZE_MAX is race-free for the
     * group-commit tests while preserving fault injection for the
     * single-threaded crash matrix.
     */
    if (file->test_fail_after == SIZE_MAX) {
        return false;
    }
    if (file->test_operation_count >= file->test_fail_after) {
        return true;
    }
    ++file->test_operation_count;
    return false;
}
#else
static inline void sdb_file_initialize_test_state(sdb_file *file)
{
    (void)file;
}

static inline bool sdb_file_should_fail_for_testing(sdb_file *file)
{
    (void)file;
    return false;
}
#endif

#ifdef _WIN32

#include "windows_path.h"

static sdb_status sdb_file_open_windows(
    const char *path, DWORD creation, DWORD access, sdb_file *file_out
)
{
    HANDLE handle;
    wchar_t *wide_path = NULL;
    sdb_status status;
    if (path == NULL || file_out == NULL) {
        return SDB_E_INVALID_ARGUMENT;
    }
    status = sdb_windows_path_from_utf8(path, &wide_path);
    if (status != SDB_OK) {
        return status;
    }
    handle = CreateFileW(
        wide_path,
        access,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL,
        creation,
        FILE_ATTRIBUTE_NORMAL,
        NULL
    );
    if (handle == INVALID_HANDLE_VALUE) {
        /*
         * Snapshot last-error BEFORE free(): HeapFree may clobber it, which
         * would turn a genuine ERROR_FILE_NOT_FOUND into SDB_E_IO. Matches the
         * correct order already used in sdb_file_remove.
         */
        const DWORD error = GetLastError();
        free(wide_path);
        if (error == ERROR_FILE_NOT_FOUND
                || error == ERROR_PATH_NOT_FOUND) {
            return SDB_E_NOT_FOUND;
        }
        /*
         * CREATE_NEW onto an existing path (mirrors POSIX EEXIST): report
         * "already exists" as SDB_E_CONFLICT, not a generic I/O error.
         */
        if (error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS) {
            return SDB_E_CONFLICT;
        }
        if (error == ERROR_DISK_FULL || error == ERROR_HANDLE_DISK_FULL) {
            return SDB_E_NO_SPACE;
        }
        if (error == ERROR_ACCESS_DENIED || error == ERROR_WRITE_PROTECT) {
            return SDB_E_ACCESS_DENIED;
        }
        return SDB_E_IO;
    }
    free(wide_path);
    file_out->handle = handle;
    sdb_file_initialize_test_state(file_out);
    return SDB_OK;
}

sdb_status sdb_file_create_new(const char *path, sdb_file *file_out)
{
    return sdb_file_open_windows(
        path, CREATE_NEW, GENERIC_READ | GENERIC_WRITE, file_out
    );
}

sdb_status sdb_file_open_existing(const char *path, bool writable, sdb_file *file_out)
{
    const DWORD access = writable ? (GENERIC_READ | GENERIC_WRITE) : GENERIC_READ;
    return sdb_file_open_windows(path, OPEN_EXISTING, access, file_out);
}

sdb_status sdb_file_close(sdb_file *file)
{
    if (file == NULL || file->handle == INVALID_HANDLE_VALUE) {
        return SDB_E_INVALID_ARGUMENT;
    }
    if (!CloseHandle(file->handle)) {
        return SDB_E_IO;
    }
    file->handle = INVALID_HANDLE_VALUE;
    return SDB_OK;
}

static size_t sdb_windows_chunk(size_t remaining, size_t limit)
{
    size_t chunk = remaining < limit ? remaining : limit;
    if (chunk > (size_t)UINT32_MAX) {
        chunk = (size_t)UINT32_MAX;
    }
    return chunk;
}

sdb_status sdb_file_read_full(
    sdb_file *file, uint64_t offset, uint8_t *output, size_t size
)
{
    size_t completed = 0U;
    if (file == NULL || (output == NULL && size != 0U)) {
        return SDB_E_INVALID_ARGUMENT;
    }
    while (completed < size) {
        OVERLAPPED operation = {0};
        DWORD transferred = 0U;
        const size_t chunk = sdb_windows_chunk(size - completed, SDB_FILE_IO_LIMIT(file));
        uint64_t position;
        if ((uint64_t)completed > UINT64_MAX - offset) {
            return SDB_E_OVERFLOW;
        }
        position = offset + (uint64_t)completed;
        operation.Offset = (DWORD)(position & UINT64_C(0xffffffff));
        operation.OffsetHigh = (DWORD)(position >> 32U);
        if (sdb_file_should_fail_for_testing(file)) {
            return SDB_E_IO;
        }
        if (!ReadFile(
                file->handle, output + completed, (DWORD)chunk, &transferred, &operation
            )) {
            return GetLastError() == ERROR_HANDLE_EOF
                ? SDB_E_TRUNCATED : SDB_E_IO;
        }
        if (transferred == 0U) {
            return SDB_E_TRUNCATED;
        }
        completed += (size_t)transferred;
    }
    return SDB_OK;
}

sdb_status sdb_file_write_full(
    sdb_file *file, uint64_t offset, const uint8_t *input, size_t size
)
{
    size_t completed = 0U;
    if (file == NULL || (input == NULL && size != 0U)) {
        return SDB_E_INVALID_ARGUMENT;
    }
    while (completed < size) {
        OVERLAPPED operation = {0};
        DWORD transferred = 0U;
        const size_t chunk = sdb_windows_chunk(size - completed, SDB_FILE_IO_LIMIT(file));
        uint64_t position;
        if ((uint64_t)completed > UINT64_MAX - offset) {
            return SDB_E_OVERFLOW;
        }
        position = offset + (uint64_t)completed;
        operation.Offset = (DWORD)(position & UINT64_C(0xffffffff));
        operation.OffsetHigh = (DWORD)(position >> 32U);
        if (sdb_file_should_fail_for_testing(file)) {
            return SDB_E_IO;
        }
        if (!WriteFile(
                file->handle, input + completed, (DWORD)chunk, &transferred, &operation
            )) {
            const DWORD write_error = GetLastError();
            if (write_error == ERROR_DISK_FULL
                    || write_error == ERROR_HANDLE_DISK_FULL) {
                return SDB_E_NO_SPACE;
            }
            return SDB_E_IO;
        }
        if (transferred == 0U) {
            return SDB_E_IO;
        }
        completed += (size_t)transferred;
    }
    return SDB_OK;
}

sdb_status sdb_file_sync(sdb_file *file)
{
    if (file == NULL) {
        return SDB_E_INVALID_ARGUMENT;
    }
#if SDB_TESTING
    ++file->test_sync_count;
#endif
    if (sdb_file_should_fail_for_testing(file) || !FlushFileBuffers(file->handle)) {
        return SDB_E_IO;
    }
    return SDB_OK;
}

sdb_status sdb_file_resize(sdb_file *file, uint64_t size)
{
    LARGE_INTEGER position;
    if (file == NULL || size > (uint64_t)INT64_MAX) {
        return SDB_E_INVALID_ARGUMENT;
    }
    if (sdb_file_should_fail_for_testing(file)) {
        return SDB_E_IO;
    }
    position.QuadPart = (LONGLONG)size;
    if (!SetFilePointerEx(file->handle, position, NULL, FILE_BEGIN)
        || !SetEndOfFile(file->handle)) {
        return SDB_E_IO;
    }
    return SDB_OK;
}

sdb_status sdb_file_size(sdb_file *file, uint64_t *size_out)
{
    LARGE_INTEGER size;
    if (file == NULL || size_out == NULL) {
        return SDB_E_INVALID_ARGUMENT;
    }
    if (sdb_file_should_fail_for_testing(file)) {
        return SDB_E_IO;
    }
    if (!GetFileSizeEx(file->handle, &size) || size.QuadPart < 0) {
        return SDB_E_IO;
    }
    *size_out = (uint64_t)size.QuadPart;
    return SDB_OK;
}

sdb_status sdb_file_sync_parent_directory(const char *path)
{

    wchar_t *wide_directory = NULL;
    HANDLE directory_handle;
    DWORD flush_error = ERROR_SUCCESS;
    char *copy;
    char *separator;
    const char *directory;
    size_t length;
    sdb_status status;

    if (path == NULL || path[0] == '\0') {
        return SDB_E_INVALID_ARGUMENT;
    }
    length = strlen(path);

    if (length > 1U
        && (path[length - 1U] == '\\' || path[length - 1U] == '/')) {
        return SDB_E_INVALID_ARGUMENT;
    }
    copy = (char *)malloc(length + 1U);
    if (copy == NULL) {
        return SDB_E_OUT_OF_MEMORY;
    }
    (void)memcpy(copy, path, length + 1U);

    {
        char *back = strrchr(copy, '\\');
        char *fwd = strrchr(copy, '/');
        separator = back == NULL ? fwd
            : (fwd == NULL ? back : (back > fwd ? back : fwd));
    }
    if (separator == NULL) {
        directory = ".";
    } else if (separator == copy) {
        separator[1] = '\0';
        directory = copy;
    } else {
        *separator = '\0';
        directory = copy;
    }

    status = sdb_windows_path_from_utf8(directory, &wide_directory);
    if (status != SDB_OK) {
        free(copy);
        return status;
    }
    directory_handle = CreateFileW(
        wide_directory,
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS,
        NULL
    );
    if (directory_handle == INVALID_HANDLE_VALUE
        && GetLastError() == ERROR_ACCESS_DENIED) {
        directory_handle = CreateFileW(
            wide_directory,
            GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            NULL,
            OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS,
            NULL
        );
    }
    free(wide_directory);
    free(copy);
    if (directory_handle == INVALID_HANDLE_VALUE) {
        return SDB_E_IO;
    }
    if (!FlushFileBuffers(directory_handle)) {
        flush_error = GetLastError();
    }
    if (!CloseHandle(directory_handle)) {
        return SDB_E_IO;
    }
    /*
     * Windows has no POSIX-style fsync(directory). FlushFileBuffers is defined
     * only for volume and file handles; on NTFS a directory handle returns
     * ERROR_INVALID_FUNCTION (and, depending on the handle's granted access,
     * ERROR_ACCESS_DENIED or ERROR_NOT_SUPPORTED). Tolerating those is
     * DELIBERATE, not a silent swallow of a real error:
     *   - NTFS journals every namespace change (create/rename/delete) via
     *     $LogFile, so the operation is crash-recoverable from the log once it
     *     returns; there is no separate directory page to flush the way
     *     ext4/xfs require, which is why Windows exposes no directory-fsync API.
     *   - Callers that need the swap durable already use
     *     MOVEFILE_WRITE_THROUGH / REPLACEFILE_WRITE_THROUGH (sdb_file_replace)
     *     and FlushFileBuffers on the FILE handle (sdb_file_sync) before this
     *     call -- that is the Windows barrier that forces the change to stable
     *     storage. SQLite (winSync), LMDB and PostgreSQL skip directory sync on
     *     Windows for the same reason.
     * Every other failure -- including ERROR_INVALID_HANDLE, which on a handle
     * CreateFileW just returned as valid indicates a genuine fault, not an
     * unsupported operation -- must propagate instead of being hidden.
     */
    return flush_error == ERROR_SUCCESS
        || flush_error == ERROR_ACCESS_DENIED
        || flush_error == ERROR_INVALID_FUNCTION
        || flush_error == ERROR_NOT_SUPPORTED
        ? SDB_OK : SDB_E_IO;
}

sdb_status sdb_file_replace(
    const char *source, const char *destination, bool replace_existing
)
{
    DWORD flags = MOVEFILE_WRITE_THROUGH;
    wchar_t *wide_source = NULL;
    wchar_t *wide_destination = NULL;
    sdb_status status;
    bool moved;
    if (source == NULL || destination == NULL) {
        return SDB_E_INVALID_ARGUMENT;
    }
    status = sdb_windows_path_from_utf8(source, &wide_source);
    if (status == SDB_OK) {
        status = sdb_windows_path_from_utf8(
            destination, &wide_destination
        );
    }
    if (status != SDB_OK) {
        free(wide_source);
        free(wide_destination);
        return status;
    }
    if (replace_existing) {
        moved = ReplaceFileW(
            wide_destination,
            wide_source,
            NULL,
            REPLACEFILE_WRITE_THROUGH,
            NULL,
            NULL
        ) != 0;
        if (!moved) {
            const DWORD replace_error = GetLastError();
            if (replace_error == ERROR_FILE_NOT_FOUND
                || replace_error == ERROR_PATH_NOT_FOUND) {
                moved = MoveFileExW(
                    wide_source,
                    wide_destination,
                    flags | MOVEFILE_REPLACE_EXISTING
                ) != 0;
            }
        }
    } else {
        moved = MoveFileExW(wide_source, wide_destination, flags) != 0;
    }
    if (!moved) {
#if SDB_TESTING
        (void)fprintf(
            stderr, "shibadb test diagnostic: Windows replace error %lu\n",
            (unsigned long)GetLastError()
        );
#endif
    }
    free(wide_source);
    free(wide_destination);
    return moved ? SDB_OK : SDB_E_IO;
}

sdb_status sdb_file_remove(const char *path, bool missing_ok)
{
    wchar_t *wide_path = NULL;
    sdb_status status;
    DWORD error;
    if (path == NULL) {
        return SDB_E_INVALID_ARGUMENT;
    }
    status = sdb_windows_path_from_utf8(path, &wide_path);
    if (status != SDB_OK) {
        return status;
    }
    if (DeleteFileW(wide_path)) {
        free(wide_path);
        return SDB_OK;
    }
    error = GetLastError();
    free(wide_path);
    return missing_ok && error == ERROR_FILE_NOT_FOUND
        ? SDB_OK : SDB_E_IO;
}

sdb_status sdb_file_resolve_database_path(
    const char *path, bool must_exist, char **resolved_path_out
)
{
    wchar_t *wide_path = NULL;
    wchar_t *resolved_wide = NULL;
    HANDLE handle = INVALID_HANDLE_VALUE;
    BY_HANDLE_FILE_INFORMATION information;
    DWORD required;
    DWORD written;
    size_t path_length;
    size_t character_count;
    size_t allocation_size;
    sdb_status status;
    if (path == NULL || path[0] == '\0' || resolved_path_out == NULL) {
        return SDB_E_INVALID_ARGUMENT;
    }
    *resolved_path_out = NULL;
    path_length = strlen(path);
    if (!must_exist) {
        const char *back = strrchr(path, '\\');
        const char *forward = strrchr(path, '/');
        const char *separator = back == NULL ? forward
            : (forward == NULL ? back : (back > forward ? back : forward));
        const char *name = separator == NULL ? path : separator + 1;
        if (path_length == 0U
            || path[path_length - 1U] == '\\'
            || path[path_length - 1U] == '/'
            || strcmp(name, ".") == 0
            || strcmp(name, "..") == 0) {
            return SDB_E_INVALID_ARGUMENT;
        }
    }
    status = sdb_windows_path_from_utf8(path, &wide_path);
    if (status != SDB_OK) {
        return status;
    }
    if (!must_exist) {
        required = GetFullPathNameW(wide_path, 0U, NULL, NULL);
        if (required == 0U
            || !sdb_checked_mul_size(
                (size_t)required, sizeof(*resolved_wide), &allocation_size
            )) {
            free(wide_path);
            return SDB_E_IO;
        }
        resolved_wide = (wchar_t *)malloc(allocation_size);
        if (resolved_wide == NULL) {
            free(wide_path);
            return SDB_E_OUT_OF_MEMORY;
        }
        written = GetFullPathNameW(
            wide_path, required, resolved_wide, NULL
        );
        free(wide_path);
        if (written == 0U || written >= required) {
            free(resolved_wide);
            return SDB_E_IO;
        }
        {
            wchar_t *separator = wcsrchr(resolved_wide, L'\\');
            wchar_t saved;
            DWORD attributes;
            if (separator == NULL) {
                free(resolved_wide);
                return SDB_E_IO;
            }
            if (separator == resolved_wide + 2
                && resolved_wide[1] == L':') {
                saved = separator[1];
                separator[1] = L'\0';
                attributes = GetFileAttributesW(resolved_wide);
                separator[1] = saved;
            } else {
                saved = *separator;
                *separator = L'\0';
                attributes = GetFileAttributesW(resolved_wide);
                *separator = saved;
            }
            if (attributes == INVALID_FILE_ATTRIBUTES
                || (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0U) {
                free(resolved_wide);
                return SDB_E_IO;
            }
        }
    } else {
        handle = CreateFileW(
            wide_path,
            GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            NULL,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            NULL
        );
        free(wide_path);
        if (handle == INVALID_HANDLE_VALUE) {
            return SDB_E_IO;
        }
        if (!GetFileInformationByHandle(handle, &information)
            || information.nNumberOfLinks != 1U) {
            (void)CloseHandle(handle);
            return SDB_E_INVALID_ARGUMENT;
        }
        required = GetFinalPathNameByHandleW(
            handle, NULL, 0U, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS
        );
        if (required == 0U
            || !sdb_checked_add_size(
                (size_t)required, 1U, &character_count
            )
            || !sdb_checked_mul_size(
                character_count, sizeof(*resolved_wide), &allocation_size
            )) {
            (void)CloseHandle(handle);
            return SDB_E_IO;
        }
        resolved_wide = (wchar_t *)malloc(allocation_size);
        if (resolved_wide == NULL) {
            (void)CloseHandle(handle);
            return SDB_E_OUT_OF_MEMORY;
        }
        written = GetFinalPathNameByHandleW(
            handle,
            resolved_wide,
            required + 1U,
            FILE_NAME_NORMALIZED | VOLUME_NAME_DOS
        );
        (void)CloseHandle(handle);
        if (written == 0U || written > required) {
            free(resolved_wide);
            return SDB_E_IO;
        }
    }
    status = sdb_windows_path_to_utf8(
        resolved_wide, resolved_path_out
    );
    free(resolved_wide);
    return status;
}

sdb_status sdb_file_path_exists(const char *path, bool *exists_out)
{
    wchar_t *wide_path = NULL;
    DWORD attributes;
    DWORD error;
    sdb_status status;
    if (path == NULL || path[0] == '\0' || exists_out == NULL) {
        return SDB_E_INVALID_ARGUMENT;
    }
    *exists_out = false;
    status = sdb_windows_path_from_utf8(path, &wide_path);
    if (status != SDB_OK) {
        return status;
    }
    attributes = GetFileAttributesW(wide_path);
    if (attributes != INVALID_FILE_ATTRIBUTES) {
        free(wide_path);
        *exists_out = true;
        return SDB_OK;
    }
    /*
     * Snapshot last-error BEFORE free(): HeapFree may clobber it and turn a
     * genuinely-absent file into a spurious SDB_E_IO, which would abort a
     * backup/replace to a new destination. Matches sdb_file_remove.
     */
    error = GetLastError();
    free(wide_path);
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND
        ? SDB_OK : SDB_E_IO;
}

#else

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

_Static_assert(
    sizeof(off_t) >= 8,
    "shibadb-c requires 64-bit off_t (set _FILE_OFFSET_BITS=64)"
);

static sdb_status sdb_file_open_posix(
    const char *path, int flags, mode_t mode, sdb_file *file_out
)
{
    int descriptor;
    if (path == NULL || file_out == NULL) {
        return SDB_E_INVALID_ARGUMENT;
    }
    do {
        descriptor = open(path, flags, mode);
    } while (descriptor < 0 && errno == EINTR);
    if (descriptor < 0) {
        /*
         * Map the common, actionable errnos to distinct status codes instead
         * of a blanket SDB_E_IO. EEXIST comes from the O_EXCL in
         * sdb_file_create_new (a create onto an existing path, e.g. running an
         * app a second time, or backup(replace=false) onto an existing file):
         * reporting that as "I/O error" wrongly implies disk/hardware failure,
         * so surface it as SDB_E_CONFLICT ("already exists"). ENOENT stays
         * SDB_E_NOT_FOUND; everything else remains SDB_E_IO.
         */
        if (errno == ENOENT) {
            return SDB_E_NOT_FOUND;
        }
        if (errno == EEXIST) {
            return SDB_E_CONFLICT;
        }
        if (errno == ENOSPC) {
            return SDB_E_NO_SPACE;
        }
        if (errno == EACCES || errno == EROFS) {
            return SDB_E_ACCESS_DENIED;
        }
        return SDB_E_IO;
    }
    file_out->descriptor = descriptor;
    sdb_file_initialize_test_state(file_out);
    return SDB_OK;
}

sdb_status sdb_file_create_new(const char *path, sdb_file *file_out)
{
    return sdb_file_open_posix(
        path, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, (mode_t)0600, file_out
    );
}

sdb_status sdb_file_open_existing(const char *path, bool writable, sdb_file *file_out)
{
    return sdb_file_open_posix(
        path, (writable ? O_RDWR : O_RDONLY) | O_CLOEXEC, (mode_t)0, file_out
    );
}

sdb_status sdb_file_close(sdb_file *file)
{
    int result;
    if (file == NULL || file->descriptor < 0) {
        return SDB_E_INVALID_ARGUMENT;
    }

    result = close(file->descriptor);
    file->descriptor = -1;
    return result == 0 ? SDB_OK : SDB_E_IO;
}

static size_t sdb_posix_chunk(size_t remaining, size_t limit)
{
    size_t chunk = remaining < limit ? remaining : limit;
#ifdef SSIZE_MAX
    if (chunk > (size_t)SSIZE_MAX) {
        chunk = (size_t)SSIZE_MAX;
    }
#endif
    return chunk;
}

sdb_status sdb_file_read_full(
    sdb_file *file, uint64_t offset, uint8_t *output, size_t size
)
{
    size_t completed = 0U;
    if (file == NULL || (output == NULL && size != 0U) || offset > (uint64_t)INT64_MAX) {
        return SDB_E_INVALID_ARGUMENT;
    }
    while (completed < size) {
        ssize_t transferred;
        const size_t chunk = sdb_posix_chunk(size - completed, SDB_FILE_IO_LIMIT(file));
        uint64_t position;
        if ((uint64_t)completed > UINT64_MAX - offset) {
            return SDB_E_OVERFLOW;
        }
        position = offset + (uint64_t)completed;
        if (position > (uint64_t)INT64_MAX) {
            return SDB_E_OVERFLOW;
        }
        if (sdb_file_should_fail_for_testing(file)) {
            return SDB_E_IO;
        }
        do {
            transferred = pread(
                file->descriptor, output + completed, chunk, (off_t)position
            );
        } while (transferred < 0 && errno == EINTR);
        if (transferred < 0) {
            return SDB_E_IO;
        }
        if (transferred == 0) {
            return SDB_E_TRUNCATED;
        }
        completed += (size_t)transferred;
    }
    return SDB_OK;
}

sdb_status sdb_file_write_full(
    sdb_file *file, uint64_t offset, const uint8_t *input, size_t size
)
{
    size_t completed = 0U;
    if (file == NULL || (input == NULL && size != 0U) || offset > (uint64_t)INT64_MAX) {
        return SDB_E_INVALID_ARGUMENT;
    }
    while (completed < size) {
        ssize_t transferred;
        const size_t chunk = sdb_posix_chunk(size - completed, SDB_FILE_IO_LIMIT(file));
        uint64_t position;
        if ((uint64_t)completed > UINT64_MAX - offset) {
            return SDB_E_OVERFLOW;
        }
        position = offset + (uint64_t)completed;
        if (position > (uint64_t)INT64_MAX) {
            return SDB_E_OVERFLOW;
        }
        if (sdb_file_should_fail_for_testing(file)) {
            return SDB_E_IO;
        }
        do {
            transferred = pwrite(
                file->descriptor, input + completed, chunk, (off_t)position
            );
        } while (transferred < 0 && errno == EINTR);
        if (transferred < 0) {
            if (errno == ENOSPC) {
                return SDB_E_NO_SPACE;
            }
            if (errno == EROFS || errno == EACCES) {
                return SDB_E_ACCESS_DENIED;
            }
            return SDB_E_IO;
        }
        if (transferred == 0) {
            return SDB_E_IO;
        }
        completed += (size_t)transferred;
    }
    return SDB_OK;
}

sdb_status sdb_file_sync(sdb_file *file)
{
    int result;
    if (file == NULL) {
        return SDB_E_INVALID_ARGUMENT;
    }
#if SDB_TESTING
    ++file->test_sync_count;
#endif
    if (sdb_file_should_fail_for_testing(file)) {
        return SDB_E_IO;
    }
#if defined(__APPLE__)

    do {
        result = fcntl(file->descriptor, F_FULLFSYNC);
    } while (result < 0 && errno == EINTR);
    if (result < 0 && (errno == EINVAL || errno == ENOTSUP)) {
        do {
            result = fsync(file->descriptor);
        } while (result < 0 && errno == EINTR);
    }
#else
    do {
        result = fsync(file->descriptor);
    } while (result < 0 && errno == EINTR);
#endif
    return result == 0 ? SDB_OK : SDB_E_IO;
}

sdb_status sdb_file_resize(sdb_file *file, uint64_t size)
{
    int result;
    if (file == NULL || size > (uint64_t)INT64_MAX) {
        return SDB_E_INVALID_ARGUMENT;
    }
    if (sdb_file_should_fail_for_testing(file)) {
        return SDB_E_IO;
    }
    do {
        result = ftruncate(file->descriptor, (off_t)size);
    } while (result < 0 && errno == EINTR);
    return result == 0 ? SDB_OK : SDB_E_IO;
}

sdb_status sdb_file_size(sdb_file *file, uint64_t *size_out)
{
    struct stat information;
    if (file == NULL || size_out == NULL) {
        return SDB_E_INVALID_ARGUMENT;
    }
    if (sdb_file_should_fail_for_testing(file)) {
        return SDB_E_IO;
    }
    if (fstat(file->descriptor, &information) != 0 || information.st_size < 0) {
        return SDB_E_IO;
    }
    *size_out = (uint64_t)information.st_size;
    return SDB_OK;
}

sdb_status sdb_file_sync_parent_directory(const char *path)
{
    char *copy;
    char *separator;
    const char *directory;
    int descriptor;
    int result;
    size_t length;

    if (path == NULL || path[0] == '\0') {
        return SDB_E_INVALID_ARGUMENT;
    }
    length = strlen(path);

    if (length > 1U && path[length - 1U] == '/') {
        return SDB_E_INVALID_ARGUMENT;
    }
    copy = (char *)malloc(length + 1U);
    if (copy == NULL) {
        return SDB_E_OUT_OF_MEMORY;
    }
    (void)memcpy(copy, path, length + 1U);
    separator = strrchr(copy, '/');
    if (separator == NULL) {
        directory = ".";
    } else if (separator == copy) {
        separator[1] = '\0';
        directory = copy;
    } else {
        *separator = '\0';
        directory = copy;
    }

    do {
        descriptor = open(directory, O_RDONLY | O_CLOEXEC);
    } while (descriptor < 0 && errno == EINTR);
    if (descriptor < 0) {
        free(copy);
        return SDB_E_IO;
    }
#if defined(__APPLE__)
    do {
        result = fcntl(descriptor, F_FULLFSYNC);
    } while (result < 0 && errno == EINTR);
    if (result < 0 && (errno == EINVAL || errno == ENOTSUP)) {
        do {
            result = fsync(descriptor);
        } while (result < 0 && errno == EINTR);
    }
#else
    do {
        result = fsync(descriptor);
    } while (result < 0 && errno == EINTR);
#endif

    if (close(descriptor) != 0 && result == 0) {
        result = -1;
    }
    free(copy);
    return result == 0 ? SDB_OK : SDB_E_IO;
}

sdb_status sdb_file_replace(
    const char *source, const char *destination, bool replace_existing
)
{
    int result;
    if (source == NULL || destination == NULL) {
        return SDB_E_INVALID_ARGUMENT;
    }
    if (replace_existing) {
        do {
            result = rename(source, destination);
        } while (result != 0 && errno == EINTR);
        return result == 0 ? SDB_OK : SDB_E_IO;
    }
    do {
        result = link(source, destination);
    } while (result != 0 && errno == EINTR);
    if (result != 0) {
        return SDB_E_IO;
    }
    do {
        result = unlink(source);
    } while (result != 0 && errno == EINTR);
    if (result != 0) {
        int rollback;
        do {
            rollback = unlink(destination);
        } while (rollback != 0 && errno == EINTR);
        return SDB_E_IO;
    }
    return SDB_OK;
}

sdb_status sdb_file_remove(const char *path, bool missing_ok)
{
    int result;
    if (path == NULL) {
        return SDB_E_INVALID_ARGUMENT;
    }
    do {
        result = unlink(path);
    } while (result != 0 && errno == EINTR);
    return result == 0 || (missing_ok && errno == ENOENT)
        ? SDB_OK : SDB_E_IO;
}

sdb_status sdb_file_resolve_database_path(
    const char *path, bool must_exist, char **resolved_path_out
)
{
    char *resolved;
    struct stat information;
    if (path == NULL || path[0] == '\0' || resolved_path_out == NULL) {
        return SDB_E_INVALID_ARGUMENT;
    }
    *resolved_path_out = NULL;
    if (must_exist) {
        resolved = realpath(path, NULL);
        if (resolved == NULL) {
            return errno == ENOENT || errno == ENOTDIR
                ? SDB_E_NOT_FOUND : SDB_E_IO;
        }
        if (stat(resolved, &information) != 0
            || !S_ISREG(information.st_mode)
            || information.st_nlink != (nlink_t)1) {
            free(resolved);
            return SDB_E_INVALID_ARGUMENT;
        }
    } else {
        char *copy;
        char *separator;
        char *directory;
        const char *name;
        char *resolved_directory;
        size_t directory_size;
        size_t name_size;
        size_t total_size;
        const size_t path_size = strlen(path);
        if (path_size == 0U || path[path_size - 1U] == '/') {
            return SDB_E_INVALID_ARGUMENT;
        }
        copy = (char *)malloc(path_size + 1U);
        if (copy == NULL) {
            return SDB_E_OUT_OF_MEMORY;
        }
        (void)memcpy(copy, path, path_size + 1U);
        separator = strrchr(copy, '/');
        if (separator == NULL) {
            directory = (char *)".";
            name = copy;
        } else {
            name = separator + 1;
            if (separator == copy) {
                separator[1] = '\0';
                directory = copy;
            } else {
                *separator = '\0';
                directory = copy;
            }
        }
        if (name[0] == '\0'
            || strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
            free(copy);
            return SDB_E_INVALID_ARGUMENT;
        }
        resolved_directory = realpath(directory, NULL);
        if (resolved_directory == NULL) {
            free(copy);
            return SDB_E_IO;
        }
        directory_size = strlen(resolved_directory);
        name_size = strlen(name);
        if (!sdb_checked_add_size(directory_size, name_size, &total_size)
            || !sdb_checked_add_size(total_size, 2U, &total_size)) {
            free(resolved_directory);
            free(copy);
            return SDB_E_OVERFLOW;
        }
        resolved = (char *)malloc(total_size);
        if (resolved == NULL) {
            free(resolved_directory);
            free(copy);
            return SDB_E_OUT_OF_MEMORY;
        }
        (void)memcpy(resolved, resolved_directory, directory_size);
        if (directory_size != 1U || resolved_directory[0] != '/') {
            resolved[directory_size] = '/';
            ++directory_size;
        }
        (void)memcpy(resolved + directory_size, name, name_size + 1U);
        free(resolved_directory);
        free(copy);
    }
    *resolved_path_out = resolved;
    return SDB_OK;
}

sdb_status sdb_file_path_exists(const char *path, bool *exists_out)
{
    struct stat information;
    if (path == NULL || path[0] == '\0' || exists_out == NULL) {
        return SDB_E_INVALID_ARGUMENT;
    }
    *exists_out = false;
    if (stat(path, &information) == 0) {
        *exists_out = true;
        return SDB_OK;
    }
    return errno == ENOENT || errno == ENOTDIR ? SDB_OK : SDB_E_IO;
}

#endif

#if SDB_TESTING
void sdb_file_set_io_limit_for_testing(sdb_file *file, size_t limit)
{
    if (file != NULL) {
        file->io_limit = limit == 0U ? 1U : limit;
    }
}

void sdb_file_fail_after_for_testing(sdb_file *file, size_t successful_operations)
{
    if (file != NULL) {
        file->test_operation_count = 0U;
        file->test_fail_after = successful_operations;
    }
}

void sdb_file_clear_failure_for_testing(sdb_file *file)
{
    if (file != NULL) {
        file->test_operation_count = 0U;
        file->test_fail_after = SIZE_MAX;
    }
}

size_t sdb_file_sync_count_for_testing(const sdb_file *file)
{
    return file != NULL ? file->test_sync_count : 0U;
}
#endif
