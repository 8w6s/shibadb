# Contributing to ShibaDB C Core

Thanks for your interest. ShibaDB is a pre-1.0 project — the surface,
docs, and test infrastructure are still solidifying — so contribution
overhead is deliberately kept low, but a few conventions keep the tree
in the state the release process expects.

## Ground rules

1. **Test-first for behaviour change.** Every fix or feature lands in a
   commit sequence that contains a failing test first (or in the same
   commit) demonstrating the bug or the missing behaviour, and the
   passing test after the fix. `ctest --preset clang` and
   `ctest --preset sanitize` must be clean before you send a PR.
2. **Atomic commits.** One logical change per commit. Mechanical
   refactors (rename, move) go in their own commit separate from
   behaviour changes.
3. **Explain the *why*.** Commit-message subjects describe *what*;
   commit bodies describe *why* (which invariant, which bug class,
   which trade-off). Reviewers should not need to reconstruct that
   from the diff.
4. **No new comments unless the *why* is non-obvious.** Well-named
   identifiers are the primary documentation; a comment should record
   a hidden constraint, subtle invariant, or non-obvious trade-off.

## Build & test

Requires a C11 compiler (Clang or GCC), CMake ≥ 3.20, and Ninja.

```sh
# Primary preset — Clang with strict warnings, sanitize-clean tests.
cmake --preset clang
cmake --build build-clang
(cd build-clang && ctest -j$(nproc))

# ASan + UBSan sanitized run — required before every PR touching src/.
cmake --preset sanitize
cmake --build build-sanitize
(cd build-sanitize && ctest -j$(nproc))

# Thread-sanitizer run — required for changes to concurrency code.
cmake --preset tsan
cmake --build build-tsan
(cd build-tsan && ctest -j$(nproc))
```

Additional presets: `gcc`, `coverage`, `fuzz`, `soak`, `release`. See
`CMakePresets.json`.

## Style

- C11, no compiler extensions. `-Wall -Wextra -Wpedantic -Wconversion
  -Wshadow -Werror` must pass under both Clang and GCC.
- No `assert()` in production code paths. Use explicit status returns.
- No `goto`. Use structured control flow.
- No `strcpy`/`strcat`/`sprintf`. Use bounded variants and check
  return values.
- Public headers use Doxygen-style `/** ... */` comments (see
  `include/shibadb.h` for the style).
- Wire format is little-endian; use the helpers in `src/codec.c`
  rather than raw pointer punning.

## Fuzz targets

Every parser or trust-boundary decoder should have a libFuzzer
harness under `fuzz/`. See existing targets — pattern is one file with
`LLVMFuzzerTestOneInput`, a matching `corpus-<name>/` seed dir, and an
entry in `SDB_FUZZ_TARGETS` in `CMakeLists.txt`.

## Reporting bugs vs. security issues

Ordinary bugs: open an issue with a minimal reproduction, the failing
test if you have one, and the output of `cmake --build build-clang &&
ctest` around the failure.

Security issues: follow `SECURITY.md`. Do NOT open a public issue for
a suspected vulnerability.

## License

By submitting a change, you agree that your contribution is licensed
under the MIT license in `LICENSE`.
