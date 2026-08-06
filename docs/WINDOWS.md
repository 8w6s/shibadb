# Windows portability standard

ShibaDB is written in portable C11 and was originally developed and gated on
Linux (GCC/Clang). This document is the reference for building, testing, and
reasoning about the engine on native Windows (MSVC). It records the Windows
specific invariants the code relies on so that changes made on Linux keep the
Windows build green, and so that a Windows contributor knows the ground rules.

Treat every rule below as a release gate for the Windows target, on par with
the Linux `-Werror` gate.

## 1. Toolchain

| Component | Version used | Notes |
|---|---|---|
| Compiler | MSVC (`cl.exe`) 14.51+ (VS 2022/BuildTools 18) | C11 (`/std:c11`), `/W4 /WX` |
| Generator | Ninja (bundled with VS CMake) | single-config |
| CMake | 3.20+ (bundled with VS) | |
| Build type | `RelWithDebInfo` | ships a `.pdb` |

The repository ships `build_msvc.bat` as the canonical Windows build driver. It
sources `vcvars64.bat`, puts the bundled Ninja on `PATH`, and exposes four
phases:

```
build_msvc.bat configure   # CMake configure (strict: /W4 /WX, no suppressions)
build_msvc.bat build       # ninja --parallel -k 0
build_msvc.bat test        # ctest --output-on-failure -j4
```

`configurefast` is retained as an alias for `configure`; it once injected
`/wd4701 /D_CRT_SECURE_NO_WARNINGS`, but those suppressions were removed once
every warning was fixed at source (see §3). The tree builds clean under
`/W4 /WX` with no warning suppressions of any kind.

## 2. Warnings-as-errors parity with Linux

The library and every C test compile with `/W4 /WX` (see `CMakeLists.txt`),
the MSVC equivalent of the Linux `-Wall -Wextra -Wpedantic -Wconversion
-Wshadow -Werror` gate. Two MSVC-specific warning classes do **not** fire on
GCC/Clang and must be handled at source rather than suppressed:

* **C4701 "potentially uninitialized local variable."** MSVC's flow analysis
  does not model the "`status == SDB_OK` implies the out-parameter was
  written" contract that pervades the engine, so it flags variables that are
  in fact always assigned before use. The fix is to initialise such variables
  at their declaration (`= 0U`, `= false`, `= {0}`). These initialisers are
  dead on every real path but make the invariant explicit and fail closed if a
  future refactor breaks the contract. Never paper over C4701 with `/wd4701`.

* **C4996 "deprecated CRT function"** (`getenv`, `sscanf`, `fopen`, ...). The
  library core avoids these entirely, so it needs no suppression. Prefer the
  portable standard alternatives (`strtoull` instead of `sscanf`, etc.). Where
  a genuinely portable call is unavoidable in *tooling* (for example the CLI
  reading `getenv("SHIBADB_PASSWORD")`), scope `_CRT_SECURE_NO_WARNINGS` to
  that one target with `target_compile_definitions`, never globally.

## 3. Path handling — the single most important Windows rule

