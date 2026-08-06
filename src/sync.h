#ifndef SHIBADB_SYNC_H
#define SHIBADB_SYNC_H

#include "shibadb.h"

#include <stdbool.h>

#ifdef _WIN32
#include <windows.h>
typedef struct sdb_mutex {
    CRITICAL_SECTION native;
    bool initialized;
} sdb_mutex;
typedef struct sdb_cond {
    CONDITION_VARIABLE native;
    bool initialized;
} sdb_cond;
typedef struct sdb_process_lock {
    HANDLE handle;
    bool held;
    bool named_semaphore;
} sdb_process_lock;
#else
#include <pthread.h>
typedef struct sdb_mutex {
    pthread_mutex_t native;
    bool initialized;
} sdb_mutex;
typedef struct sdb_cond {
    pthread_cond_t native;
    bool initialized;
} sdb_cond;
typedef struct sdb_process_lock {
    int descriptor;
    bool held;
} sdb_process_lock;
#endif

sdb_status sdb_mutex_init(sdb_mutex *mutex);
void sdb_mutex_lock(sdb_mutex *mutex);
void sdb_mutex_unlock(sdb_mutex *mutex);
void sdb_mutex_destroy(sdb_mutex *mutex);

/*
 * Condition variable used by the R4 group-commit leader/follower protocol.
 * sdb_cond_wait must be called with `mutex` held (exactly once — the mutex is
 * recursive but the group-commit lock is only ever taken to depth 1, so the
 * single unlock inside pthread_cond_wait fully releases it). Spurious wakeups
 * are possible; callers must re-check their predicate in a loop.
 */
sdb_status sdb_cond_init(sdb_cond *cond);
void sdb_cond_wait(sdb_cond *cond, sdb_mutex *mutex);
void sdb_cond_signal(sdb_cond *cond);
void sdb_cond_broadcast(sdb_cond *cond);
void sdb_cond_destroy(sdb_cond *cond);

sdb_status sdb_process_lock_acquire(
    const char *database_path, sdb_process_lock *lock_out
);
sdb_status sdb_process_lock_acquire_database(
    const char *database_path, sdb_process_lock *lock_out
);
sdb_status sdb_process_lock_release(sdb_process_lock *lock);

#endif
