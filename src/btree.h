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

typedef struct sdb_btree_cursor {
    sdb_btree *tree;
    sdb_btree_node leaf;
    uint64_t page_id;
    size_t index;
    bool valid;
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
/*
 * Cursor API — ordered iteration over the tree.
 *
 * Lifecycle: sdb_btree_cursor_first or sdb_btree_cursor_seek
 * initialises a cursor; sdb_btree_cursor_read inspects the current
 * position; sdb_btree_cursor_next advances; sdb_btree_cursor_close
 * releases resources. Every successfully-initialised cursor must be
 * closed exactly once.
 *
 * End-of-iteration contract: sdb_btree_cursor_next returns SDB_OK
 * once the cursor passes the last entry, and sets cursor->valid to
 * false at that point. Callers check `cursor->valid` after each
 * next() to decide whether to continue the loop; SDB_E_NOT_FOUND is
 * NOT the end-of-iteration signal (it is reserved for read/seek
 * failures on a live tree).
 *
 * Snapshot semantics: a cursor holds a decoded copy of the current
 * leaf node in its own storage (`sdb_btree_cursor::leaf`). Mutations
 * to the tree from a concurrent transaction (or from a mutating
 * callback if the cursor is being driven by sdb_index_visit) do not
 * invalidate the in-cursor copy, but the key/value pointers inside
 * that copy remain live only until sdb_btree_cursor_close or the
 * next state-mutating cursor call.
 *
 * Thread safety: cursors are NOT thread-safe. A cursor must be
 * touched from a single thread only.
 */
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
