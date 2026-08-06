# Pager Invariants

Formal invariants for `src/pager.c` and the storage layers it directly
coordinates (`src/wal.c`, `src/superblock_store.c`, `src/file.c`,
`src/encrypted_page.c`). These are the contracts callers may rely on and
the checks a reviewer should verify are still enforced after any change to
those files.

An invariant is a statement of the form "**at every observable state**, X
holds". "Observable" means a state that can be entered by (a) any public
`sdb_pager_*` / `sdb_txn_*` function returning, or (b) a process crash that
leaves the on-disk state as it was at the last completed durability barrier
(POSIX `fsync` return, Windows `FlushFileBuffers` return).

Invariants are numbered `P.n` for pager-wide, `T.n` for transaction-scoped,
`W.n` for WAL-scoped, `S.n` for superblock-scoped, `C.n` for cache-scoped,
`E.n` for encryption-scoped. Cross-references to source use `file:line`.

## Terminology

- **Physical page N** — the aligned byte range
  `[SDB_SUPERBLOCK_SLOT_SIZE * SDB_SUPERBLOCK_SLOT_COUNT + (N-1)*page_size,
   + page_size)` in the database file. Page 0 is reserved; the first data
  page has `page_id = 1`.
- **Committed superblock** — the `sdb_superblock_v1` mirror slot with the
  highest generation whose CRC and structural fields verify
  (`src/superblock_store.c`, `sdb_superblock_store_read_file`).
- **Committed WAL** — a WAL file whose header, body CRC, and commit trailer
  all validate against the committed superblock's `file_id` and `page_size`
  (`src/wal.c:397-522`).
- **Recovery required** — the `sdb_pager.needs_recovery` flag is `true`.
  While set, the pager refuses every mutation entry point.
- **Data offset** = `SDB_SUPERBLOCK_SLOT_SIZE * SDB_SUPERBLOCK_SLOT_COUNT`.
- **Encryption enabled** — the committed superblock has `SDB_FLAG_ENCRYPTED`
  set. In this mode every physical page except the two superblock slots is
  written as `SDB_PAGE_TYPE_ENCRYPTED` and its plaintext is only produced
  transiently inside the pager (`src/pager.c:498-546`).

## File Layout

**P.1** — The on-disk file consists of exactly two superblock slots at
offsets 0 and `SDB_SUPERBLOCK_SLOT_SIZE`, followed by an integer number of
data pages at `data_offset + (page_id-1) * page_size`. There is no
inter-region gap and no trailing region other than data pages. Enforced at
create (`src/superblock_store.c:120-138`) and after every open/recovery by
`sdb_pager_canonicalize_file_size` (`src/pager.c:69-102`).

**P.2** — After `sdb_pager_open*` returns `SDB_OK`, the file size is exactly
`data_offset + (next_page_id - 1) * page_size`. A larger file at open is
truncated and fsynced; a smaller file causes open to fail with
`SDB_E_TRUNCATED` (`src/pager.c:91-101`).

**P.3** — `page_size` is a power of two in `[SDB_MIN_PAGE_SIZE,
SDB_MAX_PAGE_SIZE]`. Enforced by every encode/decode boundary
(`src/page.c:34-37, 78-80`).

**P.4** — `page_id == 0` is reserved and rejected by every public entry that
takes a page id: `sdb_pager_write` (`src/pager.c:410`), `sdb_pager_read`
(`src/pager.c:471`), `sdb_pager_free` (`src/pager.c:640`), `sdb_txn_put`
(`src/pager.c:727`), `sdb_txn_free` (`src/pager.c:863`).

**P.5** — Every physical page read succeeds only if the page's stored
`page_id` field equals the requested id. Otherwise `SDB_E_CORRUPT` is
returned and the page is evicted from cache (`src/page.c:87-93` +
`src/pager.c:551-553`). This is the anti-swap invariant: a page whose
header claims a different id is treated as corruption, not as valid data
at the wrong location.

## Allocation

**A.1** — Runtime allocation happens exclusively through `sdb_txn_allocate`
inside a `sdb_txn`, whose commit path is atomic per T.3 and T.7. The
public `sdb_pager_allocate` and `sdb_pager_free` refuse to run while
`transaction_active` is set (`src/pager.c:563, 639`), so they can never
overlap with a transaction whose atomicity the WAL protects.

**A.2** — `sdb_pager_allocate` publishes the incremented `next_page_id`
in the superblock **before** it writes the initial page image
(`src/pager.c:614-624`). A crash between the superblock advance and the
initial-page write therefore leaves a physical page whose id is within
the committed `next_page_id` but whose on-disk content is uninitialized
(zero-extended by `ftruncate`) or partial. This is a known bounded
crash window with the following scope:

