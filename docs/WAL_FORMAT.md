# ShibaDB WAL

ShibaDB writes all committed data through a WAL sidecar named
`<database>.wal`. The current writer emits WAL v5; recovery also understands
older bound formats:

- **v1 / v2** — one transaction per WAL file. Legacy shape, still read on
  open so an unclean shutdown from an older build (or a build that hits
  the `sdb_wal_write_committed` path directly) recovers correctly.
- **v3 / v4** — legacy append-only multi-transaction WALs. They remain
  recoverable when the CRC-valid identity header binds them to the database.
- **v5** — current append-only WAL. It extends each v4 frame with the
  database `file_id`, so ownership remains verifiable when the separate
  64-byte header sector is torn.

For plaintext databases an old zero-hole WAL, or a v3/v4 WAL whose identity
header is torn, is rejected as `SDB_E_CORRUPT`: it carries no trustworthy
database identity and replaying it could overwrite the wrong database.
Encrypted legacy WALs may still use the unbound recovery path because every
page must pass AEAD verification with the target database's data key before
any frame is applied.

All integers are little-endian. Reserved bytes must be zero.

## v1 / v2 — single-transaction redo

### Durability order

1. Truncate the previous WAL.
2. Write the 64-byte header and all full-page records.
3. Sync the WAL body.
4. Write the 24-byte commit trailer.
5. Sync the commit trailer.
6. Apply every page to the database file.
7. Sync the database file.
8. Atomically advance the mirrored-superblock checkpoint and the transaction's
   target allocation watermark/freelist head.
9. Truncate and sync the WAL.

A WAL without a completely valid commit trailer is uncommitted and is ignored.
A committed WAL is replayed when its transaction ID is newer than the database
checkpoint. Replaying the same full-page images is idempotent.

### Header (64 bytes)

| Offset | Size | Field |
|---:|---:|---|
| 0 | 4 | `SWAL` magic |
| 4 | 2 | version (`2`) |
| 6 | 2 | header size (`64`) |
| 8 | 8 | transaction ID |
| 16 | 4 | database page size |
| 20 | 4 | record count |
| 24 | 16 | database file ID |
| 40 | 8 | target `next_page_id` |
| 48 | 8 | target freelist head |
| 56 | 4 | reserved zero bytes |
| 60 | 4 | header CRC32, calculated with this field zero |

Each record is an 8-byte page ID followed by one complete checksummed page.
Recovery rejects WAL files larger than 256 MiB before allocating a read
buffer. The byte ceiling is authoritative even when the record-count field is
within its syntactic limit.
Page IDs must be unique, below the target allocation watermark, and the page
LSN must equal the WAL transaction ID. Every page reserved by the transaction
must have a record; a reservation without a staged page image is rejected
before WAL creation.

### Commit trailer (24 bytes)

| Offset | Size | Field |
|---:|---:|---|
| 0 | 4 | `SCMT` magic |
| 4 | 2 | version (same as header) |
| 6 | 2 | trailer size (`24`) |
| 8 | 8 | transaction ID |
| 16 | 4 | CRC32 of header and all records |
| 20 | 4 | trailer CRC32, calculated with this field zero |

Recovery still accepts WAL v1. In v1, bytes 40..59 must be zero and allocation
state comes from the existing superblock. The legacy single-transaction API
emits v2; the normal append-only writer emits v5. WAL
files are transient and cleared after checkpoint, so this upgrade does not
change the main database format version.

Before applying any page, recovery validates every record, page checksum/LSN,
page-ID bound, and page-ID uniqueness. Uniqueness uses a bounded hash set, so
validation is O(n) rather than O(n²). A structurally corrupt committed WAL
therefore cannot partially overwrite the database before its final bad record
is discovered.

## v3 / v4 / v5 — append-only multi-transaction WAL

WAL mode reuses the sidecar file across commits. The file layout is:

```
+--------------------------------------------------------+
| 64-byte identity header (file_id, page size, CRC32)    |
+--------------------------------------------------------+
| Txn 1: frame[0] .. frame[k1-1] | commit-record         |
+--------------------------------------------------------+
| Txn 2: frame[0] .. frame[k2-1] | commit-record         |
+--------------------------------------------------------+
| ...                                                    |
+--------------------------------------------------------+
| Txn N: frame[0] .. frame[kN-1] | commit-record         |
+--------------------------------------------------------+
```

Every txn is one atomic `frames + commit-record` append. The first append also
publishes a CRC-sealed identity header in the leading 64-byte slot. Recovery
validates that header against the target database before walking the tail. WAL
v5 additionally repeats `file_id` in every frame, so a torn header cannot turn
into an unauthenticated cross-database replay. Writers never rewrite committed
transaction blocks in place: a checkpoint truncates the sidecar back to zero
once every committed txn is present in the main data file.

### Frame layouts

v3 uses an 8-byte header:

| Offset | Size | Field |
|---:|---:|---|
| 0 | 8 | page ID |
| 8 | page_size | full checksummed page image |

