#include "sync.h"

#include "internal.h"
#include "windows_path.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Abort on lock primitive failure. These calls can only fail on programmer
 * bugs (uninitialized mutex, destroyed mutex, recursive lock past PTHREAD_KEEP_
 * limit) — never on transient runtime errors. Continuing after such a failure
 * would silently violate mutual exclusion invariants relied upon by the
 * engine's WAL and transaction paths, so terminate loudly instead.
 */
static void sdb_sync_abort(const char *what, int code)
{
    (void)fprintf(stderr, "shibadb: %s failed with code %d\n", what, code);
    abort();
}

static sdb_status sdb_lock_path(const char *database_path, char **path_out)
{
    static const char suffix[] = ".lock";
    size_t path_size;
    size_t total_size;
    char *path;
    if (database_path == NULL || database_path[0] == '\0'
        || path_out == NULL) {
        return SDB_E_INVALID_ARGUMENT;
    }
    path_size = strlen(database_path);
    if (!sdb_checked_add_size(path_size, sizeof(suffix), &total_size)) {
        return SDB_E_OVERFLOW;
    }
    path = (char *)malloc(total_size);
    if (path == NULL) {
        return SDB_E_INTERNAL;
    }
    (void)memcpy(path, database_path, path_size);
    (void)memcpy(path + path_size, suffix, sizeof(suffix));
    *path_out = path;
    return SDB_OK;
}

#ifdef _WIN32

sdb_status sdb_mutex_init(sdb_mutex *mutex)
{
    if (mutex == NULL) {
        return SDB_E_INVALID_ARGUMENT;
    }
    InitializeCriticalSection(&mutex->native);
    mutex->initialized = true;
    return SDB_OK;
}

void sdb_mutex_lock(sdb_mutex *mutex)
{
    EnterCriticalSection(&mutex->native);
}

void sdb_mutex_unlock(sdb_mutex *mutex)
{
    LeaveCriticalSection(&mutex->native);
}

void sdb_mutex_destroy(sdb_mutex *mutex)
{
    if (mutex != NULL && mutex->initialized) {
        DeleteCriticalSection(&mutex->native);
        mutex->initialized = false;
    }
}

sdb_status sdb_process_lock_acquire(
    const char *database_path, sdb_process_lock *lock_out
)
{
    OVERLAPPED operation = {0};
    char *path;
    wchar_t *wide_path = NULL;
    sdb_status status;
    if (lock_out == NULL) {
        return SDB_E_INVALID_ARGUMENT;
    }
    lock_out->handle = INVALID_HANDLE_VALUE;
    lock_out->held = false;
    status = sdb_lock_path(database_path, &path);
    if (status != SDB_OK) {
        return status;
    }
    status = sdb_windows_path_from_utf8(path, &wide_path);
    free(path);
    if (status != SDB_OK) {
        return status;
    }
    lock_out->handle = CreateFileW(
        wide_path,
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL,
        OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        NULL
    );
    free(wide_path);
    if (lock_out->handle == INVALID_HANDLE_VALUE) {
        return SDB_E_IO;
    }
    if (!LockFileEx(
            lock_out->handle,
            LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY,
            0U,
            UINT32_MAX,
            UINT32_MAX,
            &operation
        )) {
        const DWORD error = GetLastError();
        (void)CloseHandle(lock_out->handle);
        lock_out->handle = INVALID_HANDLE_VALUE;
        return error == ERROR_LOCK_VIOLATION
            ? SDB_E_BUSY : SDB_E_IO;
    }
    lock_out->held = true;
    return SDB_OK;
}

sdb_status sdb_process_lock_acquire_database(
    const char *database_path, sdb_process_lock *lock_out
)
{
    OVERLAPPED operation = {0};
    wchar_t *wide_path = NULL;
    sdb_status status;
    if (database_path == NULL || database_path[0] == '\0'
        || lock_out == NULL) {
        return SDB_E_INVALID_ARGUMENT;
    }
    lock_out->handle = INVALID_HANDLE_VALUE;
    lock_out->held = false;
    status = sdb_windows_path_from_utf8(database_path, &wide_path);
    if (status != SDB_OK) {
        return status;
    }
    lock_out->handle = CreateFileW(
        wide_path,
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        NULL
    );
    free(wide_path);
    if (lock_out->handle == INVALID_HANDLE_VALUE) {
        return SDB_E_IO;
    }
    if (!LockFileEx(
            lock_out->handle,
            LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY,
            0U,
            UINT32_MAX,
            UINT32_MAX,
            &operation
        )) {
        const DWORD error = GetLastError();
        (void)CloseHandle(lock_out->handle);
        lock_out->handle = INVALID_HANDLE_VALUE;
        return error == ERROR_LOCK_VIOLATION
            ? SDB_E_BUSY : SDB_E_IO;
    }
    lock_out->held = true;
    return SDB_OK;
}

sdb_status sdb_process_lock_release(sdb_process_lock *lock)
{
    OVERLAPPED operation = {0};
    bool ok = true;
    if (lock == NULL || !lock->held
        || lock->handle == INVALID_HANDLE_VALUE) {
        return SDB_E_INVALID_ARGUMENT;
    }
    if (!UnlockFileEx(
            lock->handle, 0U, UINT32_MAX, UINT32_MAX, &operation
        )) {
        ok = false;
    }
    if (!CloseHandle(lock->handle)) {
        ok = false;
    }
    lock->handle = INVALID_HANDLE_VALUE;
    lock->held = false;
    return ok ? SDB_OK : SDB_E_IO;
}