- The **only** production caller of `sdb_pager_allocate` outside of a
  transaction is `sdb_btree_create` (`src/btree.c:800`), which itself is
  only called from `sdb_database_create` (`src/engine.c:939`). A crash
  in the window therefore only affects database creation, before any
  application data has been written.
- After such a crash, `sdb_database_create` never returned success, so
  no caller ever received a handle to the partially-created database.
  The subsequent recovery path (open the created file, decode the
  advanced superblock, then attempt to decode the never-written root
  page) will fail with `SDB_E_CORRUPT` because `sdb_page_decode` sees
  no magic. The operator must remove the partial file and retry create.
- **No runtime data path is affected.** Every runtime allocation goes
  through `sdb_txn_allocate` and is bundled into the WAL commit of its
  transaction (T.3), so the WAL either replays the entire allocation
  plus its initialization or replays neither.

A future hardening step could reorder `sdb_pager_allocate` to
write-then-publish, but doing so requires either an internal write path
that bypasses the `page_id < next_page_id` guard on the public
`sdb_pager_write` or a rework of that check. The current ordering is
retained until that refactor is undertaken.

**A.3** — `sdb_pager_free` writes the target page as `SDB_PAGE_TYPE_FREE`
(with the previous freelist head chained in its payload) **before**
advancing the superblock's `freelist_page`
(`src/pager.c:660-673`). A crash between the two leaves the page as a
valid FREE page whose content is not yet reachable from the freelist
head, i.e. an orphaned free page. It is not a dangling reference: the
caller only ever calls `sdb_pager_free` after removing the last
reference from higher-level metadata, so no upper layer still points at
the freed id.

## Superblock

**S.1** — At most one of the two slots is written to disk at a time. The
non-selected slot is written first, fsynced, then the currently selected
slot is overwritten and fsynced (`src/superblock_store.c:218-235`). This
guarantees that at any crash point at least one slot remains valid.

**S.2** — On update, the new superblock's `generation` must be strictly
greater than the currently committed one; equal or lower generations are
rejected as `SDB_E_INVALID_ARGUMENT` (`src/superblock_store.c:213-217`).
Combined with S.1 this means `generation` is the tie-breaker at recovery.

**S.3** — On open, the slot with the highest valid generation is selected.
If both mirrors are valid and their generations are equal but their content
differs, both are rejected as split-brain (`src/superblock_store.c:20-45`).
After password authentication for an encrypted database, open rewrites and
fsyncs the peer slot when it is invalid, truncated, or from an older generation.
The handle is not exposed until two identical current mirrors are durable.

**S.4** — `file_id` is a 16-byte identity assigned at create and never
changes across the file's lifetime. It authenticates every encrypted page
and every WAL frame; a mismatch is corruption
(`src/encrypted_page.c:20`, `src/wal.c:407-409, 455`).

**S.5** — `checkpoint_lsn` is monotonic non-decreasing across every
successful pager operation. It advances by exactly one on every
successful `sdb_txn_commit` (`txn_id = checkpoint_lsn + 1`, then
`checkpoint_lsn = txn_id`, `src/pager.c:926, 1003`). It may also advance
during WAL recovery when a committed WAL frame carries a higher
`txn_id` than the current committed value (`src/pager.c:175-177`). It
never wraps because commit rejects `checkpoint_lsn == UINT64_MAX`
(`src/pager.c:904`).

**S.6** — `next_page_id` is monotonic non-decreasing. It increases only in
`sdb_pager_allocate` (when growing the file) or during WAL recovery of a
v2 WAL frame whose target `next_page_id` is greater than the currently
committed value (`src/pager.c:611`, `src/wal.c:489-493`).

**S.7** — When `SDB_FLAG_ENCRYPTED` is set, `key_wrap_id >= 1`,
`kdf_iterations` is in `[SDB_MIN_KDF_ITERATIONS, SDB_MAX_KDF_ITERATIONS]`,
and both `wrapped_key` and `key_wrap_tag` are non-zero. When the flag is
clear, `key_wrap_id == 0`, `kdf_iterations == 0`, `wrapped_key` is all
zero, and `key_wrap_tag` is all zero. `salt` is not zero-checked in the
unencrypted case — it is unused by the pager but part of the on-disk
format (`src/superblock.c:42-67`).

**S.8** — Password rotation (`sdb_pager_rotate_password`) increments
`key_wrap_id` by exactly one and bumps `generation`. It refuses to run
when `key_wrap_id == UINT32_MAX` (`src/pager.c:380-386`). This is the sole
mechanism guaranteeing no wrap-key nonce reuse across rekey operations,
because the key-wrap nonce is deterministically `file_id || key_wrap_id`
(`src/key_manager.c:5-11`).

