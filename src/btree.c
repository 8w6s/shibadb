#include "btree.h"

#include "superblock_store.h"

#include <stdlib.h>
#include <string.h>

typedef struct sdb_btree_split {
    bool split;
    uint8_t *separator;
    size_t separator_size;
    uint64_t right_page;
} sdb_btree_split;

static int sdb_btree_compare(
    const uint8_t *left,
    size_t left_size,
    const uint8_t *right,
    size_t right_size
)
{
    const size_t common = left_size < right_size ? left_size : right_size;
    const int compared = memcmp(left, right, common);
    if (compared != 0) {
        return compared;
    }
    if (left_size < right_size) {
        return -1;
    }
    return left_size > right_size ? 1 : 0;
}

static sdb_status sdb_btree_copy(
    const uint8_t *source, size_t size, uint8_t **output
)
{
    uint8_t *copy;
    if (size == 0U) {
        *output = NULL;
        return SDB_OK;
    }
    copy = (uint8_t *)malloc(size);
    if (copy == NULL) {
        return SDB_E_OUT_OF_MEMORY;
    }
    (void)memcpy(copy, source, size);
    *output = copy;
    return SDB_OK;
}

static sdb_status sdb_btree_read_node(
    sdb_btree *tree, uint64_t page_id, sdb_btree_node *node_out
)
{
    uint8_t *page;
    sdb_page_view view;
    sdb_status status;
    page = (uint8_t *)malloc((size_t)tree->pager->superblock.page_size);
    if (page == NULL) {
        return SDB_E_OUT_OF_MEMORY;
    }
    status = sdb_pager_read(
        tree->pager,
        page_id,
        page,
        (size_t)tree->pager->superblock.page_size,
        &view
    );
    if (status == SDB_OK && view.type != (uint16_t)SDB_PAGE_TYPE_DATA) {
        status = SDB_E_CORRUPT;
    }
    if (status == SDB_OK) {
        status = sdb_btree_node_decode(
            view.payload, (size_t)view.payload_size, node_out
        );
    }
    free(page);
    return status;
}

static void sdb_btree_changes_destroy(sdb_btree_batch *changes)
{
    size_t index;
    for (index = 0U; index < changes->count; ++index) {
        free(changes->items[index].payload);
    }
    free(changes->items);
    (void)memset(changes, 0, sizeof(*changes));
}

static sdb_status sdb_btree_stage(
    sdb_btree_batch *changes,
    uint64_t page_id,
    const sdb_btree_node *node
)
{
    const size_t payload_capacity =
        sdb_pager_payload_capacity(changes->tree->pager);
    uint8_t *payload;
    size_t payload_size;
    size_t index;
    sdb_status status = sdb_btree_node_encoded_size(node, &payload_size);
    if (status != SDB_OK) {
        return status;
    }
    if (payload_size > payload_capacity) {
        return SDB_E_BUFFER_TOO_SMALL;
    }
    payload = (uint8_t *)malloc(payload_size);
    if (payload == NULL) {
        return SDB_E_OUT_OF_MEMORY;
    }
    status = sdb_btree_node_encode(
        node, payload, payload_size, &payload_size
    );
    if (status != SDB_OK) {
        free(payload);
        return status;
    }
    for (index = 0U; index < changes->count; ++index) {
        if (changes->items[index].page_id == page_id) {
            free(changes->items[index].payload);
            changes->items[index].payload = payload;
            changes->items[index].payload_size = payload_size;
            return SDB_OK;
        }
    }
    if (changes->count == changes->capacity) {
        size_t next_capacity =
            changes->capacity == 0U ? 8U : changes->capacity * 2U;
        sdb_btree_mutation *grown;
        if (next_capacity < changes->capacity
            || next_capacity > SIZE_MAX / sizeof(*grown)) {
            free(payload);
            return SDB_E_OVERFLOW;
        }
        grown = (sdb_btree_mutation *)realloc(
            changes->items, next_capacity * sizeof(*grown)
        );
        if (grown == NULL) {
            free(payload);
            return SDB_E_OUT_OF_MEMORY;
        }
        changes->items = grown;
        changes->capacity = next_capacity;
    }
    changes->items[changes->count].page_id = page_id;
    changes->items[changes->count].payload = payload;
    changes->items[changes->count].payload_size = payload_size;
    ++changes->count;
    return SDB_OK;
}

static sdb_btree_mutation *sdb_btree_find_staged(
    sdb_btree_batch *changes, uint64_t page_id
)
{
    size_t index;
    for (index = 0U; index < changes->count; ++index) {
        if (changes->items[index].page_id == page_id) {
            return &changes->items[index];
        }
    }
    return NULL;
}

static sdb_status sdb_btree_batch_read_node(
    sdb_btree_batch *batch, uint64_t page_id, sdb_btree_node *node_out
)
{
    sdb_btree_mutation *staged = sdb_btree_find_staged(batch, page_id);
    if (staged != NULL) {
        return sdb_btree_node_decode(
            staged->payload, staged->payload_size, node_out
        );
    }
    return sdb_btree_read_node(batch->tree, page_id, node_out);
}

static void sdb_btree_unstage(
    sdb_btree_batch *batch, uint64_t page_id
)
{
    size_t index;
    for (index = 0U; index < batch->count; ++index) {
        if (batch->items[index].page_id == page_id) {
            free(batch->items[index].payload);
            (void)memmove(
                &batch->items[index],
                &batch->items[index + 1U],
                (batch->count - index - 1U) * sizeof(*batch->items)
            );
            --batch->count;
            return;
        }
    }
}

static bool sdb_btree_txn_allocated(
    const sdb_btree_batch *batch, uint64_t page_id
)
{
    size_t index;
    for (index = 0U;
         index < batch->txn.allocated_page_count;
         ++index) {
        if (batch->txn.allocated_pages[index] == page_id) {
            return true;
        }
    }
    return false;
}

static size_t sdb_btree_lower_bound(
    const sdb_btree_node *node, const uint8_t *key, size_t key_size
)
{
    size_t low = 0U;
    size_t high = node->count;
    while (low < high) {
        const size_t middle = low + ((high - low) / 2U);
        if (sdb_btree_compare(
                node->entries[middle].key,
                node->entries[middle].key_size,
                key,
                key_size
            ) < 0) {
            low = middle + 1U;
        } else {
            high = middle;
        }
    }
    return low;
}

static uint64_t sdb_btree_child_at(
    const sdb_btree_node *node, size_t child_index
)
{
    return child_index == 0U
        ? node->first_child : node->entries[child_index - 1U].right_child;
}

static void sdb_btree_remove_child(
    sdb_btree_node *node, size_t child_index
)
{
    size_t entry_index;

    if (node->count == 0U || child_index > node->count) {
        return;
    }
    entry_index = child_index == 0U ? 0U : child_index - 1U;
    if (child_index == 0U) {
        node->first_child = node->entries[0].right_child;
    }
    free(node->entries[entry_index].key);
    free(node->entries[entry_index].value);
    (void)memmove(
        &node->entries[entry_index],
        &node->entries[entry_index + 1U],
        (node->count - entry_index - 1U) * sizeof(*node->entries)
    );
    --node->count;
}

