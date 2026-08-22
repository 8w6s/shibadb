
#ifndef SHIBADB_H
#define SHIBADB_H

#include <stddef.h>
#include <stdint.h>

#include "shibadb_visibility.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SDB_FORMAT_VERSION_V1 UINT16_C(1)
#define SDB_FORMAT_VERSION_V2 UINT16_C(2)
#define SDB_FORMAT_VERSION_CURRENT SDB_FORMAT_VERSION_V2

#define SDB_SUPERBLOCK_HEADER_SIZE ((size_t)160)

#define SDB_MIN_PAGE_SIZE UINT32_C(4096)

#define SDB_MAX_PAGE_SIZE UINT32_C(65536)

#define SDB_SALT_SIZE ((size_t)16)

#define SDB_FILE_ID_SIZE ((size_t)16)

#define SDB_SUPERBLOCK_SLOT_SIZE ((size_t)4096)

#define SDB_SUPERBLOCK_SLOT_COUNT ((size_t)2)

#define SDB_FLAG_ENCRYPTED UINT32_C(1)
#define SDB_FLAG_HEADER_AUTH UINT32_C(2)

#define SDB_WRAPPED_KEY_SIZE ((size_t)32)

#define SDB_KEY_WRAP_TAG_SIZE ((size_t)16)

#define SDB_MIN_KDF_ITERATIONS UINT32_C(600000)

#define SDB_DEFAULT_KDF_ITERATIONS UINT32_C(600000)

#define SDB_MAX_KDF_ITERATIONS UINT32_C(10000000)

#define SDB_ABI_VERSION UINT32_C(1)

#define SDB_VERSION_MAJOR UINT32_C(1)

#define SDB_VERSION_MINOR UINT32_C(0)

#define SDB_VERSION_PATCH UINT32_C(0)

#define SDB_VERSION_STRING "1.0.0"

#define SDB_VERSION_NUMBER UINT32_C(1000000)

typedef enum sdb_status {

    SDB_OK = 0,

    SDB_E_INVALID_ARGUMENT = 1,

    SDB_E_BUFFER_TOO_SMALL = 2,

    SDB_E_BAD_MAGIC = 3,

    SDB_E_UNSUPPORTED_VERSION = 4,

    SDB_E_CORRUPT = 5,

    SDB_E_OVERFLOW = 6,

    SDB_E_IO = 7,

    SDB_E_INTERNAL = 8,

    SDB_E_TRUNCATED = 9,

    SDB_E_NOT_FOUND = 10,

    SDB_E_AUTHENTICATION = 11,

    SDB_E_CONFLICT = 12,

    SDB_E_BUSY = 13,

    SDB_E_OUT_OF_MEMORY = 14,

    SDB_E_NO_SPACE = 15,

    SDB_E_ACCESS_DENIED = 16
} sdb_status;

typedef struct sdb_superblock_v1 {

    uint32_t page_size;

    uint32_t flags;

    uint64_t generation;

    uint64_t checkpoint_lsn;

    uint64_t root_page;

    uint64_t freelist_page;

    uint64_t next_page_id;

    uint32_t key_wrap_id;

    uint32_t kdf_iterations;

    uint8_t salt[SDB_SALT_SIZE];

    uint8_t file_id[SDB_FILE_ID_SIZE];

    uint8_t wrapped_key[SDB_WRAPPED_KEY_SIZE];

    uint8_t key_wrap_tag[SDB_KEY_WRAP_TAG_SIZE];
} sdb_superblock_v1;

typedef struct sdb_superblock_read_result {

    sdb_superblock_v1 superblock;

    uint8_t valid_mirror_count;

    uint8_t selected_slot;

    uint16_t format_version;
} sdb_superblock_read_result;

SDB_API uint32_t sdb_abi_version(void);

SDB_API const char *sdb_version_string(void);

SDB_API uint32_t sdb_version_number(void);

SDB_API const char *sdb_status_string(sdb_status status);

SDB_API sdb_status sdb_superblock_v1_encode(
    const sdb_superblock_v1 *superblock,
    uint8_t *output,
    size_t output_size
);

SDB_API sdb_status sdb_superblock_v1_decode(
    const uint8_t *input,
    size_t input_size,
    sdb_superblock_v1 *superblock_out
);

SDB_API sdb_status sdb_superblock_store_create(
    const char *path,
    const sdb_superblock_v1 *superblock
);

SDB_API sdb_status sdb_superblock_store_read(
    const char *path,
    sdb_superblock_read_result *result_out
);

SDB_API sdb_status sdb_superblock_store_update(
    const char *path,
    const sdb_superblock_v1 *superblock
);

#ifdef __cplusplus
}
#endif

#endif