**S.9** — Every operation that publishes a new superblock bumps
`generation` through `sdb_generation_bump` (`src/pager.c:75-82`), which
returns `SDB_E_OVERFLOW` when the current value equals `UINT64_MAX`
instead of wrapping to zero. Wrapping would silently violate S.2
(monotonicity) and cause S.3 (highest-valid-generation recovery) to
select the wrong mirror after the next split-brain. The one call site
that predates the helper — `sdb_btree_create` (`src/btree.c:819-830`) —
performs the equivalent inline check. `UINT64_MAX` bumps are far beyond
any realistic database lifetime; this is defense in depth against a
runaway loop or corrupted superblock read whose `generation` field is
already at the ceiling.

## Transactions

**T.1** — At most one transaction is active per pager. Enforced by the
`transaction_active` flag which is set in `sdb_txn_begin`
(`src/pager.c:710`) and cleared by `sdb_txn_release` at commit or abort
(`src/pager.c:692`). Every mutation entry point rejects re-entry.

**T.2** — A pager in `needs_recovery` state rejects `sdb_txn_begin` and all
direct mutations (`sdb_pager_write`, `sdb_pager_allocate`,
`sdb_pager_free`). Recovery is not retriable in-place; the caller must
close and re-open (`src/pager.c:701, 405, 562, 638`).

**T.3** — `sdb_txn_commit` writes exactly one WAL commit unit with
`txn_id = committed.checkpoint_lsn + 1`, then applies every staged page to
the database file, then fsyncs, then advances the superblock with
`checkpoint_lsn = txn_id`, then clears the WAL (`src/pager.c:926-1015`).
This is the sole ordering that yields both durability and atomicity
across a crash.

**T.4** — Every page allocated inside a transaction (via `sdb_txn_allocate`)
must be staged as at least one `sdb_txn_put` before commit; otherwise
commit fails with `SDB_E_INVALID_ARGUMENT` and the transaction is
released (`src/pager.c:911-925`). This prevents commits from advancing
`next_page_id` without also authenticating the newly reachable range.

`sdb_txn_free(page_id)` on a page that was allocated earlier
*in the same transaction* is a graceful cancel: the
page is removed from `allocated_pages` and any staged put is unstaged.
No FREE record reaches the WAL for a page that never reached disk. The
slot below `next_page_id` becomes an orphan that `sdb_engine_compact`
reclaims on the next compaction; the invariant above is preserved
because the id no longer appears in `allocated_pages` at commit time.

**T.5** — If any I/O step of commit fails after the WAL has been written,
`needs_recovery` is set to `true` before commit returns
(`src/pager.c:1017-1019`). The next open will run WAL replay and clear the
flag before returning.

**T.6** — `sdb_txn_abort` is total: it never fails, it always releases
transaction memory, and it always clears `transaction_active`
(`src/pager.c:1026-1029`). Callers may abort from any error path without
tracking further state.

**T.7** — A committed transaction is atomic under crash: after any crash
between the WAL commit trailer's `fsync` and the next open, replay
reconstructs the exact state that a successful commit would have left
(`src/wal.c:534-547` gated by `txn_id > superblock.checkpoint_lsn`;
idempotent for `txn_id <= checkpoint_lsn`).

## Write-Ahead Log

**W.1** — The WAL body is written and fsynced before the commit trailer is
written and fsynced (`src/wal.c:222-234`). A torn write between the two
`fsync`s therefore leaves a WAL with an invalid or missing trailer, which
recovery treats as "no committed WAL" (safe drop).

**W.2** — The commit trailer's CRC covers the entire body up to and
including the record area but excluding the trailer itself. The trailer
also has its own zeroed-range CRC over its own bytes. Both must match at
recovery (`src/wal.c:206-213, 518-519`).

**W.3** — A committed WAL binds to its database via `file_id` copied from
the superblock at write time. A WAL whose `file_id` disagrees with the
current superblock is rejected as corruption at open, not silently
ignored (`src/wal.c:407-409, 455`). Consequence: relocating a WAL to
another database's directory cannot cause cross-database replay.

**W.4** — Every WAL record's page image has `page_lsn == txn_id`. Enforced
at write (`src/wal.c:164`) and at replay (`src/wal.c:277-279`). A record
whose page image disagrees with the trailer's txn id is treated as
corruption, not as a valid page from a different transaction.

