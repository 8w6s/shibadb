#include <shibadb_engine.h>

#include <cstdint>

int main()
{
    sdb_database_options options{};
    sdb_database_options_init(&options);
    return sdb_abi_version() == SDB_ABI_VERSION
            && options.struct_size == sizeof(options)
        ? 0 : 1;
}
