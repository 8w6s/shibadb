#include "shibadb.h"

#include <stddef.h>
#include <stdint.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    sdb_superblock_v1 decoded;
    (void)sdb_superblock_v1_decode(data, size, &decoded);
    return 0;
}

