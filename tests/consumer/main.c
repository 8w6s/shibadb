#include <shibadb_engine.h>

#include <stdio.h>

int main(void)
{
    sdb_database_options options;
    if (sdb_abi_version() != SDB_ABI_VERSION) {
        return 1;
    }
    sdb_database_options_init(&options);
    if (options.struct_size != (uint32_t)sizeof(options)) {
        return 2;
    }
    (void)puts(sdb_version_string());
    return 0;
}