**W.5** — A WAL has no duplicate `page_id`s across its records. Enforced by
`sdb_wal_id_set_insert` at both write (`src/wal.c:155`) and pre-replay
validation (`src/wal.c:316`). Duplicate detection runs before any page is
applied at replay, so a corrupt WAL cannot half-replay.

**W.6** — The total WAL byte count is capped at `SDB_WAL_MAX_BYTES`
(256 MiB). Larger WAL files are rejected at recovery (`src/wal.c:377-380`)
and the sizing arithmetic overflow-checks against this ceiling at write
(`src/wal.c:136-141`).

**W.7** — Replay is idempotent for any WAL whose `txn_id <=
superblock.checkpoint_lsn` — no page is written to the database file
(`src/wal.c:534`). Running recovery repeatedly is safe.

**W.8** — After a successful commit, the WAL is truncated to zero and
fsynced (`src/pager.c:1015`, `src/wal.c:556-573`). A non-empty WAL after
open therefore always represents a candidate for replay, never leftover
from a previous successful commit.

**W.9** — Parent-directory `fsync` runs at WAL creation on POSIX
(`src/wal.c:102-104`) so that the new WAL entry is durable in the
directory even if the process crashes before writing any WAL body.

## Cache

**C.1** — The page cache is a bounded LRU of `SDB_PAGER_CACHE_CAPACITY`
(64) entries per pager. Eviction is deterministic
(`src/page_cache.c`, entered from `src/pager.c:453, 496, 1009`).

**C.2** — Every cache entry stores a PLAINTEXT-envelope page: the SP1
frame carrying an application-typed payload (SDB_PAGE_TYPE_DATA / WAL /
etc.), never an SDB_PAGE_TYPE_ENCRYPTED (SEN1) envelope. On plaintext
databases the cached bytes equal the on-disk bytes. On encrypted
databases the cached bytes are what the reader would materialise after
decrypt+re-encode; the disk copy remains an SEN1 envelope. A cache hit
therefore skips both the `read` syscall AND the AEAD decrypt
(`src/pager.c:548-561`). Cache hit still verifies the SP1 CRC via
`sdb_page_decode`. This invariant is a performance requirement: without
it, every read of a hot page in encrypted mode would re-run
XChaCha20-Poly1305.

**C.3** — A cache entry that fails to decode is evicted before the error
is returned (`src/pager.c:633-635`). This prevents a poisoned entry from
persisting across retries.

**C.4** — Writes populate the cache with the just-written page's
plaintext envelope after the disk write is durable
(`src/pager.c:472-511, 1198-1249`). On encrypted DBs the write path
holds ciphertext (SEN1) in the `page` buffer at fsync time; a fresh SP1
plaintext envelope is rebuilt from the same
(type, page_id, page_lsn, payload) inputs and cached instead. A
concurrent reader that observes a cache entry sees the plaintext
envelope the disk copy would decrypt to, not the ciphertext bytes
themselves.

## Encryption

**E.1** — In encrypted mode, every physical page (except superblock slots)
on disk has type `SDB_PAGE_TYPE_ENCRYPTED`. A plaintext type outside the
pager's transient decrypt buffer is corruption
(`src/pager.c:505-508, 547-549`).

**E.2** — Every encrypted page's authenticated additional data (AAD) binds
`file_id || page_id || page_lsn || plaintext_type || plaintext_size`
(`src/encrypted_page.c:10-25`). A page swapped, replayed, retyped, or
resized will fail authentication.

**E.3** — Every encrypted page uses a fresh 24-byte random nonce per
encrypt call, sourced from the OS CSPRNG (`src/encrypted_page.c:65-72`,
`src/random.c`). The 192-bit XChaCha20 nonce space makes accidental
collision negligible even across the total lifetime of any realistic
database.

**E.4** — The data key is zeroed on every failure path of open, on close,
and after every internal derivation site
(`src/pager.c:156, 197, 209, 316, 343`). Staged plaintext payloads in a
transaction are secure-zeroed before free when encryption is enabled
(`src/pager.c:682-688`). Transient encrypted-page buffers zero their
scratch space (`src/encrypted_page.c:104-107`) and the AEAD primitive
zeroes every derived subkey, IETF nonce, and first block on both
success and failure paths (`src/xchacha20poly1305.c:452-454, 486-489,
502-505`).

**E.5** — MAC verification uses constant-time comparison before any
plaintext is written to the caller's output buffer
(`src/xchacha20poly1305.c:485-490`). A wrong key or a tampered ciphertext
produces `SDB_E_CORRUPT` (surfaced as `SDB_E_AUTHENTICATION` for key wrap,
`src/key_manager.c:114-116`) without ever revealing a plaintext byte.

