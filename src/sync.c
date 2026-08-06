#include "sync.h"

#include "internal.h"
#include "windows_path.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
        return SDB_E_OUT_OF_MEMORY;
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

sdb_status sdb_cond_init(sdb_cond *cond)
{
    if (cond == NULL) {
        return SDB_E_INVALID_ARGUMENT;
    }
    InitializeConditionVariable(&cond->native);
    cond->initialized = true;
    return SDB_OK;
}

void sdb_cond_wait(sdb_cond *cond, sdb_mutex *mutex)
{
    if (!SleepConditionVariableCS(&cond->native, &mutex->native, INFINITE)) {
        sdb_sync_abort("SleepConditionVariableCS", (int)GetLastError());
    }
}

void sdb_cond_signal(sdb_cond *cond)
{
    WakeConditionVariable(&cond->native);
}

void sdb_cond_broadcast(sdb_cond *cond)
{
    WakeAllConditionVariable(&cond->native);
}

void sdb_cond_destroy(sdb_cond *cond)
{
    if (cond != NULL) {
        cond->initialized = false;
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
    lock_out->named_semaphore = false;
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
    BY_HANDLE_FILE_INFORMATION information;
    HANDLE database_handle;
    HANDLE identity_semaphore;
    DWORD wait_status;
    wchar_t semaphore_name[96];
    wchar_t *wide_path = NULL;
    sdb_status status;
    if (database_path == NULL || database_path[0] == '\0'
        || lock_out == NULL) {
        return SDB_E_INVALID_ARGUMENT;
    }
    lock_out->handle = INVALID_HANDLE_VALUE;
    lock_out->held = false;
    lock_out->named_semaphore = false;
    status = sdb_windows_path_from_utf8(database_path, &wide_path);
    if (status != SDB_OK) {
        return status;
    }
    database_handle = CreateFileW(
        wide_path,
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        NULL
    );
    free(wide_path);
    if (database_handle == INVALID_HANDLE_VALUE) {
        return SDB_E_IO;
    }
    if (!GetFileInformationByHandle(database_handle, &information)) {
        (void)CloseHandle(database_handle);
        return SDB_E_IO;
    }
    (void)CloseHandle(database_handle);
    if (swprintf(
            semaphore_name,
            sizeof(semaphore_name) / sizeof(semaphore_name[0]),
            L"Global\\ShibaDB-%08lX-%08lX%08lX",
            (unsigned long)information.dwVolumeSerialNumber,
            (unsigned long)information.nFileIndexHigh,
            (unsigned long)information.nFileIndexLow
        ) < 0) {
        return SDB_E_INTERNAL;
    }
    identity_semaphore = CreateSemaphoreW(
        NULL, 1L, 1L, semaphore_name
    );
    if (identity_semaphore == NULL) {
        return SDB_E_IO;
    }
    wait_status = WaitForSingleObject(identity_semaphore, 0U);
    if (wait_status != WAIT_OBJECT_0) {
        (void)CloseHandle(identity_semaphore);
        return wait_status == WAIT_TIMEOUT ? SDB_E_BUSY : SDB_E_IO;
    }
    lock_out->handle = identity_semaphore;
    lock_out->held = true;
    lock_out->named_semaphore = true;
    return SDB_OK;
}

sdb_status sdb_process_lock_release(sdb_process_lock *lock)
{
    bool ok = true;
    if (lock == NULL || !lock->held
        || lock->handle == INVALID_HANDLE_VALUE) {
        return SDB_E_INVALID_ARGUMENT;
    }
    if (lock->named_semaphore) {
        if (!ReleaseSemaphore(lock->handle, 1L, NULL)) {
            ok = false;
        }
    } else {
        OVERLAPPED operation = {0};
        if (!UnlockFileEx(
                lock->handle, 0U, UINT32_MAX, UINT32_MAX, &operation
            )) {
            ok = false;
        }
    }
    if (!CloseHandle(lock->handle)) {
        ok = false;
    }
    lock->handle = INVALID_HANDLE_VALUE;
    lock->held = false;
    lock->named_semaphore = false;
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

sdb_status sdb_cond_init(sdb_cond *cond)
{
    if (cond == NULL) {
        return SDB_E_INVALID_ARGUMENT;
    }
    cond->initialized = false;
    if (pthread_cond_init(&cond->native, NULL) != 0) {
        return SDB_E_INTERNAL;
    }
    cond->initialized = true;
    return SDB_OK;
}

void sdb_cond_wait(sdb_cond *cond, sdb_mutex *mutex)
{
    const int result = pthread_cond_wait(&cond->native, &mutex->native);
    if (result != 0) {
        sdb_sync_abort("pthread_cond_wait", result);
    }
}

void sdb_cond_signal(sdb_cond *cond)
{
    const int result = pthread_cond_signal(&cond->native);
    if (result != 0) {
        sdb_sync_abort("pthread_cond_signal", result);
    }
}

void sdb_cond_broadcast(sdb_cond *cond)
{
    const int result = pthread_cond_broadcast(&cond->native);
    if (result != 0) {
        sdb_sync_abort("pthread_cond_broadcast", result);
    }
}

void sdb_cond_destroy(sdb_cond *cond)
{
    if (cond != NULL && cond->initialized) {
        const int result = pthread_cond_destroy(&cond->native);
        if (result != 0) {
            sdb_sync_abort("pthread_cond_destroy", result);
        }
        cond->initialized = false;
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

        descriptor = open(
            path, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, (mode_t)0600
        );
    } while (descriptor < 0 && errno == EINTR);
    free(path);
    if (descriptor < 0) {
        return errno == ELOOP ? SDB_E_INVALID_ARGUMENT : SDB_E_IO;
    }

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
