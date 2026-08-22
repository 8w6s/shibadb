#include "cpu_features.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#if SDB_CPU_X86
#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <cpuid.h>
#endif

/* Read the extended control register 0 (XCR0) to confirm the OS saves the
 * vector state. Only valid after CPUID reports OSXSAVE. */
static uint64_t sdb_xgetbv0(void)
{
#if defined(_MSC_VER)
    return (uint64_t)_xgetbv(0);
#else
    uint32_t eax;
    uint32_t edx;
    __asm__ volatile("xgetbv" : "=a"(eax), "=d"(edx) : "c"(0));
    return ((uint64_t)edx << 32) | (uint64_t)eax;
#endif
}
#endif /* SDB_CPU_X86 */

static sdb_cpu_features sdb_g_features;

static void sdb_cpu_detect(void)
{
    sdb_cpu_features features;
    (void)memset(&features, 0, sizeof(features));

#if SDB_CPU_X86
    {
        uint32_t max_leaf;
        uint32_t ecx1 = 0U;
        uint32_t edx1 = 0U;
        uint32_t ebx7 = 0U;
        bool osxsave;
        bool avx;
        bool os_saves_ymm = false;

#if defined(_MSC_VER)
        int regs[4];
        __cpuid(regs, 0);
        max_leaf = (uint32_t)regs[0];
        __cpuid(regs, 1);
        ecx1 = (uint32_t)regs[2];
        edx1 = (uint32_t)regs[3];
        if (max_leaf >= 7U) {
            __cpuidex(regs, 7, 0);
            ebx7 = (uint32_t)regs[1];
        }
#else
        uint32_t eax;
        uint32_t ebx;
        uint32_t ecx;
        uint32_t edx;
        max_leaf = (uint32_t)__get_cpuid_max(0, NULL);
        if (__get_cpuid(1U, &eax, &ebx, &ecx, &edx) != 0) {
            ecx1 = ecx;
            edx1 = edx;
        }
        if (max_leaf >= 7U
            && __get_cpuid_count(7U, 0U, &eax, &ebx, &ecx, &edx) != 0) {
            ebx7 = ebx;
        }
#endif

        osxsave = ((ecx1 >> 27) & 1U) != 0U;
        avx = ((ecx1 >> 28) & 1U) != 0U;
        if (osxsave) {
            const uint64_t xcr0 = sdb_xgetbv0();
            /* bit 1 = XMM (SSE) state, bit 2 = YMM (AVX) state */
            os_saves_ymm = (xcr0 & 0x6U) == 0x6U;
        }

        features.sse2 = ((edx1 >> 26) & 1U) != 0U;
        features.sse41 = ((ecx1 >> 19) & 1U) != 0U;
        features.pclmulqdq = ((ecx1 >> 1) & 1U) != 0U;
        features.sha = ((ebx7 >> 29) & 1U) != 0U;
        features.avx2 = avx && os_saves_ymm && (((ebx7 >> 5) & 1U) != 0U);
    }
#endif /* SDB_CPU_X86 */

    {
        /* Escape hatch: force the scalar path regardless of hardware. Lets the
         * differential tests exercise both backends on one machine and gives
         * operators a runtime kill switch if a SIMD kernel ever misbehaves.
         *
         * The MSVC CRT deprecates getenv in favour of _dupenv_s, which returns
         * an owned copy. Suppressing the deprecation would mean defining
         * _CRT_SECURE_NO_WARNINGS across the whole library, so the Windows path
         * uses the CRT's own spelling instead and frees what it is handed. */
        bool disabled = false;
#if defined(_MSC_VER) || (defined(_WIN32) && defined(__clang__))
        char *disable = NULL;
        size_t disable_size = 0;
        if (_dupenv_s(&disable, &disable_size, "SDB_NO_SIMD") == 0
            && disable != NULL) {
            disabled = disable[0] != '\0' && strcmp(disable, "0") != 0;
            free(disable);
        }
#else
        const char *disable = getenv("SDB_NO_SIMD");
        disabled = disable != NULL && disable[0] != '\0'
            && strcmp(disable, "0") != 0;
#endif
        if (disabled) {
            (void)memset(&features, 0, sizeof(features));
        }
    }

    sdb_g_features = features;
}

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

static INIT_ONCE sdb_g_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK sdb_cpu_detect_cb(
    PINIT_ONCE once, PVOID parameter, PVOID *context
)
{
    (void)once;
    (void)parameter;
    (void)context;
    sdb_cpu_detect();
    return TRUE;
}

const sdb_cpu_features *sdb_cpu_features_get(void)
{
    (void)InitOnceExecuteOnce(&sdb_g_once, sdb_cpu_detect_cb, NULL, NULL);
    return &sdb_g_features;
}

#else

#include <pthread.h>

static pthread_once_t sdb_g_once = PTHREAD_ONCE_INIT;

const sdb_cpu_features *sdb_cpu_features_get(void)
{
    (void)pthread_once(&sdb_g_once, sdb_cpu_detect);
    return &sdb_g_features;
}

#endif
