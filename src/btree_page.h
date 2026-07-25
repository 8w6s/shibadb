#ifndef SHIBADB_BTREE_PAGE_H
#define SHIBADB_BTREE_PAGE_H

#include "internal.h"

#define SDB_BTREE_NODE_HEADER_SIZE ((size_t)20)

typedef enum sdb_btree_node_kind {
    SDB_BTREE_LEAF = 1,
    SDB_BTREE_INTERNAL = 2
} sdb_btree_node_kind;

typedef struct sdb_btree_entry {
    uint8_t *key;
    size_t key_size;
    uint8_t *value;
    size_t value_size;
    uint64_t right_child;
} sdb_btree_entry;

typedef struct sdb_btree_node {
    sdb_btree_node_kind kind;
    uint64_t right_sibling;
    uint64_t first_child;
    sdb_btree_entry *entries;
    size_t count;
} sdb_btree_node;

void sdb_btree_node_destroy(sdb_btree_node *node);
sdb_status sdb_btree_node_decode(
    const uint8_t *payload, size_t payload_size, sdb_btree_node *node_out
);
sdb_status sdb_btree_node_encoded_size(
    const sdb_btree_node *node, size_t *size_out
);
sdb_status sdb_btree_node_encode(
    const sdb_btree_node *node,
    uint8_t *output,
    size_t output_size,
    size_t *written_out
);

#endif