static sdb_status sdb_btree_reclaim_internal_up(
    sdb_btree_batch *batch,
    uint64_t node_page,
    uint64_t survivor_page,
    const uint64_t path_pages[64],
    const size_t path_children[64],
    size_t node_depth_in_path
)
{
    sdb_btree_node parent;
    sdb_btree_node sibling;
    sdb_btree_node combined;
    uint64_t parent_page;
    size_t node_slot;
    uint64_t sibling_page = 0U;
    uint64_t left_page = 0U;
    uint64_t right_page = 0U;
    size_t sep_slot = 0U;
    size_t capacity;
    size_t combined_size = 0U;
    bool parent_valid = false;
    bool sibling_valid = false;
    bool combined_valid = false;
    sdb_status status;

    if (node_depth_in_path == 0U || node_page == 0U || survivor_page == 0U) {
        return SDB_E_CORRUPT;
    }
    parent_page = path_pages[node_depth_in_path - 1U];
    node_slot = path_children[node_depth_in_path - 1U];
    if (parent_page == 0U || parent_page == node_page) {
        return SDB_E_CORRUPT;
    }
    capacity = sdb_pager_payload_capacity(batch->tree->pager);

    (void)memset(&parent, 0, sizeof(parent));
    (void)memset(&sibling, 0, sizeof(sibling));
    (void)memset(&combined, 0, sizeof(combined));

    status = sdb_btree_batch_read_node(batch, parent_page, &parent);
    if (status == SDB_OK) {
        parent_valid = true;
        if (parent.kind != SDB_BTREE_INTERNAL
            || node_slot > parent.count
            || parent.count == 0U) {
            status = SDB_E_CORRUPT;
        }
    }

    if (status == SDB_OK) {
        if (node_slot >= 1U) {
            sep_slot = node_slot - 1U;
            sibling_page = sdb_btree_child_at(&parent, node_slot - 1U);
            left_page = sibling_page;
            right_page = node_page;
        } else {
            sep_slot = 0U;
            sibling_page = sdb_btree_child_at(&parent, 1U);
            left_page = node_page;
            right_page = sibling_page;
        }

        if (sibling_page == node_page
            || sibling_page == 0U
            || sibling_page == parent_page
            || survivor_page == parent_page
            || survivor_page == sibling_page) {
            status = SDB_E_CORRUPT;
        }
        if (status == SDB_OK) {
            status = sdb_btree_batch_read_node(batch, sibling_page, &sibling);
            if (status == SDB_OK) {
                sibling_valid = true;
                if (sibling.kind != SDB_BTREE_INTERNAL) {
                    status = SDB_E_CORRUPT;
                }
            }
        }
    }

    if (status == SDB_OK) {
        const size_t new_count = sibling.count + 1U;
        if (new_count > SIZE_MAX / sizeof(*combined.entries)) {
            status = SDB_E_OVERFLOW;
        } else {
            combined.entries = (sdb_btree_entry *)malloc(
                new_count * sizeof(*combined.entries)
            );
            if (combined.entries == NULL) {
                status = SDB_E_OUT_OF_MEMORY;
            }
        }
    }
    if (status == SDB_OK) {
        uint8_t *sep_key = NULL;
        const size_t sep_size = parent.entries[sep_slot].key_size;
        status = sdb_btree_copy(
            parent.entries[sep_slot].key, sep_size, &sep_key
        );
        if (status != SDB_OK) {
            free(combined.entries);
            combined.entries = NULL;
        } else {
            combined.kind = SDB_BTREE_INTERNAL;
            combined_valid = true;
            if (left_page == sibling_page) {

                combined.first_child = sibling.first_child;
                if (sibling.count != 0U) {
                    (void)memcpy(
                        combined.entries,
                        sibling.entries,
                        sibling.count * sizeof(*combined.entries)
                    );
                }
                combined.entries[sibling.count].key = sep_key;
                combined.entries[sibling.count].key_size = sep_size;
                combined.entries[sibling.count].value = NULL;
                combined.entries[sibling.count].value_size = 0U;
                combined.entries[sibling.count].right_child = survivor_page;
            } else {

                combined.first_child = survivor_page;
                combined.entries[0].key = sep_key;
                combined.entries[0].key_size = sep_size;
                combined.entries[0].value = NULL;
                combined.entries[0].value_size = 0U;
                combined.entries[0].right_child = sibling.first_child;
                if (sibling.count != 0U) {
                    (void)memcpy(
                        &combined.entries[1],
                        sibling.entries,
                        sibling.count * sizeof(*combined.entries)
                    );
                }
            }
            combined.count = sibling.count + 1U;

            sibling.count = 0U;
            free(sibling.entries);
            sibling.entries = NULL;
            sibling_valid = false;
        }
    }

    if (status == SDB_OK) {
        status = sdb_btree_node_encoded_size(&combined, &combined_size);
    }

    if (status == SDB_OK && combined_size <= capacity) {

        const bool root_collapse =
            parent_page == batch->tree->root_page && parent.count == 1U;
        const uint64_t merge_target =
            root_collapse ? batch->tree->root_page : sibling_page;
        status = sdb_btree_stage(batch, merge_target, &combined);
        if (status == SDB_OK) {
            sdb_btree_remove_child(&parent, node_slot);
            sdb_btree_unstage(batch, node_page);
            status = sdb_txn_free(&batch->txn, node_page);
        }
        if (status == SDB_OK) {
            if (root_collapse) {

                sdb_btree_unstage(batch, sibling_page);
                status = sdb_txn_free(&batch->txn, sibling_page);
            } else if (parent.count > 0U) {
                status = sdb_btree_stage(batch, parent_page, &parent);
            } else {

                status = sdb_btree_reclaim_internal_up(
                    batch,
                    parent_page,
                    parent.first_child,
                    path_pages,
                    path_children,
                    node_depth_in_path - 1U
                );
            }
        }
    } else if (status == SDB_OK) {

        sdb_btree_node left_half;
        sdb_btree_node right_half;
        size_t middle = SIZE_MAX;
        size_t best_largest = SIZE_MAX;
        size_t total_entries_size = 0U;
        size_t left_entries_size;
        size_t candidate;
        uint8_t *promoted_key = NULL;

        for (candidate = 0U; candidate < combined.count; ++candidate) {
            total_entries_size += 12U + combined.entries[candidate].key_size;
        }
        left_entries_size = 12U + combined.entries[0].key_size;
        for (candidate = 1U; candidate + 1U < combined.count; ++candidate) {
            const size_t promoted_entry_size =
                12U + combined.entries[candidate].key_size;
            const size_t left_size =
                SDB_BTREE_NODE_HEADER_SIZE + left_entries_size;
            const size_t right_size = SDB_BTREE_NODE_HEADER_SIZE
                + total_entries_size - left_entries_size - promoted_entry_size;
            const size_t largest =
                left_size > right_size ? left_size : right_size;
            if (left_size <= capacity && right_size <= capacity
                && largest < best_largest) {
                middle = candidate;
                best_largest = largest;
            }
            left_entries_size += promoted_entry_size;
        }
        if (middle == SIZE_MAX) {
            status = SDB_E_BUFFER_TOO_SMALL;
        } else {
            status = sdb_btree_copy(
                combined.entries[middle].key,
                combined.entries[middle].key_size,
                &promoted_key
            );
        }
        if (status == SDB_OK) {

            (void)memset(&left_half, 0, sizeof(left_half));
            (void)memset(&right_half, 0, sizeof(right_half));
            left_half.kind = SDB_BTREE_INTERNAL;
            left_half.first_child = combined.first_child;
            left_half.entries = combined.entries;
            left_half.count = middle;
            right_half.kind = SDB_BTREE_INTERNAL;
            right_half.first_child = combined.entries[middle].right_child;
            right_half.entries = &combined.entries[middle + 1U];
            right_half.count = combined.count - middle - 1U;

            status = sdb_btree_stage(batch, left_page, &left_half);
            if (status == SDB_OK) {
                status = sdb_btree_stage(batch, right_page, &right_half);
            }
            if (status == SDB_OK) {

                free(parent.entries[sep_slot].key);
                parent.entries[sep_slot].key = promoted_key;
                parent.entries[sep_slot].key_size =
                    combined.entries[middle].key_size;
                promoted_key = NULL;
                status = sdb_btree_stage(batch, parent_page, &parent);
            }
        }
        free(promoted_key);
    }

    if (parent_valid) {
        sdb_btree_node_destroy(&parent);
    }
    if (sibling_valid) {
        sdb_btree_node_destroy(&sibling);
    }
    if (combined_valid) {
        sdb_btree_node_destroy(&combined);
    }
    return status;
}

