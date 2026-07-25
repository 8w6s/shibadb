#include "crypto.h"

#ifdef _WIN32
#include <bcrypt.h>

sdb_status sdb_random_bytes(uint8_t *output, size_t size)
{
    if (output == NULL || size == 0U || size > (size_t)ULONG_MAX) {
        return SDB_E_INVALID_ARGUMENT;
    }
    return BCryptGenRandom(
        NULL,
        output,
        (ULONG)size,
        BCRYPT_USE_SYSTEM_PREFERRED_RNG
    ) == 0 ? SDB_OK : SDB_E_IO;
}
#else
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(__linux__)
#include <sys/random.h>

/*
 * getrandom(2) is not filesystem-dependent (works under chroot/seccomp) and
 * blocks briefly on early boot until the pool is initialized instead of
 * silently returning low-entropy bytes as /dev/urandom can. Fall back to
 * /dev/urandom only if the syscall is unavailable (kernel < 3.17 or blocked
 * by a restrictive seccomp filter).
 */
static sdb_status sdb_random_bytes_getrandom(uint8_t *output, size_t size)
{
    size_t remaining = size;
    uint8_t *cursor = output;
    while (remaining != 0U) {
        ssize_t transferred;
        do {
            transferred = getrandom(cursor, remaining, 0);
        } while (transferred < 0 && errno == EINTR);
        if (transferred < 0) {
            /*
             * Fall back to /dev/urandom whenever the syscall is unavailable —
             * ENOSYS on old kernels, and EPERM/EACCES under seccomp filters
             * that use SECCOMP_RET_ERRNO to deny it.
             */
            if (errno == ENOSYS || errno == EPERM || errno == EACCES) {
                return SDB_E_INTERNAL;
            }
            return SDB_E_IO;
        }
        cursor += (size_t)transferred;
        remaining -= (size_t)transferred;
    }
    return SDB_OK;
}
#endif

static sdb_status sdb_random_bytes_urandom(uint8_t *output, size_t size)
{
    /*
     * Hardened /dev/urandom open. Bypass sdb_file_open_existing so we
     * can enforce two guarantees the generic helper does not:
     *
     *   1. O_NOFOLLOW: refuse the open if /dev/urandom is a symlink.
     *      Prevents a rogue /dev (chroot, unprivileged user-namespace,
     *      hostile container) from redirecting reads to a predictable
     *      or attacker-controlled file.
     *   2. fstat + S_ISCHR: verify the opened fd is a character device.
     *      Even without O_NOFOLLOW-defeating tricks, a rogue /dev can
     *      hand us a regular file, FIFO, or socket — all of which
     *      readily "read" but with no entropy guarantees. On mainstream
     *      Linux, BSD, and macOS, /dev/urandom is always character-device.
     *
     * A failure here is fatal: we return SDB_E_IO and refuse to hand
     * back potentially predictable bytes. Callers reach this fallback
     * only when getrandom(2) is unavailable, so failing loud is safer
     * than silently returning weak randomness.
     */
    int descriptor;
    ssize_t transferred_signed;
    size_t remaining = size;
    uint8_t *cursor = output;
    struct stat st;

    do {
        descriptor = open(
            "/dev/urandom",
            O_RDONLY | O_NOFOLLOW | O_CLOEXEC
        );
    } while (descriptor < 0 && errno == EINTR);
    if (descriptor < 0) {
        return SDB_E_IO;
    }
    if (fstat(descriptor, &st) != 0 || !S_ISCHR(st.st_mode)) {
        (void)close(descriptor);
        return SDB_E_IO;
    }

    while (remaining != 0U) {
        do {
            transferred_signed = read(descriptor, cursor, remaining);
        } while (transferred_signed < 0 && errno == EINTR);
        if (transferred_signed <= 0) {
            (void)close(descriptor);
            return SDB_E_IO;
        }
        cursor += (size_t)transferred_signed;
        remaining -= (size_t)transferred_signed;
    }
    (void)close(descriptor);
    return SDB_OK;
}

sdb_status sdb_random_bytes(uint8_t *output, size_t size)
{
    if (output == NULL || size == 0U) {
        return SDB_E_INVALID_ARGUMENT;
    }
#if defined(__linux__)
    {
        const sdb_status status = sdb_random_bytes_getrandom(output, size);
        if (status != SDB_E_INTERNAL) {
            return status;
        }
    }
#endif
    return sdb_random_bytes_urandom(output, size);
}
#endif
