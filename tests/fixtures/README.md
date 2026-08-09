# Compatibility fixtures

`legacy-v1-pre-reverse.sdb.gz.b64` is an encrypted v1 database produced by
commit `e28e8b4` (before document reverse rows were introduced). It contains:

- password: `legacy-fixture-password`;
- unique index `users/email`;
- document `users/legacy-doc` with body `{"v":1}`;
- index value `legacy-old@example.com`;
- no `0x53` reverse-row entry.

The decoded image is 12,288 bytes with SHA-256
`ad28aaa4c68a8a623f6528860c33ed9c76f3cfbb6f363cbe1f93e79abd99581f`.
`test_legacy_fixture.py` verifies the digest before opening it, exercises the
legacy fallback, migrates it in place with a new password and page size, then
reopens and verifies the resulting database.