static sdb_status sdb_btree_reclaim_empty_leaf(
    sdb_btree_batch *batch,
    uint64_t leaf_page,
    uint64_t leaf_right_sibling,
    const uint64_t path_pages[64],
    const size_t path_children[64],
    size_t depth
)
{
    sdb_btree_node parent;
    const uint64_t parent_page = path_pages[depth - 1U];
    const size_t parent_child = path_children[depth - 1U];
    size_t level;
    sdb_status status = SDB_OK;

    for (level = depth; level > 0U; --level) {
        sdb_btree_node ancestor;
        const size_t child_index = path_children[level - 1U];
        uint64_t predecessor_page;
        if (child_index == 0U) {
            continue;
        }
        status = sdb_btree_batch_read_node(
            batch, path_pages[level - 1U], &ancestor
        );
        if (status != SDB_OK) {
            return status;
        }
        predecessor_page = sdb_btree_child_at(
            &ancestor, child_index - 1U
        );
        sdb_btree_node_destroy(&ancestor);
        {
            size_t hops = 0U;
            for (;;) {
                sdb_btree_node predecessor;
                status = sdb_btree_batch_read_node(
                    batch, predecessor_page, &predecessor
                );
                if (status != SDB_OK) {
                    return status;
                }
                if (predecessor.kind == SDB_BTREE_LEAF) {
                    predecessor.right_sibling = leaf_right_sibling;
                    status = sdb_btree_stage(
                        batch, predecessor_page, &predecessor
                    );
                    sdb_btree_node_destroy(&predecessor);
                    break;
                }
                /*
                 * Bound the descent to the rightmost child like every other
                 * tree walk in this file (depth >= 64). A corrupt tree with a
                 * child-pointer cycle would otherwise loop here forever;
                 * capping it turns the corruption into a clean SDB_E_CORRUPT.
                 */
                if (hops >= 64U) {
                    sdb_btree_node_destroy(&predecessor);
                    return SDB_E_CORRUPT;
                }
                ++hops;
                predecessor_page = sdb_btree_child_at(
                    &predecessor, predecessor.count
                );
                sdb_btree_node_destroy(&predecessor);
            }
        }
        break;
    }
    if (status != SDB_OK) {
        return status;
    }
    status = sdb_btree_batch_read_node(batch, parent_page, &parent);
    if (status != SDB_OK || parent.kind != SDB_BTREE_INTERNAL
        || parent.count == 0U || parent_child > parent.count) {
        if (status == SDB_OK) {
            sdb_btree_node_destroy(&parent);
        }
        return status == SDB_OK ? SDB_E_CORRUPT : status;
    }
    sdb_btree_remove_child(&parent, parent_child);
    sdb_btree_unstage(batch, leaf_page);
    status = sdb_txn_free(&batch->txn, leaf_page);
    if (status != SDB_OK) {
        sdb_btree_node_destroy(&parent);
        return status;
    }
    if (parent.count != 0U) {
        status = sdb_btree_stage(batch, parent_page, &parent);
        sdb_btree_node_destroy(&parent);
        return status;
    }
    if (parent_page == batch->tree->root_page) {
        sdb_btree_node child;
        const uint64_t child_page = parent.first_child;
        bool child_valid;
        (void)memset(&child, 0, sizeof(child));
        status = sdb_btree_batch_read_node(batch, child_page, &child);
        child_valid = status == SDB_OK;
        if (status == SDB_OK && child.kind == SDB_BTREE_LEAF) {

            child.right_sibling = 0U;
        }
        if (status == SDB_OK) {

            sdb_btree_unstage(batch, child_page);
            status = sdb_txn_free(&batch->txn, child_page);
        }
        if (status == SDB_OK) {
            status = sdb_btree_stage(
                batch, batch->tree->root_page, &child
            );
        }
        if (child_valid) {
            sdb_btree_node_destroy(&child);
        }
    } else if (depth >= 2U) {

        status = sdb_btree_reclaim_internal_up(
            batch,
            parent_page,
            parent.first_child,
            path_pages,
            path_children,
            depth - 1U
        );
    } else {

        status = SDB_E_CORRUPT;
    }
    sdb_btree_node_destroy(&parent);
    return status;
}

static sdb_status sdb_btree_leaf_put(
    sdb_btree_node *node,
    const uint8_t *key,
    size_t key_size,
    const uint8_t *value,
    size_t value_size
)
{
    size_t position = sdb_btree_lower_bound(node, key, key_size);
    uint8_t *key_copy = NULL;
    uint8_t *value_copy = NULL;
    sdb_status status;
    if (position < node->count
        && sdb_btree_compare(
            node->entries[position].key,
            node->entries[position].key_size,
            key,
            key_size
        ) == 0) {
        status = sdb_btree_copy(value, value_size, &value_copy);
        if (status != SDB_OK) {
            return status;
        }
        free(node->entries[position].value);
        node->entries[position].value = value_copy;
        node->entries[position].value_size = value_size;
        return SDB_OK;
    }
    status = sdb_btree_copy(key, key_size, &key_copy);
    if (status == SDB_OK) {
        status = sdb_btree_copy(value, value_size, &value_copy);
    }
    if (status != SDB_OK) {
        free(key_copy);
        free(value_copy);
        return status;
    }
    if (node->count == SIZE_MAX / sizeof(*node->entries)) {
        free(key_copy);
        free(value_copy);
        return SDB_E_OVERFLOW;
    }
    {
        sdb_btree_entry *grown = (sdb_btree_entry *)realloc(
            node->entries, (node->count + 1U) * sizeof(*grown)
        );
        if (grown == NULL) {
            free(key_copy);
            free(value_copy);
            return SDB_E_OUT_OF_MEMORY;
        }
        node->entries = grown;
    }
    (void)memmove(
        &node->entries[position + 1U],
        &node->entries[position],
        (node->count - position) * sizeof(*node->entries)
    );
    (void)memset(&node->entries[position], 0, sizeof(*node->entries));
    node->entries[position].key = key_copy;
    node->entries[position].key_size = key_size;
    node->entries[position].value = value_copy;
    node->entries[position].value_size = value_size;
    ++node->count;
    return SDB_OK;
}

static sdb_status sdb_btree_internal_insert(
    sdb_btree_node *node,
    size_t position,
    const uint8_t *separator,
    size_t separator_size,
    uint64_t right_child
)
{
    uint8_t *copy;
    sdb_btree_entry *grown;
    sdb_status status = sdb_btree_copy(
        separator, separator_size, &copy
    );
    if (status != SDB_OK) {
        return status;
    }
    grown = (sdb_btree_entry *)realloc(
        node->entries, (node->count + 1U) * sizeof(*grown)
    );
    if (grown == NULL) {
        free(copy);
        return SDB_E_OUT_OF_MEMORY;
    }
    node->entries = grown;
    (void)memmove(
        &node->entries[position + 1U],
        &node->entries[position],
        (node->count - position) * sizeof(*node->entries)
    );
    (void)memset(&node->entries[position], 0, sizeof(*node->entries));
    node->entries[position].key = copy;
    node->entries[position].key_size = separator_size;
    node->entries[position].right_child = right_child;
    ++node->count;
    return SDB_OK;
}

