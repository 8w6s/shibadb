#include "btree_page.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    sdb_btree_node node;
    if (size > UINT16_MAX) {
        return 0;
    }
    if (sdb_btree_node_decode(data, size, &node) == SDB_OK) {
        size_t encoded_size;
        if (sdb_btree_node_encoded_size(&node, &encoded_size) == SDB_OK) {
            uint8_t *encoded = (uint8_t *)malloc(encoded_size);
            size_t written;
            if (encoded != NULL
                && sdb_btree_node_encode(
                    &node, encoded, encoded_size, &written
                ) == SDB_OK) {
                sdb_btree_node roundtrip;
                if (sdb_btree_node_decode(
                    encoded, written, &roundtrip
                ) == SDB_OK) {
                    sdb_btree_node_destroy(&roundtrip);
                }
            }
            free(encoded);
        }
        sdb_btree_node_destroy(&node);
    }
    return 0;
}
