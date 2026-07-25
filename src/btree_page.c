#include "btree_page.h"

#include <stdlib.h>
#include <string.h>

static const uint8_t sdb_btree_magic[4] = {
    (uint8_t)'S', (uint8_t)'B', (uint8_t)'T', (uint8_t)'1'
};

void sdb_btree_node_destroy(sdb_btree_node *node)
{
    size_t index;
    if (node == NULL) {
        return;
    }
    for (index = 0U; index < node->count; ++index) {
        free(node->entries[index].key);
        free(node->entries[index].value);
    }
    free(node->entries);
    (void)memset(node, 0, sizeof(*node));
}

static sdb_status sdb_btree_copy_bytes(
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
        return SDB_E_INTERNAL;
    }
    (void)memcpy(copy, source, size);
    *output = copy;
    return SDB_OK;
}

sdb_status sdb_btree_node_decode(
    const uint8_t *payload, size_t payload_size, sdb_btree_node *node_out
)
{
    sdb_btree_node node;
    const uint8_t *cursor;
    size_t remaining;
    size_t index;
    uint16_t kind;
    uint16_t count;
    if (payload == NULL || node_out == NULL
        || payload_size < SDB_BTREE_NODE_HEADER_SIZE) {
        return SDB_E_INVALID_ARGUMENT;
    }
    if (memcmp(payload, sdb_btree_magic, sizeof(sdb_btree_magic)) != 0) {
        return SDB_E_BAD_MAGIC;
    }
    kind = sdb_read_u16_le(payload + 4U);
    count = sdb_read_u16_le(payload + 6U);
    if ((kind != (uint16_t)SDB_BTREE_LEAF
         && kind != (uint16_t)SDB_BTREE_INTERNAL)
        || sdb_read_u32_le(payload + 8U) != 0U) {
        return SDB_E_CORRUPT;
    }
    (void)memset(&node, 0, sizeof(node));
    node.kind = (sdb_btree_node_kind)kind;
    if (node.kind == SDB_BTREE_LEAF) {
        node.right_sibling = sdb_read_u64_le(payload + 12U);
    } else {
        node.first_child = sdb_read_u64_le(payload + 12U);
        if (node.first_child == 0U) {
            return SDB_E_CORRUPT;
        }
    }
    {
        const size_t minimum_entry_size =
            node.kind == SDB_BTREE_LEAF ? 7U : 13U;
        if ((size_t)count
            > (payload_size - SDB_BTREE_NODE_HEADER_SIZE)
                / minimum_entry_size) {
            return SDB_E_CORRUPT;
        }
    }
    if (count != 0U) {
        node.entries = (sdb_btree_entry *)calloc(
            (size_t)count, sizeof(*node.entries)
        );
        if (node.entries == NULL) {
            return SDB_E_INTERNAL;
        }
    }
    cursor = payload + SDB_BTREE_NODE_HEADER_SIZE;
    remaining = payload_size - SDB_BTREE_NODE_HEADER_SIZE;
    for (index = 0U; index < (size_t)count; ++index) {
        size_t key_size;
        size_t value_size = 0U;
        size_t fixed_size = node.kind == SDB_BTREE_LEAF ? 6U : 12U;
        sdb_status status;
        if (remaining < fixed_size) {
            sdb_btree_node_destroy(&node);
            return SDB_E_CORRUPT;
        }
        key_size = (size_t)sdb_read_u16_le(cursor);
        if (key_size == 0U) {
            sdb_btree_node_destroy(&node);
            return SDB_E_CORRUPT;
        }
        if (node.kind == SDB_BTREE_LEAF) {
            value_size = (size_t)sdb_read_u32_le(cursor + 2U);
        } else if (sdb_read_u16_le(cursor + 2U) != 0U) {
            sdb_btree_node_destroy(&node);
            return SDB_E_CORRUPT;
        }
        if (key_size > remaining - fixed_size
            || value_size > remaining - fixed_size - key_size) {
            sdb_btree_node_destroy(&node);
            return SDB_E_CORRUPT;
        }
        /*
         * Hoist node.count BEFORE the value alloc so a leaf-value
         * malloc failure still lets sdb_btree_node_destroy free the
         * key we just allocated. Without this, an OOM after the key
         * copy but before the value copy leaks the key buffer: the
         * destroy loop uses `count` as its upper bound.
         */
        status = sdb_btree_copy_bytes(
            cursor + fixed_size, key_size, &node.entries[index].key
        );
        if (status != SDB_OK) {
            sdb_btree_node_destroy(&node);
            return status;
        }
        node.entries[index].key_size = key_size;
        node.count = index + 1U;
        if (node.kind == SDB_BTREE_LEAF) {
            status = sdb_btree_copy_bytes(
                cursor + fixed_size + key_size,
                value_size,
                &node.entries[index].value
            );
            if (status != SDB_OK) {
                sdb_btree_node_destroy(&node);
                return status;
            }
        }
        node.entries[index].value_size = value_size;
        if (node.kind == SDB_BTREE_INTERNAL) {
            node.entries[index].right_child = sdb_read_u64_le(cursor + 4U);
            if (node.entries[index].right_child == 0U) {
                sdb_btree_node_destroy(&node);
                return SDB_E_CORRUPT;
            }
        }
        cursor += fixed_size + key_size + value_size;
        remaining -= fixed_size + key_size + value_size;
    }
    if (remaining != 0U) {
        sdb_btree_node_destroy(&node);
        return SDB_E_CORRUPT;
    }
    for (index = 1U; index < node.count; ++index) {
        const sdb_btree_entry *left = &node.entries[index - 1U];
        const sdb_btree_entry *right = &node.entries[index];
        const size_t common =
            left->key_size < right->key_size ? left->key_size : right->key_size;
        const int compared = memcmp(left->key, right->key, common);
        if (compared > 0
            || (compared == 0 && left->key_size >= right->key_size)) {
            sdb_btree_node_destroy(&node);
            return SDB_E_CORRUPT;
        }
    }
    *node_out = node;
    return SDB_OK;
}