**E.6** — The wrapped-key AAD binds `file_id || salt || page_size || flags
|| key_wrap_id || kdf_iterations` (`src/key_manager.c:13-24`). An attacker
who edits the superblock to downgrade `kdf_iterations`, clear the
encryption flag, or reuse a `key_wrap_id` cannot produce a superblock
whose wrap tag still verifies. Current encrypted superblocks additionally
store an HMAC-SHA256 at slot bytes 160..191, keyed by the unwrapped data key
and covering the complete 160-byte header. Open verifies it before replace
recovery, WAL recovery, file-size canonicalization, or root use. This binds
`generation`, `checkpoint_lsn`, `root_page`, `freelist_page`, and
`next_page_id`; CRC32 remains only the pre-authentication damage check.
Legacy encrypted headers are rewrapped and upgraded after successful password
authentication.

**E.7** — **Threat-model scope of CRC32.** The 32-bit CRC on every
page (`src/page.c`, offset 28) is a NON-CRYPTOGRAPHIC checksum. It
detects accidental corruption — bit rot, torn writes, and firmware
glitches — but does NOT provide authenticity: an attacker with
offline write access to the database file can flip arbitrary bytes
and recompute a matching CRC in microseconds. On an **encrypted**
database the AEAD tag on every page envelope (invariant E.2)
authenticates the payload cryptographically and closes this arm;
on a **plaintext** database, page CRC alone is not a defence
against offline tampering. Applications whose threat model
includes an attacker with database-file write access must open the
database in encrypted mode (`SDB_FLAG_ENCRYPTED`).

Encrypted databases also authenticate the superblock control fields with the
keyed header tag described by E.6. Plaintext databases retain CRC32-only damage
detection and therefore do not claim resistance to an offline writer.

## Recovery

**R.1** — Open with password P succeeds only if unwrapping the committed
superblock's wrapped key with a PBKDF2-derived key from P yields a MAC
that verifies. A wrong password returns `SDB_E_AUTHENTICATION` and zeroes
the output data key (`src/key_manager.c:114-121`).

**R.2** — After successful superblock read at open, the pager attempts WAL
recovery. If any I/O step of recovery fails, the pager returns an error
and `sdb_pager_open*` does not produce a usable handle
(`src/pager.c:167-200`, `src/wal.c:544-546`). The pager never returns
`SDB_OK` while `needs_recovery` is still set.

**R.3** — File-size canonicalization runs after WAL recovery, not before.
An abandoned trailing allocation (crashed inside `sdb_pager_allocate`
between file resize and superblock advance) is truncated back to the size
implied by the committed `next_page_id`, then fsynced
(`src/pager.c:91-101`). A file smaller than that implied size fails open.

**R.4** — A hardlinked database file is rejected at open. This is enforced
at the sync/lock layer (`docs/CONCURRENCY.md`) because a single WAL /
replacement sidecar cannot name two inode aliases unambiguously.

## Cross-Cutting

**X.1** — Every entry point that returns `SDB_OK` leaves the pager in a
consistent state per every invariant above. Every entry point that
returns a non-`SDB_OK` status either (a) leaves the pager unchanged, or
(b) sets `needs_recovery` and marks the fact that the caller must close
and reopen before further use.

**X.2** — No pager entry point returns `SDB_OK` after any partial write
without a subsequent successful `fsync` on the affected file. The commit
sequence in `sdb_txn_commit` and the mirrored-slot sequence in
`sdb_superblock_store_update_file` are the two canonical instances.

**X.3** — Every checked arithmetic on `page_id`, offset, and size uses
`sdb_checked_add_size` / `sdb_checked_mul_size` (`src/codec.c:5-21`) or an
explicit overflow check against `UINT64_MAX / page_size` before the
multiplication (`src/pager.c:47, 594, 599, 904-907`,
`src/wal.c:262-263`). No untrusted integer from the disk or from the
public API reaches allocation or seek arithmetic without such a check.

---

## Review checklist

When modifying `src/pager.c`, `src/wal.c`, or `src/superblock_store.c`,
verify that every invariant above still holds. In particular:

- Any new fsync-adjacent write must preserve the body-then-trailer order
  in W.1 and the mirror-then-fsync order in S.1.
- Any new failure path must preserve T.5 (set `needs_recovery` on
  post-WAL failure) and E.4 (zero the data key).
- Any new integer arithmetic touching page ids, offsets, or sizes must be
  checked per X.3.
- Any new page type or superblock field must be authenticated by the
  encrypted-page AAD (E.2) or the wrap-key AAD (E.6), respectively.

Fuzz targets in `fuzz/` should be extended when adding a new decoder that
reads untrusted bytes.
