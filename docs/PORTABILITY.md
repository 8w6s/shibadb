# Portability status & known non-Linux issues

ShibaDB is developed and continuously tested on **64-bit little-endian Linux**
(gcc/clang, ASan/TSan/UBSan, fuzzers, crash-injection, dm-flakey power-loss).
The on-disk format is explicitly little-endian (`src/codec.c` `_le` helpers) and
CRC32 is byte-wise (endian-neutral), so files are portable across byte orders.

Windows and macOS code paths are built and tested by the pull-request CI
matrix. Tagged candidates additionally require signed native and soak evidence;
workflow configuration alone is not release evidence. A MinGW
warnings-as-errors cross-build is an independent Windows-header/compiler gate.

## Fixed
- **`src/random.c` — `ULONG_MAX` without `<limits.h>` (MSVC compile break).**
  The `_WIN32` branch used `ULONG_MAX`, which `<windows.h>` does not define.
  Fixed by including `<limits.h>` in that branch. (Compile-only; verified it
  does not affect the Linux build.)
- **Windows process lock.** The mandatory whole-file `LockFileEx` lock was
  replaced by a named semaphore keyed from the database's stable volume/file
  identity. The sidecar path lock remains a byte-range lock. This preserves
  alias exclusion without blocking ShibaDB's own pager handle.
- **Windows replace/compaction.** Writable secondary handles use coordinated
  share flags, existing files are replaced with `ReplaceFileW`, and compaction
  suspends/reopens its source handle around the replacement operation.
- **Windows path and directory handling.** Absolute drive and UNC UTF-8 paths
  are converted to extended-length Win32 paths; drive roots are preserved when
  syncing a parent. Windows exposes no POSIX-style directory fsync:
  `FlushFileBuffers` on a directory handle is unsupported on NTFS
  (`ERROR_INVALID_FUNCTION`/`ERROR_ACCESS_DENIED`/`ERROR_NOT_SUPPORTED`).
  Tolerating those is a deliberate, documented no-op — NTFS `$LogFile` journals
  namespace changes and callers force the swap durable via
  `MOVEFILE_WRITE_THROUGH`/`REPLACEFILE_WRITE_THROUGH` plus `FlushFileBuffers`
  on the file handle (SQLite/LMDB/PostgreSQL do the same). A genuinely bad
  handle (`ERROR_INVALID_HANDLE`) and every other error still propagate as
  `SDB_E_IO`.
- **Windows local verification (2026-08-02).** MinGW compiled every core,
  shared-library, and test target with warnings as errors. All 48 applicable
  native C tests passed sequentially under Wine, including recovery,
  encryption, concurrency, compaction, reclaim, UTF-8, and extended path
  conversion. This is useful compatibility evidence, not native Windows or
  signed release evidence.
- **macOS/BSD CSPRNG.** Supported Apple/BSD targets use `getentropy(3)` in
  API-compliant 256-byte chunks, avoiding `/dev/urandom` descriptor exhaustion.
- **macOS directory barrier.** Parent-directory durability now attempts
  `F_FULLFSYNC`, matching data-file synchronization, with `fsync()` fallback on
  filesystems that report the stronger operation unsupported.

## Remaining Windows qualification

- A native Windows run from the exact release commit and its signed artifact
  evidence remain mandatory. Wine does not model every NTFS cache, sharing, or
  power-loss behavior.
- Windows crash-injection cannot exercise the POSIX `fork`/`SIGKILL` harness;
  tagged native and encrypted-soak workflows provide the platform gate, while
  Linux dm-flakey remains the destructive power-cut gate.

## Remaining macOS/BSD qualification

- The native macOS CI suite must pass from the exact release commit. Physical
  power-cut behavior on Apple storage remains an external qualification gate;
  no portable test harness can prove drive-cache persistence.
## 32-bit
- Review flagged potential `uint64_t` on-disk size/offset truncation into
  `size_t` on a 32-bit build (files/values > 4 GB). Not confirmed exploitable;
  a 32-bit CI build + large-file test is needed to settle it.
