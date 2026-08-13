# ShibaDB B+Tree node format v1

B+Tree nodes live inside ordinary checksummed DATA pages. Keys are compared as
unsigned byte strings in lexicographic order. Duplicate keys are not stored;
`put` replaces the existing value.

## Node header

| Offset | Size | Field |
|---:|---:|---|
| 0 | 4 | `SBT1` magic |
| 4 | 2 | kind: leaf (`1`) or internal (`2`) |
| 6 | 2 | entry count |
| 8 | 4 | reserved zero bytes |
| 12 | 8 | leaf right sibling or internal first child |

Leaf entries contain a 2-byte key length, 4-byte value length, key bytes, and
value bytes. Internal entries contain a 2-byte key length, 2 reserved zero
bytes, an 8-byte right-child page ID, and key bytes.

All keys in a node must be strictly increasing. Internal keys separate child
ranges. Leaves form a forward linked list used by ordered cursors.

The root has a stable page ID. When it splits, its left image is moved to a new
page and the original root page becomes an internal node. This makes root
splits part of the same atomic multi-page WAL transaction without changing the
superblock root pointer.

Deletion removes leaf entries in place. A leaf that becomes completely empty is
reclaimed within the same WAL transaction: its linked-list predecessor is
relinked past it, its separator is removed from the parent, and the page is
freed. An internal node then left with a single child collapses upward, and an
empty path reaching the root collapses the tree's height through the stable
root page. Non-empty leaves are never merged or redistributed on underflow, so
a sparsely-filled leaf keeps its separator boundaries in place — this preserves
lookup and cursor correctness. Reclaiming that residual slack (compacting
sparse-but-live leaves) is assigned to the compaction milestone.

## Sizing invariants

Callers of `sdb_btree_batch_put` (directly or through the engine layer)
must respect these bounds on individual entry sizes:

- **Key size**: `≤ UINT16_MAX` (65 535 bytes). Enforced by the public
  API guard in `sdb_btree_batch_put` and by the on-disk decoder in
  `sdb_btree_node_decode`.
- **Value size**: `≤ UINT32_MAX` (~4.3 GiB). Enforced by the same
  guards. In practice the engine layer chunks values larger than one
  page's payload capacity into multiple pages, so btree entries seen
  at the leaf level are further bounded by
  `sdb_pager_payload_capacity(pager)`.
- **Combined entry footprint**: on the wire an internal entry
  occupies `12 + key_size` bytes; a leaf entry occupies
  `6 + key_size + value_size` bytes. The split path (`sdb_btree_split_leaf`,
  `sdb_btree_split_internal`) computes these sums in `uint64_t` so
  the arithmetic cannot wrap on 32-bit hosts even at the API-permitted
  bounds; range violations return `SDB_E_OVERFLOW`.
- **Minimum page capacity for internal splits**: `sdb_btree_split_internal`
  requires the source node to hold at least three entries — one for
  the left half, one to promote, one for the right half. In practice
  this means the logical page-payload capacity must be at least
  `3 * (12 + max_key_size)` = ~200 KiB when `key_size` is at its
  maximum. Since the maximum page size is `SDB_MAX_PAGE_SIZE = 64 KiB`,
  a pathologically-large key at max size cannot fit three-per-page.
  Applications should keep keys well under one third of
  `sdb_pager_payload_capacity`; long keys are best hashed at the
  application layer.