static sdb_status sdb_btree_split_leaf(
    sdb_btree_batch *changes,
    uint64_t page_id,
    sdb_btree_node *left,
    sdb_btree_split *split_out
)
{
    sdb_btree_node right;
    uint64_t right_page;
    size_t middle = 0U;
    size_t best_largest = SIZE_MAX;
    size_t candidate;
    size_t total_entries_size = 0U;
    size_t left_entries_size = 0U;
    const size_t capacity =
        sdb_pager_payload_capacity(changes->tree->pager);
    size_t right_count;
    sdb_status status;
    for (candidate = 0U; candidate < left->count; ++candidate) {
        const sdb_btree_entry *entry = &left->entries[candidate];

        const uint64_t entry_size_wide =
            (uint64_t)6U
            + (uint64_t)entry->key_size
            + (uint64_t)entry->value_size;
        size_t entry_size;
        if (entry_size_wide > (uint64_t)SIZE_MAX
            || (size_t)entry_size_wide
                > SIZE_MAX - total_entries_size) {
            return SDB_E_OVERFLOW;
        }
        entry_size = (size_t)entry_size_wide;
        total_entries_size += entry_size;
    }
    for (candidate = 1U; candidate < left->count; ++candidate) {
        const sdb_btree_entry *previous = &left->entries[candidate - 1U];

        const size_t previous_size = (size_t)(
            (uint64_t)6U
            + (uint64_t)previous->key_size
            + (uint64_t)previous->value_size
        );
        size_t left_size;
        size_t right_size;
        size_t largest;
        left_entries_size += previous_size;
        left_size = SDB_BTREE_NODE_HEADER_SIZE + left_entries_size;
        right_size = SDB_BTREE_NODE_HEADER_SIZE
            + total_entries_size - left_entries_size;
        largest = left_size > right_size ? left_size : right_size;
        if (left_size <= capacity && right_size <= capacity
            && largest < best_largest) {
            middle = candidate;
            best_largest = largest;
        }
    }
    if (middle == 0U) {
        return SDB_E_BUFFER_TOO_SMALL;
    }
    right_count = left->count - middle;
    (void)memset(&right, 0, sizeof(right));
    right.kind = SDB_BTREE_LEAF;
    right.count = right_count;
    right.right_sibling = left->right_sibling;
    right.entries = (sdb_btree_entry *)malloc(
        right_count * sizeof(*right.entries)
    );
    if (right.entries == NULL) {
        return SDB_E_OUT_OF_MEMORY;
    }
    (void)memcpy(
        right.entries,
        &left->entries[middle],
        right_count * sizeof(*right.entries)
    );
    left->count = middle;
    status = sdb_txn_allocate(&changes->txn, &right_page);
    if (status == SDB_OK) {
        left->right_sibling = right_page;
        status = sdb_btree_stage(changes, page_id, left);
    }
    if (status == SDB_OK) {
        status = sdb_btree_stage(changes, right_page, &right);
    }
    if (status == SDB_OK) {
        status = sdb_btree_copy(
            right.entries[0].key,
            right.entries[0].key_size,
            &split_out->separator
        );
    }
    if (status == SDB_OK) {
        split_out->split = true;
        split_out->separator_size = right.entries[0].key_size;
        split_out->right_page = right_page;
    }
    sdb_btree_node_destroy(&right);
    return status;
}

static sdb_status sdb_btree_split_internal(
    sdb_btree_batch *changes,
    uint64_t page_id,
    sdb_btree_node *left,
    sdb_btree_split *split_out
)
{
    sdb_btree_node right;
    size_t middle = SIZE_MAX;
    size_t best_largest = SIZE_MAX;
    size_t total_entries_size = 0U;
    size_t left_entries_size = 0U;
    size_t candidate;
    const size_t capacity =
        sdb_pager_payload_capacity(changes->tree->pager);
    size_t right_count;
    uint64_t right_page;
    uint8_t *promoted = NULL;
    size_t promoted_size;
    sdb_status status;
    for (candidate = 0U; candidate < left->count; ++candidate) {
        const size_t entry_size =
            12U + left->entries[candidate].key_size;
        if (entry_size > SIZE_MAX - total_entries_size) {
            return SDB_E_OVERFLOW;
        }
        total_entries_size += entry_size;
    }

    if (left->count < 3U) {
        return SDB_E_BUFFER_TOO_SMALL;
    }
    left_entries_size = 12U + left->entries[0].key_size;
    for (candidate = 1U; candidate + 1U < left->count; ++candidate) {
        const size_t promoted_entry_size =
            12U + left->entries[candidate].key_size;
        const size_t left_size =
            SDB_BTREE_NODE_HEADER_SIZE + left_entries_size;
        const size_t right_size = SDB_BTREE_NODE_HEADER_SIZE
            + total_entries_size - left_entries_size - promoted_entry_size;
        const size_t largest = left_size > right_size
            ? left_size : right_size;
        if (left_size <= capacity && right_size <= capacity
            && largest < best_largest) {
            middle = candidate;
            best_largest = largest;
        }
        left_entries_size += promoted_entry_size;
    }
    if (middle == SIZE_MAX) {
        return SDB_E_BUFFER_TOO_SMALL;
    }
    right_count = left->count - middle - 1U;
    promoted_size = left->entries[middle].key_size;
    status = sdb_btree_copy(
        left->entries[middle].key, promoted_size, &promoted
    );
    if (status != SDB_OK) {
        return status;
    }
    (void)memset(&right, 0, sizeof(right));
    right.kind = SDB_BTREE_INTERNAL;
    right.first_child = left->entries[middle].right_child;
    right.count = right_count;
    if (right_count != 0U) {
        right.entries = (sdb_btree_entry *)malloc(
            right_count * sizeof(*right.entries)
        );
        if (right.entries == NULL) {
            free(promoted);
            return SDB_E_OUT_OF_MEMORY;
        }
        (void)memcpy(
            right.entries,
            &left->entries[middle + 1U],
            right_count * sizeof(*right.entries)
        );
    }
    free(left->entries[middle].key);
    left->entries[middle].key = NULL;
    left->count = middle;
    status = sdb_txn_allocate(&changes->txn, &right_page);
    if (status == SDB_OK) {
        status = sdb_btree_stage(changes, page_id, left);
    }
    if (status == SDB_OK) {
        status = sdb_btree_stage(changes, right_page, &right);
    }
    if (status == SDB_OK) {
        split_out->split = true;
        split_out->separator = promoted;
        split_out->separator_size = promoted_size;
        split_out->right_page = right_page;
        promoted = NULL;
    }
    free(promoted);
    sdb_btree_node_destroy(&right);
    return status;
}

static sdb_status sdb_btree_insert_recursive(
    sdb_btree_batch *changes,
    uint64_t page_id,
    const uint8_t *key,
    size_t key_size,
    const uint8_t *value,
    size_t value_size,
    size_t depth,
    sdb_btree_split *split_out
)
{
    sdb_btree_node node;
    size_t encoded_size = 0U;
    const size_t payload_capacity =
        sdb_pager_payload_capacity(changes->tree->pager);
    sdb_status status;

    (void)memset(split_out, 0, sizeof(*split_out));

    if (depth >= 64U) {
        return SDB_E_CORRUPT;
    }
    status = sdb_btree_batch_read_node(changes, page_id, &node);
    if (status != SDB_OK) {
        return status;
    }
    if (node.kind == SDB_BTREE_LEAF) {
        status = sdb_btree_leaf_put(
            &node, key, key_size, value, value_size
        );
    } else {
        size_t child_index = sdb_btree_lower_bound(&node, key, key_size);
        uint64_t child_page;
        sdb_btree_split child_split;
        if (child_index < node.count
            && sdb_btree_compare(
                node.entries[child_index].key,
                node.entries[child_index].key_size,
                key,
                key_size
            ) == 0) {
            ++child_index;
        }
        child_page = child_index == 0U
            ? node.first_child : node.entries[child_index - 1U].right_child;
        status = sdb_btree_insert_recursive(
            changes,
            child_page,
            key,
            key_size,
            value,
            value_size,
            depth + 1U,
            &child_split
        );
        if (status == SDB_OK && child_split.split) {
            status = sdb_btree_internal_insert(
                &node,
                child_index,
                child_split.separator,
                child_split.separator_size,
                child_split.right_page
            );
        }
        free(child_split.separator);
    }
    if (status == SDB_OK) {
        status = sdb_btree_node_encoded_size(&node, &encoded_size);
    }
    if (status == SDB_OK && encoded_size <= payload_capacity) {
        status = sdb_btree_stage(changes, page_id, &node);
    } else if (status == SDB_OK && node.count < 2U) {
        status = SDB_E_BUFFER_TOO_SMALL;
    } else if (status == SDB_OK && node.kind == SDB_BTREE_LEAF) {
        status = sdb_btree_split_leaf(
            changes, page_id, &node, split_out
        );
    } else if (status == SDB_OK) {
        status = sdb_btree_split_internal(
            changes, page_id, &node, split_out
        );
    }
    sdb_btree_node_destroy(&node);
    return status;
}

