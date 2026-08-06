#include "shibadb_engine.h"

#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

_Static_assert(sizeof(sdb_status) == 4U, "sdb_status ABI changed");
_Static_assert(sizeof(sdb_superblock_v1) == 136U, "superblock ABI changed");
#if SIZE_MAX == UINT64_MAX
_Static_assert(
    sizeof(sdb_superblock_read_result) == 144U,
    "superblock result ABI changed"
);
_Static_assert(sizeof(sdb_database_options) == 64U, "options ABI changed");
_Static_assert(sizeof(sdb_index_term) == 32U, "index term ABI changed");
_Static_assert(
    offsetof(sdb_database_options, password) == 16U,
    "options password offset changed"
);
#elif SIZE_MAX == UINT32_MAX
_Static_assert(
    sizeof(sdb_superblock_read_result) == 140U,
    "superblock result ABI changed"
);
_Static_assert(sizeof(sdb_database_options) == 52U, "options ABI changed");
_Static_assert(sizeof(sdb_index_term) == 16U, "index term ABI changed");
_Static_assert(
    offsetof(sdb_database_options, password) == 12U,
    "options password offset changed"
);
#else
#error Unsupported size_t width
#endif

_Static_assert(sizeof(sdb_verify_result) == 104U, "verify ABI changed");
_Static_assert(sizeof(sdb_backup_result) == 40U, "backup ABI changed");
_Static_assert(sizeof(sdb_compact_result) == 64U, "compact ABI changed");
_Static_assert(
    offsetof(sdb_database_options, struct_size) == 0U,
    "options prefix changed"
);

int main(void)
{
    sdb_database_options options;
    size_t index;
    assert(sdb_abi_version() == SDB_ABI_VERSION);
    assert(strcmp(sdb_version_string(), SDB_VERSION_STRING) == 0);
    sdb_database_options_init(&options);
    assert(options.struct_size == (uint32_t)sizeof(options));
    assert(options.kdf_iterations == SDB_DEFAULT_KDF_ITERATIONS);
    for (index = 0U;
         index < sizeof(options.reserved) / sizeof(options.reserved[0]);
         ++index) {
        assert(options.reserved[index] == 0U);
    }
    (void)puts("ABI tests: ok");
    return 0;
}
