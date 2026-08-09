#ifndef SHIBADB_BENCH_TIMER_H
#define SHIBADB_BENCH_TIMER_H

/*
 * Portable monotonic nanosecond clock for the benchmark harnesses.
 *
 * The engine-level benches (bench_kv/bench_churn/bench_concurrent) each carry
 * their own POSIX-only clock_gettime timer, which does not build on Windows.
 * This header centralizes a timer that works on both: QueryPerformanceCounter
 * on Windows, CLOCK_MONOTONIC elsewhere. The Windows path converts ticks to
 * nanoseconds with an integer whole/remainder split so long runs neither
 * overflow uint64 nor lose precision to double rounding.
 */

#include <stdint.h>

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

static uint64_t bench_now_ns(void)
{
    LARGE_INTEGER frequency;
    LARGE_INTEGER counter;
    uint64_t ticks;
    uint64_t hz;
    uint64_t whole;
    uint64_t remainder;
    (void)QueryPerformanceFrequency(&frequency);
    (void)QueryPerformanceCounter(&counter);
    ticks = (uint64_t)counter.QuadPart;
    hz = (uint64_t)frequency.QuadPart;
    if (hz == 0U) {
        return 0U;
    }
    whole = ticks / hz;
    remainder = ticks % hz;
    return whole * UINT64_C(1000000000)
        + remainder * UINT64_C(1000000000) / hz;
}

#else

#include <time.h>

static uint64_t bench_now_ns(void)
{
    struct timespec ts;
    (void)clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * UINT64_C(1000000000)
        + (uint64_t)ts.tv_nsec;
}

#endif

#endif
