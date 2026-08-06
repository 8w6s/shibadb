## Summary

A one-sentence description of the change.

## Motivation

What this fixes, adds, or improves. Link to the issue if one exists.

## Verification

- [ ] `ctest --preset clang` — all tests pass.
- [ ] `ctest --preset sanitize` — ASan + UBSan clean.
- [ ] New behavior has a test that fails on `main` and passes with
      this change (test-first).
- [ ] Invariants added or modified are reflected in
      `docs/PAGER_INVARIANTS.md` or the relevant format document.
- [ ] Public API changes update `include/shibadb.h`,
      `include/shibadb_engine.h`, `abi/symbols-v1.txt`, and the
      version macros in `include/shibadb.h`.
- [ ] Commit messages describe the *why*, not just the *what*.

## Notes for review

Anything the reviewer should look at first: the trickiest hunk, an
invariant that was subtle, a performance measurement, etc.