Windows path handling lives in `src/windows_path.c`. The engine converts every
UTF-8 path to UTF-16 (`sdb_windows_path_from_utf8`) and, for absolute and UNC
paths, prepends the extended-length `\\?\` prefix so paths longer than
`MAX_PATH` (260) work.

**Invariant: the `\\?\` prefix disables Win32 path normalization.** Once a path
carries that prefix, Windows no longer treats `/` as a separator — it becomes
an ordinary (invalid) filename character. Therefore:

* All separators **must** be normalised to backslash *before* the `\\?\`
  prefix is applied. `sdb_windows_path_from_utf8` does this in one place so
  create, open, and the lock sidecar agree.
* A caller may pass either `/` or `\` (Win32 accepts both for prefix-less
  paths); the engine accepts both and canonicalises internally.

Historical bug (fixed): `open` reached `CreateFileW` with a raw `\\?\C:/dir/f`
and failed with `SDB_E_IO`, while `create` survived only because it laundered
the path through `GetFullPathNameW` (which normalises `/`→`\`). The forward
slashes are now converted up front, so create/open/lock are consistent.

**Rule for Linux contributors:** never build a `\\?\`-prefixed path from a
string that may contain `/`. If you add a new path entry point, route it
through `sdb_windows_path_from_utf8`.

## 4. Filesystem integrity checks that surprise people

* **`nNumberOfLinks != 1` is rejected.** On open the engine refuses a database
  file with more than one hard link (an anti-tampering / anti-aliasing check,
  mirroring the POSIX `st_nlink == 1` check). This is correct, but it means a
  database must not live in a directory that a sync client hard-links behind
  your back. In particular **Google Drive for Desktop** hard-links every new
  file under `Downloads`/synced folders into a `.tmp.driveupload` staging path,
  giving the file two links and causing open to fail with
  `SDB_E_INVALID_ARGUMENT`. Build and run tests outside synced folders (this
  repo lives at `C:\shibadb`, not under `Downloads`).

* **Process-lock sidecar.** Each open creates a `<db>.lock` sidecar
  (`src/sync.c`) holding a byte-range lock for single-writer exclusion. On
  Windows the sidecar file is left on disk after close (only the lock is
  released, the file is not deleted); this is benign — a stale zero-byte
  `.lock` is not a held lock. Do not treat a lingering `.lock` as corruption.

* **Identity lock namespace.** In addition to the `.lock` byte-range lock,
  open takes a named kernel object keyed by the file's volume-serial + file
  index (anti-aliasing: two path spellings of the same physical file collide).
  It is created in the `Global\` namespace when possible and falls back to
  `Local\` when the process lacks `SeCreateGlobalPrivilege` (ordinary
  non-elevated apps), so a standard user can always open a database; the
  `.lock` byte-range lock still enforces path-based, machine-wide exclusion.

* **Known limitation (identity lock crash-release).** The identity object is a
  named *semaphore*, chosen because it can be released from a different thread
  than the one that acquired it (a named *mutex* is thread-affine, which would
  break open-on-one-thread / close-on-another). A semaphore does not go
  "abandoned" on owner death the way a mutex does, so in a narrow race a
  crashed holder's identity slot can stay taken until every transient handle to
  the object closes. The `.lock` byte-range lock (which the OS releases on
  process death) is the primary crash-release mechanism; the identity object is
  a secondary anti-aliasing guard. A fully crash-release-correct,
  cross-thread, no-privilege identity lock needs a dedicated redesign
  (e.g. an identity-named lock *file*) and multi-process/multi-thread tests.

## 5. Sanitizers and fuzzing on Windows

* **AddressSanitizer** is available through MSVC (`/fsanitize=address`); the
  ASan runtime ships with the VS toolset. Use it for a Windows memory-safety
  pass equivalent to the Linux ASan gate. Configure a separate build tree with
  `-DCMAKE_C_FLAGS=/fsanitize=address` and run `ctest`. Exclude the
  timing-dependent concurrency test from the ASan lane —
  `ctest -LE packaging -E "^group_commit$"` — because `test_group_commit` has
  an *anti-vacuous* `compact_busy > 0` assertion that requires observing a
  concurrent compact/commit BUSY race; ASan's altered scheduling does not
  reliably hit that window, so the assertion fails without any memory error.
  The engine itself is ASan-clean (0 findings) across the rest of the suite.
* **UBSan / TSan / libFuzzer** are Clang features and are **not** available
  under MSVC. Run those on the Linux/Clang gate. If Clang for Windows
  (`clang-cl`) is installed, the fuzz targets can be built there; plain MSVC
  cannot build them.

## 6. Long-running evidence on Windows

The soak target (`-DSDB_ENABLE_SOAK_TESTS=ON` → `test_engine_soak`) is
standalone and honours `SDB_SOAK_OPERATIONS` (default 2000, max 10,000,000).
It performs mixed put/delete/get with periodic verify, backup+reopen, and
compact, so its wall-clock is dominated by the PBKDF2 reopen (600k iterations)
rather than the KV ops. Windows-native soak/crash/concurrency results belong in
`docs/METRICS.md` alongside the Linux evidence.

## 7. Checklist before claiming the Windows gate

1. `build_msvc.bat configure` from a clean `build-msvc` (delete it first — a
   stale `CMakeCache.txt` can retain old `CMAKE_C_FLAGS` such as `/wd4701` and
   mask warnings).
2. `build_msvc.bat build` — zero warnings (only the benign `D9025`
   `/UNDEBUG`-override command-line note is expected for test targets).
3. `build_msvc.bat test` — 100% pass.
4. If touching path or file code, re-run `test_utf8_path`, `test_file`, and the
   `cli` conformance test, which exercise `/` vs `\`, extended-length, and UNC
   paths.