sdb_status sdb_btree_node_encoded_size(
    const sdb_btree_node *node, size_t *size_out
)
{
    size_t total = SDB_BTREE_NODE_HEADER_SIZE;
    size_t index;
    if (node == NULL || size_out == NULL
        || (node->kind != SDB_BTREE_LEAF
            && node->kind != SDB_BTREE_INTERNAL)
        || node->count > (size_t)UINT16_MAX
        || (node->kind == SDB_BTREE_INTERNAL && node->first_child == 0U)) {
        return SDB_E_INVALID_ARGUMENT;
    }
    for (index = 0U; index < node->count; ++index) {
        const sdb_btree_entry *entry = &node->entries[index];
        size_t entry_size = node->kind == SDB_BTREE_LEAF ? 6U : 12U;
        if (entry->key == NULL || entry->key_size == 0U
            || entry->key_size > (size_t)UINT16_MAX
            || entry->value_size > (size_t)UINT32_MAX
            || (entry->value == NULL && entry->value_size != 0U)
            || (node->kind == SDB_BTREE_INTERNAL
                && entry->right_child == 0U)
            || !sdb_checked_add_size(entry_size, entry->key_size, &entry_size)
            || !sdb_checked_add_size(
                entry_size,
                node->kind == SDB_BTREE_LEAF ? entry->value_size : 0U,
                &entry_size
            )
            || !sdb_checked_add_size(total, entry_size, &total)) {
            return SDB_E_INVALID_ARGUMENT;
        }
    }
    *size_out = total;
    return SDB_OK;
}

sdb_status sdb_btree_node_encode(
    const sdb_btree_node *node,
    uint8_t *output,
    size_t output_size,
    size_t *written_out
)
{
    size_t required;
    size_t index;
    uint8_t *cursor;
    sdb_status status = sdb_btree_node_encoded_size(node, &required);
    if (status != SDB_OK || output == NULL || written_out == NULL) {
        return status != SDB_OK ? status : SDB_E_INVALID_ARGUMENT;
    }
    if (output_size < required) {
        return SDB_E_BUFFER_TOO_SMALL;
    }
    (void)memset(output, 0, required);
    (void)memcpy(output, sdb_btree_magic, sizeof(sdb_btree_magic));
    sdb_write_u16_le(output + 4U, (uint16_t)node->kind);
    sdb_write_u16_le(output + 6U, (uint16_t)node->count);
    sdb_write_u64_le(
        output + 12U,
        node->kind == SDB_BTREE_LEAF
            ? node->right_sibling : node->first_child
    );
    cursor = output + SDB_BTREE_NODE_HEADER_SIZE;
    for (index = 0U; index < node->count; ++index) {
        const sdb_btree_entry *entry = &node->entries[index];
        sdb_write_u16_le(cursor, (uint16_t)entry->key_size);
        if (node->kind == SDB_BTREE_LEAF) {
            sdb_write_u32_le(cursor + 2U, (uint32_t)entry->value_size);
            (void)memcpy(cursor + 6U, entry->key, entry->key_size);
            if (entry->value_size != 0U) {
                (void)memcpy(
                    cursor + 6U + entry->key_size,
                    entry->value,
                    entry->value_size
                );
            }
            cursor += 6U + entry->key_size + entry->value_size;
        } else {
            sdb_write_u64_le(cursor + 4U, entry->right_child);
            (void)memcpy(cursor + 12U, entry->key, entry->key_size);
            cursor += 12U + entry->key_size;
        }
    }
    *written_out = required;
    return SDB_OK;
}