sdb_status sdb_btree_create(sdb_pager *pager, sdb_btree *tree_out)
{
    sdb_btree_node root;
    sdb_superblock_v1 next;
    uint8_t payload[SDB_BTREE_NODE_HEADER_SIZE];
    size_t payload_size;
    uint64_t root_page;
    sdb_status status;
    if (pager == NULL || !pager->open || pager->needs_recovery
        || pager->superblock.root_page != 0U || tree_out == NULL) {
        return SDB_E_INVALID_ARGUMENT;
    }
    status = sdb_pager_allocate(pager, &root_page);
    if (status != SDB_OK) {
        return status;
    }
    (void)memset(&root, 0, sizeof(root));
    root.kind = SDB_BTREE_LEAF;
    status = sdb_btree_node_encode(
        &root, payload, sizeof(payload), &payload_size
    );
    if (status == SDB_OK) {
        status = sdb_pager_write(
            pager,
            root_page,
            (uint16_t)SDB_PAGE_TYPE_DATA,
            pager->superblock.checkpoint_lsn,
            payload,
            payload_size
        );
    }
    if (status == SDB_OK) {
        next = pager->superblock;
        next.root_page = root_page;
        if (next.generation == UINT64_MAX) {
            status = SDB_E_OVERFLOW;
        } else {
            ++next.generation;
            status = sdb_pager_store_superblock(pager, &next);
        }
    }
    if (status == SDB_OK) {
        tree_out->pager = pager;
        tree_out->root_page = root_page;
        tree_out->open = true;
    } else {
        (void)sdb_pager_free(pager, root_page);
    }
    return status;
}

sdb_status sdb_btree_open(sdb_pager *pager, sdb_btree *tree_out)
{
    sdb_btree tree;
    sdb_btree_node root;
    sdb_status status;
    if (pager == NULL || !pager->open || pager->needs_recovery
        || pager->superblock.root_page == 0U || tree_out == NULL) {
        return SDB_E_INVALID_ARGUMENT;
    }
    tree.pager = pager;
    tree.root_page = pager->superblock.root_page;
    tree.open = true;
    status = sdb_btree_read_node(&tree, tree.root_page, &root);
    if (status != SDB_OK) {
        return status;
    }
    sdb_btree_node_destroy(&root);
    *tree_out = tree;
    return SDB_OK;
}

sdb_status sdb_btree_get(
    sdb_btree *tree,
    const uint8_t *key,
    size_t key_size,
    uint8_t *value_out,
    size_t value_capacity,
    size_t *value_size_out
)
{
    uint64_t page_id;
    if (tree == NULL || !tree->open || key == NULL || key_size == 0U
        || key_size > (size_t)UINT16_MAX || value_size_out == NULL
        || (value_out == NULL && value_capacity != 0U)) {
        return SDB_E_INVALID_ARGUMENT;
    }
    page_id = tree->root_page;
    {
        size_t depth = 0U;
    for (;;) {
        sdb_btree_node node;
        size_t position;
        sdb_status status;

        if (depth >= 64U) {
            return SDB_E_CORRUPT;
        }
        ++depth;
        status = sdb_btree_read_node(tree, page_id, &node);
        if (status != SDB_OK) {
            return status;
        }
        position = sdb_btree_lower_bound(&node, key, key_size);
        if (node.kind == SDB_BTREE_LEAF) {
            if (position == node.count
                || sdb_btree_compare(
                    node.entries[position].key,
                    node.entries[position].key_size,
                    key,
                    key_size
                ) != 0) {
                sdb_btree_node_destroy(&node);
                return SDB_E_NOT_FOUND;
            }
            *value_size_out = node.entries[position].value_size;
            if (value_capacity < node.entries[position].value_size) {
                sdb_btree_node_destroy(&node);
                return SDB_E_BUFFER_TOO_SMALL;
            }
            if (node.entries[position].value_size != 0U) {
                (void)memcpy(
                    value_out,
                    node.entries[position].value,
                    node.entries[position].value_size
                );
            }
            sdb_btree_node_destroy(&node);
            return SDB_OK;
        }
        if (position < node.count
            && sdb_btree_compare(
                node.entries[position].key,
                node.entries[position].key_size,
                key,
                key_size
            ) == 0) {
            ++position;
        }
        page_id = position == 0U
            ? node.first_child : node.entries[position - 1U].right_child;
        sdb_btree_node_destroy(&node);
    }
    }
}

sdb_status sdb_btree_batch_begin(
    sdb_btree *tree, sdb_btree_batch *batch_out
)
{
    sdb_status status;
    if (tree == NULL || !tree->open || batch_out == NULL) {
        return SDB_E_INVALID_ARGUMENT;
    }
    (void)memset(batch_out, 0, sizeof(*batch_out));
    status = sdb_txn_begin(tree->pager, &batch_out->txn);
    if (status != SDB_OK) {
        return status;
    }
    batch_out->tree = tree;
    batch_out->failure = SDB_OK;
    batch_out->active = true;
    return SDB_OK;
}

sdb_status sdb_btree_batch_get(
    sdb_btree_batch *batch,
    const uint8_t *key,
    size_t key_size,
    uint8_t *value_out,
    size_t value_capacity,
    size_t *value_size_out
)
{
    uint64_t page_id;
    if (batch == NULL || !batch->active || batch->failure != SDB_OK
        || key == NULL || key_size == 0U
        || key_size > (size_t)UINT16_MAX || value_size_out == NULL
        || (value_out == NULL && value_capacity != 0U)) {
        return SDB_E_INVALID_ARGUMENT;
    }
    page_id = batch->tree->root_page;
    {
    size_t depth = 0U;
    for (;;) {
        sdb_btree_node node;
        size_t position;
        sdb_status status;
        if (depth >= 64U) {
            return SDB_E_CORRUPT;
        }
        ++depth;
        status = sdb_btree_batch_read_node(batch, page_id, &node);
        if (status != SDB_OK) {
            return status;
        }
        position = sdb_btree_lower_bound(&node, key, key_size);
        if (node.kind == SDB_BTREE_LEAF) {
            if (position == node.count
                || sdb_btree_compare(
                    node.entries[position].key,
                    node.entries[position].key_size,
                    key,
                    key_size
                ) != 0) {
                sdb_btree_node_destroy(&node);
                return SDB_E_NOT_FOUND;
            }
            *value_size_out = node.entries[position].value_size;
            if (value_capacity < node.entries[position].value_size) {
                sdb_btree_node_destroy(&node);
                return SDB_E_BUFFER_TOO_SMALL;
            }
            if (node.entries[position].value_size != 0U) {
                (void)memcpy(
                    value_out,
                    node.entries[position].value,
                    node.entries[position].value_size
                );
            }
            sdb_btree_node_destroy(&node);
            return SDB_OK;
        }
        if (position < node.count
            && sdb_btree_compare(
                node.entries[position].key,
                node.entries[position].key_size,
                key,
                key_size
            ) == 0) {
            ++position;
        }
        page_id = position == 0U
            ? node.first_child : node.entries[position - 1U].right_child;
        sdb_btree_node_destroy(&node);
    }
    }
}