v4 uses a 24-byte header: page ID, transaction ID, zero-based frame index,
and frame count. v5 uses the same fields followed by the 16-byte database
`file_id`; its page image begins at byte 40. Current recovery uses the
self-described frame count to locate the commit record deterministically and
validates every v5 frame's `file_id` before applying any page.

The page image obeys the ordinary page format: `SPG1` magic, type, page ID,
page LSN, payload, and page CRC32. Recovery requires `page_lsn == txn_id`.
Page IDs within one txn must be unique and strictly less than the txn's
committed `next_page_id`.

### Commit record (44 bytes, shared v3/v4/v5 layout)

| Offset | Size | Field |
|---:|---:|---|
| 0 | 4 | `SCMT` magic |
| 4 | 2 | version (`3`, `4`, or current `5`) |
| 6 | 2 | record size (`44`) |
| 8 | 8 | transaction ID |
| 16 | 4 | frame count for this txn |
| 20 | 8 | target `next_page_id` |
| 28 | 8 | target freelist head |
| 36 | 4 | running CRC32 over this txn's frame bytes |
| 40 | 4 | commit-record CRC32 (this field zero when computing) |

`sdb_wal_encode_commit_rec` produces exactly this layout;
`sdb_wal_decode_commit_rec` verifies both the version stamp and the
self-checksum. The running CRC covers `frame_count * frame_size` bytes
starting at the first frame of this txn, so a torn tail (bytes lost between
`fsync(frames)` and `fsync(commit-record)` or after the commit-record fsync)
is detected by either magic mismatch, size/version mismatch, self-checksum
mismatch, or running-CRC mismatch.

### Durability order per txn

1. Encode `[identity header if first append][frames][commit record]` into one
   staging buffer.
2. Write that buffer at the current WAL tail (or offset zero on first append).
3. `fsync` the WAL once.
4. Publish the new tail/index in memory.

A commit is durable only after step 3 returns. The commit-record CRC and its
running CRC over every frame make a partial/torn staging-buffer write invalid,
so recovery drops the whole transaction. A second pre-commit fsync is not
required: the commit record is never accepted independently of its frames.

### Checkpoint

Checkpoint drains every committed txn (`txn_id > checkpoint_lsn`) into the
main data file, `fsync`s the data file, advances `checkpoint_lsn` in the
mirrored superblock, and truncates the WAL back to zero. The pager runs a
checkpoint when the WAL crosses a size threshold and unconditionally on
`close`, so a clean shutdown always leaves an empty sidecar.

### Recovery: `sdb_wal_recover_all`

On open, recovery scans the sidecar forward and replays every well-formed
committed txn whose `txn_id > checkpoint_lsn`. The scanner:

1. Reads the whole file (size-capped at 256 MiB), validates the identity
   header when present, and selects the frame layout from its version. A torn
   v5 header may be recovered only when the self-describing frames carry the
   matching `file_id`; unsafe unbound plaintext legacy tails are rejected.
2. For v4/v5, reads the first frame's self-described `frame_count` to locate
   the commit record deterministically, then validates every frame index,
   count, transaction ID, and (for v5) `file_id`. The legacy v3 compatibility
   path searches bounded candidate frame counts because v3 frames did not
   contain that metadata.
3. Enforces contiguity: the accepted `txn_id` must equal
   `expected_next = last_accepted_lsn + 1`, starting from
   `checkpoint_lsn + 1`. A gap is a torn tail: recovery stops without
   accepting the out-of-order txn.
4. Enforces monotone allocation: `next_page_id` must be non-shrinking and
   `freelist_page` must be either zero or strictly less than
   `next_page_id`. A violation is a torn tail.
5. Validates every frame (page checksum, page LSN equals txn_id, unique
   page IDs, optional decryption sanity check) and applies the frames to
   the data file if the txn is above the checkpoint.
6. After the loop, syncs the data file exactly once (only if anything was
   applied) and returns the highest accepted `txn_id`, the latest
   `next_page_id`, and the latest `freelist_page`.

Recovery is **all-or-nothing per txn**: a txn either applies every one of
its frames to the data file and is counted in `last_lsn`, or it is dropped
whole. It is **all-committed-below-a-torn-tail** across the file: every txn
before the first torn-tail marker is applied, everything after it (even a
syntactically valid txn that happens to follow trailing garbage) is
discarded.

### v3 fuzz coverage

`fuzz/fuzz_wal_recover.c` feeds arbitrary bytes to both single-txn
`sdb_wal_recover` and multi-txn `sdb_wal_recover_all`, and additionally
builds structured v3 multi-txn images from the fuzz input with independent
corrupt/valid switches on `txn_id`, `frame_count`, `next_page_id`,
`freelist_page`, running CRC, commit CRC, magic bytes, and the tail
truncation offset. Recovery must return a status for every input and must
never leak, crash, or corrupt memory — libFuzzer + ASan + UBSan enforce
that invariant.
