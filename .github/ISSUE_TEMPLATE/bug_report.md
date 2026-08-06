---
name: Bug report
about: Report an incorrect behavior or crash in ShibaDB
title: ''
labels: bug
assignees: ''
---

## Summary

A one-sentence description of what went wrong.

## Reproduction

Minimum steps to reproduce. If possible, attach a short C program or
a shell snippet that exhibits the failure. If the failure depends on
a specific database file, include a hex dump or an anonymized copy.

```c
// paste here
```

## Expected behavior

What you expected ShibaDB to do.

## Actual behavior

What ShibaDB actually did. Paste the exact status code returned by
the API, or the output of `sdb_status_string(status)`.

## Environment

- ShibaDB version (from `sdb_version_string()` or `git describe`):
- Compiler and version (`cc --version`):
- Operating system and kernel (`uname -a` on POSIX, `ver` on Windows):
- CMake preset used to build (`clang`, `gcc`, `release`, ...):
- Encrypted or plaintext database:

## Additional context

Any relevant sanitizer output, stack traces, or `docs/PAGER_INVARIANTS.md`
invariants you suspect are being violated.