sdb_status sdb_btree_batch_put(
    sdb_btree_batch *changes,
    const uint8_t *key,
    size_t key_size,
    const uint8_t *value,
    size_t value_size
)
{
    sdb_btree_split root_split;
    sdb_status status;
    if (changes == NULL || !changes->active
        || changes->failure != SDB_OK
        || key == NULL || key_size == 0U
        || key_size > (size_t)UINT16_MAX
        || (value == NULL && value_size != 0U)
        || value_size > (size_t)UINT32_MAX) {
        return SDB_E_INVALID_ARGUMENT;
    }
    (void)memset(&root_split, 0, sizeof(root_split));
    status = sdb_btree_insert_recursive(
        changes,
        changes->tree->root_page,
        key,
        key_size,
        value,
        value_size,
        0U,
        &root_split
    );
    if (status == SDB_OK && root_split.split) {
        sdb_btree_mutation *left_at_root = sdb_btree_find_staged(
            changes, changes->tree->root_page
        );
        sdb_btree_node left;
        sdb_btree_node new_root;
        sdb_btree_entry root_entry;
        uint64_t left_page = 0U;
        bool left_decoded = false;
        if (left_at_root == NULL) {
            status = SDB_E_INTERNAL;
        } else {
            status = sdb_btree_node_decode(
                left_at_root->payload, left_at_root->payload_size, &left
            );
            left_decoded = status == SDB_OK;
        }
        if (status == SDB_OK) {
            status = sdb_txn_allocate(&changes->txn, &left_page);
        }
        if (status == SDB_OK) {
            status = sdb_btree_stage(changes, left_page, &left);
        }
        if (status == SDB_OK) {
            (void)memset(&root_entry, 0, sizeof(root_entry));
            root_entry.key = root_split.separator;
            root_entry.key_size = root_split.separator_size;
            root_entry.right_child = root_split.right_page;
            (void)memset(&new_root, 0, sizeof(new_root));
            new_root.kind = SDB_BTREE_INTERNAL;
            new_root.first_child = left_page;
            new_root.entries = &root_entry;
            new_root.count = 1U;
            status = sdb_btree_stage(
                changes, changes->tree->root_page, &new_root
            );
        }
        if (left_decoded) {
            sdb_btree_node_destroy(&left);
        }
    }
    free(root_split.separator);
    if (status != SDB_OK) {
        changes->failure = status;
    }
    return status;
}

sdb_status sdb_btree_batch_delete(
    sdb_btree_batch *batch, const uint8_t *key, size_t key_size
)
{
    uint64_t page_id;
    uint64_t path_pages[64];
    size_t path_children[64];
    size_t depth = 0U;
    if (batch == NULL || !batch->active || batch->failure != SDB_OK
        || key == NULL || key_size == 0U
        || key_size > (size_t)UINT16_MAX) {
        return SDB_E_INVALID_ARGUMENT;
    }
    page_id = batch->tree->root_page;
    for (;;) {
        sdb_btree_node node;
        size_t position;
        sdb_status status = sdb_btree_batch_read_node(
            batch, page_id, &node
        );
        if (status != SDB_OK) {
            batch->failure = status;
            return status;
        }
        position = sdb_btree_lower_bound(&node, key, key_size);
        if (node.kind == SDB_BTREE_LEAF) {
            uint64_t right_sibling;
            if (position == node.count
                || sdb_btree_compare(
                    node.entries[position].key,
                    node.entries[position].key_size,
                    key,
                    key_size
                ) != 0) {

                sdb_btree_node_destroy(&node);
                return SDB_E_NOT_FOUND;
            }
            free(node.entries[position].key);
            free(node.entries[position].value);
            (void)memmove(
                &node.entries[position],
                &node.entries[position + 1U],
                (node.count - position - 1U) * sizeof(*node.entries)
            );
            --node.count;
            right_sibling = node.right_sibling;
            status = sdb_btree_stage(batch, page_id, &node);
            sdb_btree_node_destroy(&node);
            if (status != SDB_OK) {
                batch->failure = status;
                return status;
            }

            if (depth > 0U
                && page_id != batch->tree->root_page
                && !sdb_btree_txn_allocated(batch, page_id)) {
                sdb_btree_node check;
                status = sdb_btree_batch_read_node(
                    batch, page_id, &check
                );
                if (status == SDB_OK) {
                    bool empty = check.count == 0U;
                    sdb_btree_node_destroy(&check);
                    if (empty) {
                        status = sdb_btree_reclaim_empty_leaf(
                            batch,
                            page_id,
                            right_sibling,
                            path_pages,
                            path_children,
                            depth
                        );
                    }
                }
                if (status != SDB_OK) {
                    batch->failure = status;
                }
            }
            return status;
        }
        if (position < node.count
            && sdb_btree_compare(
                node.entries[position].key,
                node.entries[position].key_size,
                key,
                key_size
            ) == 0) {
            ++position;
        }
        if (depth >= 64U) {
            sdb_btree_node_destroy(&node);
            batch->failure = SDB_E_CORRUPT;
            return SDB_E_CORRUPT;
        }
        path_pages[depth] = page_id;
        path_children[depth] = position;
        ++depth;
        page_id = position == 0U
            ? node.first_child : node.entries[position - 1U].right_child;
        sdb_btree_node_destroy(&node);
    }
}

sdb_status sdb_btree_batch_commit(sdb_btree_batch *batch)
{
    sdb_status status;
    size_t index;
    if (batch == NULL || !batch->active) {
        return SDB_E_INVALID_ARGUMENT;
    }
    if (batch->failure != SDB_OK) {
        status = batch->failure;
        sdb_btree_batch_abort(batch);
        return status;
    }
    if (batch->count == 0U) {
        sdb_btree_batch_abort(batch);
        return SDB_OK;
    }
    status = SDB_OK;
    for (index = 0U; status == SDB_OK && index < batch->count; ++index) {
        status = sdb_txn_put(
            &batch->txn,
            batch->items[index].page_id,
            (uint16_t)SDB_PAGE_TYPE_DATA,
            batch->items[index].payload,
            batch->items[index].payload_size
        );
    }
    if (status == SDB_OK) {
        status = sdb_txn_commit(&batch->txn);
    } else {
        sdb_txn_abort(&batch->txn);
    }
    sdb_btree_changes_destroy(batch);
    return status;
}

void sdb_btree_batch_abort(sdb_btree_batch *batch)
{
    if (batch == NULL) {
        return;
    }
    if (batch->txn.active) {
        sdb_txn_abort(&batch->txn);
    }
    sdb_btree_changes_destroy(batch);
}

sdb_status sdb_btree_put(
    sdb_btree *tree,
    const uint8_t *key,
    size_t key_size,
    const uint8_t *value,
    size_t value_size
)
{
    sdb_btree_batch batch;
    sdb_status status;
    (void)memset(&batch, 0, sizeof(batch));
    status = sdb_btree_batch_begin(tree, &batch);
    if (status == SDB_OK) {
        status = sdb_btree_batch_put(
            &batch, key, key_size, value, value_size
        );
    }
    if (status == SDB_OK) {
        return sdb_btree_batch_commit(&batch);
    }
    if (batch.active) {
        sdb_btree_batch_abort(&batch);
    }
    return status;
}

sdb_status sdb_btree_delete(
    sdb_btree *tree, const uint8_t *key, size_t key_size
)
{
    sdb_btree_batch batch;
    sdb_status status;
    (void)memset(&batch, 0, sizeof(batch));
    status = sdb_btree_batch_begin(tree, &batch);
    if (status == SDB_OK) {
        status = sdb_btree_batch_delete(&batch, key, key_size);
    }
    if (status == SDB_OK) {
        return sdb_btree_batch_commit(&batch);
    }
    if (batch.active) {
        sdb_btree_batch_abort(&batch);
    }
    return status;
}

static sdb_status sdb_btree_cursor_descend(
    sdb_btree *tree,
    const uint8_t *key,
    size_t key_size,
    bool seek,
    bool advance_empty,
    sdb_btree_cursor *cursor
)
{
    uint64_t page_id = tree->root_page;
    size_t depth = 0U;
    for (;;) {
        sdb_btree_node node;
        size_t position = 0U;
        sdb_status status;
        if (depth >= 64U) {
            return SDB_E_CORRUPT;
        }
        ++depth;
        status = sdb_btree_read_node(tree, page_id, &node);
        if (status != SDB_OK) {
            return status;
        }
        if (seek) {
            position = sdb_btree_lower_bound(&node, key, key_size);
        }
        if (node.kind == SDB_BTREE_LEAF) {
            cursor->tree = tree;
            cursor->leaf = node;
            cursor->page_id = page_id;
            cursor->index = position;
            cursor->valid = position < node.count;
            /*
             * Forward seek/first: an empty tail hands off to the next leaf via
             * the right-sibling link. A reverse seek-to-end must NOT do this —
             * the sibling hop does not update the descent path, so a later
             * _prev would read a stale path and skip a whole leaf. It passes
             * advance_empty=false and instead stays put (index==count) so _prev
             * steps back to this leaf's greatest key.
             */
            if (advance_empty && !cursor->valid && node.right_sibling != 0U) {
                return sdb_btree_cursor_next(cursor);
            }
            return SDB_OK;
        }
        if (seek && position < node.count
            && sdb_btree_compare(
                node.entries[position].key,
                node.entries[position].key_size,
                key,
                key_size
            ) == 0) {
            ++position;
        }
        /*
         * Record the descent so a later _prev can step to the predecessor leaf
         * (reverse traversal reads this; forward first/seek/next ignore it).
         */
        cursor->path[cursor->depth].page_id = page_id;
        cursor->path[cursor->depth].child_index = position;
        cursor->depth += 1U;
        page_id = position == 0U
            ? node.first_child : node.entries[position - 1U].right_child;
        sdb_btree_node_destroy(&node);
    }
}

