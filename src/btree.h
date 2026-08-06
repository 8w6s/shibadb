#ifndef SHIBADB_BTREE_H
#define SHIBADB_BTREE_H

#include "btree_page.h"
#include "pager.h"

typedef struct sdb_btree {
    sdb_pager *pager;
    uint64_t root_page;
    bool open;
} sdb_btree;

typedef struct sdb_btree_mutation {
    uint64_t page_id;
    uint8_t *payload;
    size_t payload_size;
} sdb_btree_mutation;

typedef struct sdb_btree_batch {
    sdb_btree *tree;
    sdb_btree_mutation *items;
    size_t count;
    size_t capacity;
    sdb_txn txn;
    sdb_status failure;
    bool active;
} sdb_btree_batch;

/*
 * One level of the root-to-leaf descent, remembered so a reverse cursor can
 * step to the predecessor leaf without a leaf left-sibling link (which the
 * on-disk format does not have). page_id is the internal node; child_index is
 * which child was taken (0 == first_child, i == entries[i-1].right_child).
 */
typedef struct sdb_btree_path_entry {
    uint64_t page_id;
    size_t child_index;
} sdb_btree_path_entry;

typedef struct sdb_btree_cursor {
    sdb_btree *tree;
    sdb_btree_node leaf;
    uint64_t page_id;
    size_t index;
    bool valid;
    /*
     * Populated by sdb_btree_cursor_last and maintained by
     * sdb_btree_cursor_prev (reverse traversal only; forward first/seek/next
     * leave it unused). Depth-capped at 64 like the descent loop.
     */
    sdb_btree_path_entry path[64];
    size_t depth;
} sdb_btree_cursor;

typedef struct sdb_btree_verify_result {
    uint64_t node_count;
    uint64_t leaf_count;
    uint64_t entry_count;
    uint32_t height;
} sdb_btree_verify_result;

sdb_status sdb_btree_create(sdb_pager *pager, sdb_btree *tree_out);
sdb_status sdb_btree_open(sdb_pager *pager, sdb_btree *tree_out);
sdb_status sdb_btree_get(
    sdb_btree *tree,
    const uint8_t *key,
    size_t key_size,
    uint8_t *value_out,
    size_t value_capacity,
    size_t *value_size_out
);
sdb_status sdb_btree_put(
    sdb_btree *tree,
    const uint8_t *key,
    size_t key_size,
    const uint8_t *value,
    size_t value_size
);
sdb_status sdb_btree_delete(
    sdb_btree *tree, const uint8_t *key, size_t key_size
);
sdb_status sdb_btree_batch_begin(
    sdb_btree *tree, sdb_btree_batch *batch_out
);
sdb_status sdb_btree_batch_get(
    sdb_btree_batch *batch,
    const uint8_t *key,
    size_t key_size,
    uint8_t *value_out,
    size_t value_capacity,
    size_t *value_size_out
);
sdb_status sdb_btree_batch_put(
    sdb_btree_batch *batch,
    const uint8_t *key,
    size_t key_size,
    const uint8_t *value,
    size_t value_size
);
sdb_status sdb_btree_batch_delete(
    sdb_btree_batch *batch, const uint8_t *key, size_t key_size
);
sdb_status sdb_btree_batch_commit(sdb_btree_batch *batch);
void sdb_btree_batch_abort(sdb_btree_batch *batch);

sdb_status sdb_btree_cursor_first(
    sdb_btree *tree, sdb_btree_cursor *cursor_out
);
sdb_status sdb_btree_cursor_seek(
    sdb_btree *tree,
    const uint8_t *key,
    size_t key_size,
    sdb_btree_cursor *cursor_out
);
sdb_status sdb_btree_cursor_read(
    const sdb_btree_cursor *cursor,
    uint8_t *key_out,
    size_t key_capacity,
    size_t *key_size_out,
    uint8_t *value_out,
    size_t value_capacity,
    size_t *value_size_out
);
sdb_status sdb_btree_cursor_next(sdb_btree_cursor *cursor);
/*
 * Like _seek but never advances past an empty tail to the right sibling, so the
 * descent path stays valid for a following _prev. May land invalid; _prev then
 * steps back to the greatest key < the sought key. Use only before _prev.
 */
sdb_status sdb_btree_cursor_seek_floor(
    sdb_btree *tree,
    const uint8_t *key,
    size_t key_size,
    sdb_btree_cursor *cursor_out
);
/* Position at the last (greatest) entry; reverse counterpart of _first. */
sdb_status sdb_btree_cursor_last(
    sdb_btree *tree, sdb_btree_cursor *cursor_out
);
/*
 * Step to the previous (lesser) entry. On the first entry, leaves the cursor
 * invalid and returns SDB_OK. Must follow _last/_prev (uses the descent path),
 * not _next.
 */
sdb_status sdb_btree_cursor_prev(sdb_btree_cursor *cursor);
void sdb_btree_cursor_close(sdb_btree_cursor *cursor);
sdb_status sdb_btree_verify(
    sdb_btree *tree, sdb_btree_verify_result *result_out
);
sdb_status sdb_btree_verify_pages(
    sdb_btree *tree,
    sdb_btree_verify_result *result_out,
    bool *reachable_pages,
    size_t reachable_page_count
);

#endif
