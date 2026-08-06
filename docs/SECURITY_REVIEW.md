# Security review

_Last updated: 2026-07-28 (Phase 0 cleanup)_


Baseline: cbf47ca
Head: 5d53b33 (findings 1 and 3 fixed in 6cf800d)

## Follow-up status
- **Finding 1** (`sdb_validate_superblock_field` global): **fixed** in commit
  6cf800d — function and declaration gated behind `#if SDB_TESTING`. Release
  build (SDB_TESTING=OFF) no longer contains the symbol.
- **Finding 2** (missed alias cases in reclaim_internal_up): **fixed** in
  commit 5546fbb — extended preamble now rejects
  `sibling_page==0`, `sibling_page==parent_page`,
  `survivor_page==parent_page`, and `survivor_page==sibling_page`
  alongside the pre-existing `sibling_page==node_page` guard.
- **Finding 3** (CMake PUBLIC leak of SDB_TESTING / fuzzer flags): **fixed**
  in commit 6cf800d — flags now wrapped in `$<BUILD_INTERFACE:...>`, so
  installed CMake targets do not propagate build-mode-dependent flags.
- **Findings 4, 5**: INFO-level, no action needed.

---

Files touched (in scope): src/btree.c, src/file.c, src/file.h, src/wal.c, src/wal.h, src/engine.c, src/engine_internal.h, src/superblock.c, src/replace.c, fuzz/fuzz_page.c, CMakeLists.txt.

## Findings

### 1. `sdb_validate_superblock_field` is a global symbol in `libshibadb_core.a` — LOW
- Location: src/superblock.c:112-115.
- The function is declared/defined without `static` and without `SDB_API`. In the shared library it is hidden by `C_VISIBILITY_PRESET hidden` (`nm -D libshibadb.so | grep validate_superblock` is empty). But the static archive exposes it as a global text symbol:
  ```
  superblock.c.o: 0000000000000000 T sdb_validate_superblock_field
  ```
  Any consumer that statically links `ShibaDB::core` and provides an extern declaration can call it.
- Exploitation: The returned `sdb_sb_field` enum names exactly which superblock field failed validation (page_size / flags / next_page_id / file_id / salt / key_wrap_id / kdf_iterations / wrapped_key / key_wrap_tag / enc_mismatch). A statically-linked adversary who can feed candidate structs (e.g. a fuzz harness in their own binary) gets a field-granularity oracle instead of a flat `SDB_E_INVALID_ARGUMENT`, narrowing the search for a superblock that passes validation.
- Fix: mark the function `static` and expose it to the field-diag test via a private `src/`-scoped header included only by the test, or gate the non-static declaration behind `#if SDB_TESTING`.

### 2. Missed alias cases in `sdb_btree_reclaim_internal_up` — LOW
- Location: src/btree.c:310-361.
- The new guards catch `parent_page == node_page` and `sibling_page == node_page`, and reject zero for `node_page`/`survivor_page`. They do NOT catch:
  - `sibling_page == 0` — a corrupt internal parent with a zero child slot reads page 0 (the superblock) as a btree node.
  - `sibling_page == parent_page` — immediate parent/child cycle.
  - `survivor_page == parent_page` or `survivor_page == sibling_page` — on the cascade at src/btree.c:465-473 `survivor_page` is fed from `parent.first_child`, which is untrusted after corruption. Once written into `combined.first_child` (line 414) or `combined.entries[..].right_child` (line 411), a cycle is baked into the merged node.
- Downstream `kind != SDB_BTREE_INTERNAL` and `sdb_btree_node_encoded_size` checks catch most of these before any staged write escapes, so this is defense-in-depth rather than an exploitable write. Public APIs cannot craft this state on a healthy DB.
- Fix: extend the pre-check with `sibling_page == 0 || sibling_page == parent_page || survivor_page == parent_page || survivor_page == sibling_page`.

### 3. `SDB_TESTING` and fuzzer sanitizer flags leak via CMake PUBLIC — LOW
- Location: CMakeLists.txt:101-118.
- `target_compile_definitions(${target} PUBLIC SDB_TESTING=1)` and `target_compile_options(${target} PUBLIC -fsanitize=fuzzer-no-link,address,undefined ...)` propagate through the exported `ShibaDBTargets.cmake` INTERFACE to downstream consumers of `ShibaDB::core` / `ShibaDB::shared`.
- Consequences: a consumer building against an installed test-mode library sees `-DSDB_TESTING=1` in their own compilation; `sdb_file` layout is gated by that macro but the header ships only under `src/`, so real ABI risk is low. Ticking `SDB_ENABLE_FUZZERS=ON` in a release configuration silently instruments every consumer binary with libFuzzer/ASan/UBSan — a configuration foot-gun.
- Fix: switch to `PRIVATE`, or wrap in `$<BUILD_INTERFACE:...>` so install-time exports strip the flag.

### 4. fuzz_page.c OOB review — INFO (clean)
- Location: fuzz/fuzz_page.c.
- All memcpy/memset lengths are bounded by `page_size` (validated ≤ `SDB_MAX_PAGE_SIZE`). `size < 2` early return keeps `data + 2` in range; `size - 2 >= page_size` in the large-size branch keeps the source read inside the fuzzer buffer. `memcpy(dst, src, 0)` when `size == 2` is defined behaviour. `page` is freed on every exit path. ASan-clean.

### 5. `SDB_FILE_IO_LIMIT` macro side-effects — INFO (clean)
- Location: src/file.c:11-15.
- Both branches evaluate the argument exactly once (`((void)(f), SIZE_MAX)` and `((f)->io_limit)`). All four call sites pass a plain pointer. `grep io_limit` shows no reader outside src/file.c and no writer outside the setter — the field is fully internal.

## Red-team pass

- Tried to exploit the field oracle at (1) via the shared library — blocked by hidden visibility. Only static-link consumers are affected.
- Checked whether SDB_TESTING leaks into hot-path behaviour: no. `sdb_file_should_fail_for_testing` collapses to `return false` inline; `sdb_compact_test_crash` / `sdb_backup_test_crash` collapse to `(void)phase` no-ops.
- Struct-layout ABI mismatch across SDB_TESTING modes: `sdb_file` is only defined in `src/file.h`, never installed, so downstream code cannot see either layout.
- Confirmed exported symbols: `nm -D libshibadb.so | grep -iE 'validate_superblock|for_testing'` returns empty; internal `nm` shows every hit as lowercase `t` (hidden).
- Verified `sdb_backup_file_fail_after_for_testing` reference at src/engine.c:2755 is under `#if SDB_TESTING` — no dangling symbol in release.

## Verification

Static inspection only (no rebuild, no test run):

- `git diff cbf47ca..HEAD` on every in-scope file.
- `nm -D build-clang/libshibadb.so` — no exported test-hook or field-diag symbol.
- `nm build-clang/libshibadb.so | grep -iE 'validate_superblock|for_testing'` — all lowercase `t`.
- `nm build-clang/libshibadb_core.a | grep validate_superblock_field` — one hit: `T` global (evidence for finding 1).
- `build-clang/CMakeCache.txt` — SDB_BUILD_TESTS=ON, SDB_TESTING=ON, SDB_ENABLE_FUZZERS=OFF.
- `grep -rn io_limit src/ tests/ include/` — sole reader is src/file.c; sole writer is the gated setter.

No exploitable findings survived verification; the four items above are hardening recommendations, not shipping blockers.
