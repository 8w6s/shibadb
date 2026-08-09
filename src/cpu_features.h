#ifndef SHIBADB_CPU_FEATURES_H
#define SHIBADB_CPU_FEATURES_H

#include <stdbool.h>

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) \
    || defined(_M_IX86)
#define SDB_CPU_X86 1
#else
#define SDB_CPU_X86 0
#endif

/*
 * Runtime CPU feature detection for selecting hardware-accelerated crypto and
 * checksum kernels. Detection is done once (thread-safe) on first query and
 * cached; the returned struct is immutable for the process lifetime. On
 * non-x86 targets every field is false, so callers fall back to the portable
 * scalar path. AVX2 is reported true only when the OS is also confirmed to
 * preserve YMM state (OSXSAVE + XGETBV), so a kernel guarded on it is safe to
 * enter.
 */
typedef struct sdb_cpu_features {
    bool sse2;
    bool sse41;
    bool avx2;
    bool sha;        /* SHA-NI (SHA1/SHA256 message + rounds instructions) */
    bool pclmulqdq;  /* carry-less multiply, for CRC folding */
} sdb_cpu_features;

const sdb_cpu_features *sdb_cpu_features_get(void);

#endif