#else

#include <errno.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

sdb_status sdb_mutex_init(sdb_mutex *mutex)
{
    pthread_mutexattr_t attributes;
    int result;
    if (mutex == NULL) {
        return SDB_E_INVALID_ARGUMENT;
    }
    mutex->initialized = false;
    result = pthread_mutexattr_init(&attributes);
    if (result != 0) {
        return SDB_E_INTERNAL;
    }
    result = pthread_mutexattr_settype(&attributes, PTHREAD_MUTEX_RECURSIVE);
    if (result == 0) {
        result = pthread_mutex_init(&mutex->native, &attributes);
    }
    (void)pthread_mutexattr_destroy(&attributes);
    if (result != 0) {
        return SDB_E_INTERNAL;
    }
    mutex->initialized = true;
    return SDB_OK;
}

void sdb_mutex_lock(sdb_mutex *mutex)
{
    const int result = pthread_mutex_lock(&mutex->native);
    if (result != 0) {
        sdb_sync_abort("pthread_mutex_lock", result);
    }
}

void sdb_mutex_unlock(sdb_mutex *mutex)
{
    const int result = pthread_mutex_unlock(&mutex->native);
    if (result != 0) {
        sdb_sync_abort("pthread_mutex_unlock", result);
    }
}

void sdb_mutex_destroy(sdb_mutex *mutex)
{
    if (mutex != NULL && mutex->initialized) {
        const int result = pthread_mutex_destroy(&mutex->native);
        if (result != 0) {
            sdb_sync_abort("pthread_mutex_destroy", result);
        }
        mutex->initialized = false;
    }
}

sdb_status sdb_process_lock_acquire(
    const char *database_path, sdb_process_lock *lock_out
)
{
    char *path;
    int descriptor;
    sdb_status status;
    if (lock_out == NULL) {
        return SDB_E_INVALID_ARGUMENT;
    }
    lock_out->descriptor = -1;
    lock_out->held = false;
    status = sdb_lock_path(database_path, &path);
    if (status != SDB_OK) {
        return status;
    }
    do {
        /*
         * O_NOFOLLOW rejects a symlink planted at the sidecar path — an
         * attacker with directory write access could otherwise redirect
         * our flock onto another file.
         */
        descriptor = open(
            path, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, (mode_t)0600
        );
    } while (descriptor < 0 && errno == EINTR);
    free(path);
    if (descriptor < 0) {
        return errno == ELOOP ? SDB_E_INVALID_ARGUMENT : SDB_E_IO;
    }
    /*
     * O_NOFOLLOW blocks a symlink swap, but a hostile directory owner
     * can still pre-plant a FIFO, socket, or hardlink at the sidecar
     * path — all of which open cleanly. Verify the fd we got points
     * at a plain regular file with a single directory entry:
     *
     * - S_ISREG rejects FIFOs, sockets, character/block devices;
     *   flock semantics on those are non-portable at best.
     * - st_nlink == 1 rejects a hardlink alias to a file elsewhere
     *   in the filesystem; without this an attacker can hardlink
     *   /etc/passwd (or any file they lack write access to) into
     *   the sidecar path and observe the acquisition through
     *   inotify or by tailing the file (empty writes still update
     *   ctime, and open/close are visible in fanotify).
     *
     * A failing check does not distinguish "attacker" from "user
     * configured something weird" — either way, refuse to lock and
     * let the caller surface a clear error.
     */
    {
        struct stat st;
        if (fstat(descriptor, &st) != 0
            || !S_ISREG(st.st_mode)
            || st.st_nlink != 1) {
            (void)close(descriptor);
            return SDB_E_INVALID_ARGUMENT;
        }
    }
    if (flock(descriptor, LOCK_EX | LOCK_NB) != 0) {
        const int error = errno;
        (void)close(descriptor);
        return error == EWOULDBLOCK || error == EAGAIN
            ? SDB_E_BUSY : SDB_E_IO;
    }
    lock_out->descriptor = descriptor;
    lock_out->held = true;
    return SDB_OK;
}

sdb_status sdb_process_lock_acquire_database(
    const char *database_path, sdb_process_lock *lock_out
)
{
    int descriptor;
    if (database_path == NULL || database_path[0] == '\0'
        || lock_out == NULL) {
        return SDB_E_INVALID_ARGUMENT;
    }
    lock_out->descriptor = -1;
    lock_out->held = false;
    do {
        descriptor = open(database_path, O_RDONLY | O_CLOEXEC);
    } while (descriptor < 0 && errno == EINTR);
    if (descriptor < 0) {
        return SDB_E_IO;
    }
    if (flock(descriptor, LOCK_EX | LOCK_NB) != 0) {
        const int error = errno;
        (void)close(descriptor);
        return error == EWOULDBLOCK || error == EAGAIN
            ? SDB_E_BUSY : SDB_E_IO;
    }
    lock_out->descriptor = descriptor;
    lock_out->held = true;
    return SDB_OK;
}

sdb_status sdb_process_lock_release(sdb_process_lock *lock)
{
    int unlock_result;
    int close_result;
    if (lock == NULL || !lock->held || lock->descriptor < 0) {
        return SDB_E_INVALID_ARGUMENT;
    }
    unlock_result = flock(lock->descriptor, LOCK_UN);
    close_result = close(lock->descriptor);
    lock->descriptor = -1;
    lock->held = false;
    return unlock_result == 0 && close_result == 0 ? SDB_OK : SDB_E_IO;
}

#endif
