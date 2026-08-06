#include "shibadb.h"

uint32_t sdb_abi_version(void)
{
    return SDB_ABI_VERSION;
}

const char *sdb_version_string(void)
{
    return SDB_VERSION_STRING;
}

uint32_t sdb_version_number(void)
{
    return SDB_VERSION_NUMBER;
}

const char *sdb_status_string(sdb_status status)
{
    switch (status) {
    case SDB_OK:
        return "ok";
    case SDB_E_INVALID_ARGUMENT:
        return "invalid argument";
    case SDB_E_BUFFER_TOO_SMALL:
        return "buffer too small";
    case SDB_E_BAD_MAGIC:
        return "bad magic";
    case SDB_E_UNSUPPORTED_VERSION:
        return "unsupported version";
    case SDB_E_CORRUPT:
        return "corrupt data";
    case SDB_E_OVERFLOW:
        return "integer overflow";
    case SDB_E_IO:
        return "I/O error";
    case SDB_E_INTERNAL:
        return "internal error";
    case SDB_E_TRUNCATED:
        return "truncated data";
    case SDB_E_NOT_FOUND:
        return "not found";
    case SDB_E_AUTHENTICATION:
        return "authentication failed";
    case SDB_E_CONFLICT:
        return "conflict";
    case SDB_E_BUSY:
        return "database busy";
    case SDB_E_OUT_OF_MEMORY:
        return "out of memory";
    }
    return "unknown status";
}