sdb_status sdb_btree_cursor_first(
    sdb_btree *tree, sdb_btree_cursor *cursor_out
)
{
    if (tree == NULL || !tree->open || cursor_out == NULL) {
        return SDB_E_INVALID_ARGUMENT;
    }
    (void)memset(cursor_out, 0, sizeof(*cursor_out));
    return sdb_btree_cursor_descend(
        tree, NULL, 0U, false, true, cursor_out
    );
}

sdb_status sdb_btree_cursor_seek(
    sdb_btree *tree,
    const uint8_t *key,
    size_t key_size,
    sdb_btree_cursor *cursor_out
)
{
    if (tree == NULL || !tree->open || key == NULL || key_size == 0U
        || key_size > (size_t)UINT16_MAX || cursor_out == NULL) {
        return SDB_E_INVALID_ARGUMENT;
    }
    (void)memset(cursor_out, 0, sizeof(*cursor_out));
    return sdb_btree_cursor_descend(
        tree, key, key_size, true, true, cursor_out
    );
}

sdb_status sdb_btree_cursor_seek_floor(
    sdb_btree *tree,
    const uint8_t *key,
    size_t key_size,
    sdb_btree_cursor *cursor_out
)
{
    if (tree == NULL || !tree->open || key == NULL || key_size == 0U
        || key_size > (size_t)UINT16_MAX || cursor_out == NULL) {
        return SDB_E_INVALID_ARGUMENT;
    }
    (void)memset(cursor_out, 0, sizeof(*cursor_out));
    /*
     * Like _seek but never hops to the right sibling on an empty tail, so the
     * descent path stays consistent for a following _prev (reverse start). The
     * cursor may land invalid (index == count); _prev steps it back.
     */
    return sdb_btree_cursor_descend(
        tree, key, key_size, true, false, cursor_out
    );
}

sdb_status sdb_btree_cursor_read(
    const sdb_btree_cursor *cursor,
    uint8_t *key_out,
    size_t key_capacity,
    size_t *key_size_out,
    uint8_t *value_out,
    size_t value_capacity,
    size_t *value_size_out
)
{
    const sdb_btree_entry *entry;
    if (cursor == NULL || !cursor->valid
        || cursor->index >= cursor->leaf.count
        || key_size_out == NULL || value_size_out == NULL
        || (key_out == NULL && key_capacity != 0U)
        || (value_out == NULL && value_capacity != 0U)) {
        return SDB_E_INVALID_ARGUMENT;
    }
    entry = &cursor->leaf.entries[cursor->index];
    *key_size_out = entry->key_size;
    *value_size_out = entry->value_size;
    if (key_capacity < entry->key_size
        || value_capacity < entry->value_size) {
        return SDB_E_BUFFER_TOO_SMALL;
    }
    if (entry->key_size != 0U) {
        (void)memcpy(key_out, entry->key, entry->key_size);
    }
    if (entry->value_size != 0U) {
        (void)memcpy(value_out, entry->value, entry->value_size);
    }
    return SDB_OK;
}

sdb_status sdb_btree_cursor_next(sdb_btree_cursor *cursor)
{
    if (cursor == NULL || cursor->tree == NULL) {
        return SDB_E_INVALID_ARGUMENT;
    }
    if (cursor->valid) {
        ++cursor->index;
        if (cursor->index < cursor->leaf.count) {
            return SDB_OK;
        }
    }
    {
        uint64_t hops = 0U;
        const uint64_t limit = cursor->tree->pager->superblock.next_page_id;
        for (;;) {
            const uint64_t sibling = cursor->leaf.right_sibling;
            sdb_btree_node next;
            sdb_status status;
            if (sibling == 0U) {
                cursor->valid = false;
                return SDB_OK;
            }
            if (sibling == cursor->page_id || ++hops >= limit) {
                cursor->valid = false;
                return SDB_E_CORRUPT;
            }
            status = sdb_btree_read_node(cursor->tree, sibling, &next);
            if (status != SDB_OK) {
                cursor->valid = false;
                return status;
            }
            if (next.kind != SDB_BTREE_LEAF) {
                sdb_btree_node_destroy(&next);
                cursor->valid = false;
                return SDB_E_CORRUPT;
            }
            sdb_btree_node_destroy(&cursor->leaf);
            cursor->leaf = next;
            cursor->page_id = sibling;
            cursor->index = 0U;
            if (next.count != 0U) {
                cursor->valid = true;
                return SDB_OK;
            }
            cursor->valid = false;
        }
    }
}

/*
 * Descend from `page_id` always taking the rightmost child, pushing each
 * internal node onto cursor->path, and land on the greatest entry of the
 * rightmost leaf. cursor->leaf must already be released by the caller.
 */
static sdb_status sdb_btree_cursor_descend_last(
    sdb_btree_cursor *cursor, uint64_t page_id
)
{
    for (;;) {
        sdb_btree_node node;
        sdb_status status;
        if (cursor->depth >= 64U) {
            cursor->valid = false;
            return SDB_E_CORRUPT;
        }
        status = sdb_btree_read_node(cursor->tree, page_id, &node);
        if (status != SDB_OK) {
            cursor->valid = false;
            return status;
        }
        if (node.kind == SDB_BTREE_LEAF) {
            cursor->leaf = node;
            cursor->page_id = page_id;
            cursor->index = node.count > 0U ? node.count - 1U : 0U;
            cursor->valid = node.count > 0U;
            return SDB_OK;
        }
        /*
         * Internal node: the rightmost child is child_index == count
         * (entries[count-1].right_child), or first_child when empty.
         */
        cursor->path[cursor->depth].page_id = page_id;
        cursor->path[cursor->depth].child_index = node.count;
        cursor->depth += 1U;
        page_id = node.count == 0U
            ? node.first_child : node.entries[node.count - 1U].right_child;
        sdb_btree_node_destroy(&node);
    }
}

sdb_status sdb_btree_cursor_last(
    sdb_btree *tree, sdb_btree_cursor *cursor_out
)
{
    if (tree == NULL || !tree->open || cursor_out == NULL) {
        return SDB_E_INVALID_ARGUMENT;
    }
    (void)memset(cursor_out, 0, sizeof(*cursor_out));
    cursor_out->tree = tree;
    return sdb_btree_cursor_descend_last(cursor_out, tree->root_page);
}

