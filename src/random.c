#include "crypto.h"

#ifdef _WIN32

#include <windows.h>
#include <bcrypt.h>
#include <limits.h> /* ULONG_MAX (not provided by <windows.h>) */

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

#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) \
    || defined(__OpenBSD__) || defined(__DragonFly__)
static sdb_status sdb_random_bytes_getentropy(uint8_t *output, size_t size)
{
    uint8_t *cursor = output;
    size_t remaining = size;

    while (remaining != 0U) {
        const size_t chunk = remaining > 256U ? 256U : remaining;
        int result;
        do {
            result = getentropy(cursor, chunk);
        } while (result != 0 && errno == EINTR);
        if (result != 0) {
            return SDB_E_IO;
        }
        cursor += chunk;
        remaining -= chunk;
    }
    return SDB_OK;
}
#endif

static sdb_status sdb_random_bytes_urandom(uint8_t *output, size_t size)
{

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
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) \
    || defined(__OpenBSD__) || defined(__DragonFly__)
    return sdb_random_bytes_getentropy(output, size);
#endif
    return sdb_random_bytes_urandom(output, size);
}
#endif
