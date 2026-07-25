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
typedef struct sdb_process_lock {
    HANDLE handle;
    bool held;
} sdb_process_lock;
#else
#include <pthread.h>
typedef struct sdb_mutex {
    pthread_mutex_t native;
    bool initialized;
} sdb_mutex;
typedef struct sdb_process_lock {
    int descriptor;
    bool held;
} sdb_process_lock;
#endif

sdb_status sdb_mutex_init(sdb_mutex *mutex);
void sdb_mutex_lock(sdb_mutex *mutex);
void sdb_mutex_unlock(sdb_mutex *mutex);
void sdb_mutex_destroy(sdb_mutex *mutex);

sdb_status sdb_process_lock_acquire(
    const char *database_path, sdb_process_lock *lock_out
);
sdb_status sdb_process_lock_acquire_database(
    const char *database_path, sdb_process_lock *lock_out
);
sdb_status sdb_process_lock_release(sdb_process_lock *lock);

#endif