sdb_status sdb_btree_cursor_prev(sdb_btree_cursor *cursor)
{
    if (cursor == NULL || cursor->tree == NULL) {
        return SDB_E_INVALID_ARGUMENT;
    }
    /*
     * index > 0 covers both a positioned cursor stepping back and a cursor a
     * seek left past the end of this leaf (index == count): index - 1 is the
     * last real entry either way.
     */
    if (cursor->index > 0U) {
        cursor->index -= 1U;
        cursor->valid = true;
        return SDB_OK;
    }
    /*
     * At the first entry of the current leaf: walk back up the remembered path
     * to the nearest ancestor with a left-hand child not yet visited, then
     * descend that subtree's rightmost path to its greatest entry.
     */
    while (cursor->depth > 0U) {
        sdb_btree_path_entry *entry = &cursor->path[cursor->depth - 1U];
        if (entry->child_index > 0U) {
            sdb_btree_node node;
            uint64_t child_page;
            size_t new_index;
            sdb_status status = sdb_btree_read_node(
                cursor->tree, entry->page_id, &node
            );
            if (status != SDB_OK) {
                cursor->valid = false;
                return status;
            }
            new_index = entry->child_index - 1U;
            child_page = new_index == 0U
                ? node.first_child
                : node.entries[new_index - 1U].right_child;
            entry->child_index = new_index;
            sdb_btree_node_destroy(&node);
            /*
             * Release the current leaf before descending onto a new one; the
             * path above `entry` stays and the descent extends it below.
             */
            sdb_btree_node_destroy(&cursor->leaf);
            (void)memset(&cursor->leaf, 0, sizeof(cursor->leaf));
            return sdb_btree_cursor_descend_last(cursor, child_page);
        }
        /* This ancestor's leftmost child was the path we came down; pop it. */
        cursor->depth -= 1U;
    }
    /* No ancestor had an unvisited left child: we were at the first entry. */
    cursor->valid = false;
    return SDB_OK;
}

void sdb_btree_cursor_close(sdb_btree_cursor *cursor)
{
    if (cursor != NULL) {
        sdb_btree_node_destroy(&cursor->leaf);
        (void)memset(cursor, 0, sizeof(*cursor));
    }
}

typedef struct sdb_btree_verify_context {
    sdb_btree *tree;
    bool *visited;
    size_t visited_count;
    uint64_t previous_leaf_sibling;
    bool have_previous_leaf;
    uint32_t leaf_depth;
    bool have_leaf_depth;
    sdb_btree_verify_result result;
} sdb_btree_verify_context;

static bool sdb_btree_key_at_or_above(
    const uint8_t *key,
    size_t key_size,
    const uint8_t *bound,
    size_t bound_size
)
{
    return bound == NULL
        || sdb_btree_compare(key, key_size, bound, bound_size) >= 0;
}

static bool sdb_btree_key_below(
    const uint8_t *key,
    size_t key_size,
    const uint8_t *bound,
    size_t bound_size
)
{
    return bound == NULL
        || sdb_btree_compare(key, key_size, bound, bound_size) < 0;
}

static sdb_status sdb_btree_verify_node(
    sdb_btree_verify_context *context,
    uint64_t page_id,
    uint32_t depth,
    const uint8_t *lower,
    size_t lower_size,
    const uint8_t *upper,
    size_t upper_size
)
{
    sdb_btree_node node;
    size_t index;
    sdb_status status;
    if (page_id == 0U || page_id >= (uint64_t)context->visited_count
        || depth > 64U || context->visited[(size_t)page_id]) {
        return SDB_E_CORRUPT;
    }
    context->visited[(size_t)page_id] = true;
    status = sdb_btree_read_node(context->tree, page_id, &node);
    if (status != SDB_OK) {
        return status;
    }
    ++context->result.node_count;
    for (index = 0U; index < node.count; ++index) {
        if (!sdb_btree_key_at_or_above(
                node.entries[index].key,
                node.entries[index].key_size,
                lower,
                lower_size
            )
            || !sdb_btree_key_below(
                node.entries[index].key,
                node.entries[index].key_size,
                upper,
                upper_size
            )) {
            sdb_btree_node_destroy(&node);
            return SDB_E_CORRUPT;
        }
    }
    if (node.kind == SDB_BTREE_LEAF) {
        if (context->have_previous_leaf
            && context->previous_leaf_sibling != page_id) {
            sdb_btree_node_destroy(&node);
            return SDB_E_CORRUPT;
        }
        if (context->have_leaf_depth && context->leaf_depth != depth) {
            sdb_btree_node_destroy(&node);
            return SDB_E_CORRUPT;
        }
        context->have_leaf_depth = true;
        context->leaf_depth = depth;
        context->have_previous_leaf = true;
        context->previous_leaf_sibling = node.right_sibling;
        ++context->result.leaf_count;
        context->result.entry_count += (uint64_t)node.count;
        sdb_btree_node_destroy(&node);
        return SDB_OK;
    }
    if (node.count == 0U || depth == UINT32_MAX) {
        sdb_btree_node_destroy(&node);
        return SDB_E_CORRUPT;
    }
    status = sdb_btree_verify_node(
        context,
        node.first_child,
        depth + 1U,
        lower,
        lower_size,
        node.entries[0].key,
        node.entries[0].key_size
    );
    for (index = 0U; status == SDB_OK && index < node.count; ++index) {
        status = sdb_btree_verify_node(
            context,
            node.entries[index].right_child,
            depth + 1U,
            node.entries[index].key,
            node.entries[index].key_size,
            index + 1U < node.count
                ? node.entries[index + 1U].key : upper,
            index + 1U < node.count
                ? node.entries[index + 1U].key_size : upper_size
        );
    }
    sdb_btree_node_destroy(&node);
    return status;
}

static sdb_status sdb_btree_verify_internal(
    sdb_btree *tree,
    sdb_btree_verify_result *result_out,
    bool *reachable_pages,
    size_t reachable_page_count
)
{
    sdb_btree_verify_context context;
    uint64_t file_size;
    uint64_t minimum_size;
    size_t visited_count;
    sdb_status status;
    if (tree == NULL || !tree->open || result_out == NULL
        || tree->pager->superblock.next_page_id == 0U
        || tree->pager->superblock.next_page_id > (uint64_t)SIZE_MAX
        || (reachable_pages != NULL
            && reachable_page_count
                < (size_t)tree->pager->superblock.next_page_id)
        || (reachable_pages == NULL && reachable_page_count != 0U)) {
        return SDB_E_INVALID_ARGUMENT;
    }
    status = sdb_file_size(&tree->pager->file, &file_size);
    if (status != SDB_OK) {
        return status;
    }
    if (tree->pager->superblock.next_page_id - 1U
        > (UINT64_MAX
            - (uint64_t)(SDB_SUPERBLOCK_SLOT_SIZE
                * SDB_SUPERBLOCK_SLOT_COUNT))
            / (uint64_t)tree->pager->superblock.page_size) {
        return SDB_E_CORRUPT;
    }
    minimum_size =
        (uint64_t)(SDB_SUPERBLOCK_SLOT_SIZE * SDB_SUPERBLOCK_SLOT_COUNT)
        + ((tree->pager->superblock.next_page_id - 1U)
            * (uint64_t)tree->pager->superblock.page_size);
    if (file_size < minimum_size) {
        return SDB_E_TRUNCATED;
    }
    visited_count = (size_t)tree->pager->superblock.next_page_id;
    (void)memset(&context, 0, sizeof(context));
    context.tree = tree;
    context.visited = reachable_pages;
    if (context.visited != NULL) {
        (void)memset(
            context.visited, 0, visited_count * sizeof(*context.visited)
        );
    } else {
        context.visited = (bool *)calloc(
            visited_count, sizeof(*context.visited)
        );
    }
    context.visited_count = visited_count;
    if (context.visited == NULL) {
        return SDB_E_OUT_OF_MEMORY;
    }
    status = sdb_btree_verify_node(
        &context, tree->root_page, 1U, NULL, 0U, NULL, 0U
    );
    if (status == SDB_OK && context.previous_leaf_sibling != 0U) {
        status = SDB_E_CORRUPT;
    }
    if (status == SDB_OK) {
        context.result.height = context.leaf_depth;
        *result_out = context.result;
    }
    if (reachable_pages == NULL) {
        free(context.visited);
    }
    return status;
}

sdb_status sdb_btree_verify(
    sdb_btree *tree, sdb_btree_verify_result *result_out
)
{
    return sdb_btree_verify_internal(tree, result_out, NULL, 0U);
}

sdb_status sdb_btree_verify_pages(
    sdb_btree *tree,
    sdb_btree_verify_result *result_out,
    bool *reachable_pages,
    size_t reachable_page_count
)
{
    if (reachable_pages == NULL) {
        return SDB_E_INVALID_ARGUMENT;
    }
    return sdb_btree_verify_internal(
        tree, result_out, reachable_pages, reachable_page_count
    );
}
