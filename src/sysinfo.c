#include "sysinfo.h"

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

uint64_t sdb_physical_memory_bytes(void)
{
    MEMORYSTATUSEX status;
    status.dwLength = sizeof(status);
    if (GlobalMemoryStatusEx(&status) == 0) {
        return 0U;
    }
    return (uint64_t)status.ullTotalPhys;
}

#else

#include <unistd.h>

uint64_t sdb_physical_memory_bytes(void)
{
#if defined(_SC_PHYS_PAGES) && defined(_SC_PAGESIZE)
    const long pages = sysconf(_SC_PHYS_PAGES);
    const long page_size = sysconf(_SC_PAGESIZE);
    if (pages <= 0L || page_size <= 0L) {
        return 0U;
    }
    return (uint64_t)pages * (uint64_t)page_size;
#else
    return 0U;
#endif
}

#endif
