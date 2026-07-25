# ShibaDB WAL v2

ShibaDB uses a single-transaction redo WAL sidecar named `<database>.wal`.
All integers are little-endian. Reserved bytes must be zero.

## Durability order

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

## Header (64 bytes)

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

## Commit trailer (24 bytes)

| Offset | Size | Field |
|---:|---:|---|
| 0 | 4 | `SCMT` magic |
| 4 | 2 | version (same as header) |
| 6 | 2 | trailer size (`24`) |
| 8 | 8 | transaction ID |
| 16 | 4 | CRC32 of header and all records |
| 20 | 4 | trailer CRC32, calculated with this field zero |

Recovery still accepts WAL v1. In v1, bytes 40..59 must be zero and allocation
state comes from the existing superblock. Writers emit only v2. WAL files are
transient and cleared after checkpoint, so this upgrade does not change the
main database format version.

Before applying any page, recovery validates every record, page checksum/LSN,
page-ID bound, and page-ID uniqueness. Uniqueness uses a bounded hash set, so
validation is O(n) rather than O(n²). A structurally corrupt committed WAL
therefore cannot partially overwrite the database before its final bad record
is discovered.
