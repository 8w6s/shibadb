#ifndef SHIBADB_SYSINFO_H
#define SHIBADB_SYSINFO_H

#include <stdint.h>

/* Total physical RAM in bytes, or 0 if it cannot be determined. Used only to
 * size the page cache when the caller requests SDB_CACHE_AUTO; a 0 return
 * falls back to the conservative built-in default. */
uint64_t sdb_physical_memory_bytes(void);

#endif
