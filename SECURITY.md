# Security policy

## Supported versions

ShibaDB is a pre-1.0 project shipping `v1.0.0-rc*` release candidates.
The only supported line is the current `main` branch. Release-candidate
tags receive security fixes only if `HEAD` shares the affected code;
downgrade backports are not provided until the first production release.
Support windows for production tags will be published with the final
1.0.0 release.

## Audit status

ShibaDB has not yet completed an independent security audit. Do not
describe the current release series as externally audited. Internal
reviews are recorded in `docs/AUDIT.md` and `docs/AUDIT_LOG.md`; treat
them as best-effort self-review, not as a replacement for independent
review.

## Reporting a vulnerability

Report suspected vulnerabilities **privately** through the repository
host's security-advisory mechanism. Do not open a public issue for a
suspected vulnerability before a fix is available.

Please include:

- Affected version (git commit SHA is best; a tag is fine too).
- Platform and build configuration (compiler, sanitizers on/off, OS,
  filesystem).
- Minimal reproduction — ideally a small C program that links against
  the library and exhibits the issue.
- Expected impact (data loss, DoS, information disclosure, privilege
  escalation across trust boundaries, etc.).
- Whether the report may be acknowledged publicly and under what
  name/handle.

## What we treat as in-scope

- Any memory-safety bug reachable from the public API surface
  (`SDB_API` symbols in `include/shibadb.h` and
  `include/shibadb_engine.h`).
- Any path that lets a non-owner of a database file influence the
  behaviour of a legitimate `sdb_database_open` — corrupted
  sidecars, WAL forgery, `.replace` marker manipulation, superblock
  tampering.
- Any code path that leaks plaintext, keys, passwords, or wrap-tag
  bits through logs, side channels, or error paths.
- Any invariant listed in `docs/PAGER_INVARIANTS.md` that can be
  violated via the public API alone (no filesystem tampering).
- Any missing preconditions in `SDB_API` functions that lead to
  memory corruption or crash on caller-supplied input.

## What we do NOT consider a vulnerability

- Denial-of-service by a caller who already holds a valid database
  handle (they can freely make their own database misbehave).
- Attacks against a database file the attacker controls before
  opening (this is a trust-domain violation of the caller's own
  making — the file must come from a trusted source).
- Issues introduced by non-default compile-time flags
  (`SDB_TESTING=ON` in a production build, disabled sanitizers on
  a security-critical deployment, etc.).

## Application responsibilities (what ShibaDB does NOT do for you)

ShibaDB provides **encryption at rest**: the database file is
unreadable without the database password, and each database gets a
fresh random 16-byte salt fed into PBKDF2-HMAC-SHA256 (600k iterations)
so that two databases opened with the *same* password derive *different*
keys — there is no shared/global salt and no way to tell from the files
that two databases share a password.

That salt+KDF protects the **database password**. It does **not** hash
any application-level secrets you store *inside* the database. ShibaDB
stores the bytes you hand it verbatim (then encrypts the page). So if
you use ShibaDB to hold **end-user account passwords** (e.g. a shop's
customer logins), you must hash them yourself, at the application layer,
with a dedicated password hash — **Argon2id** (preferred), scrypt, or
bcrypt — *before* calling `put`. Those algorithms apply a per-user salt,
so two users with the same password get different stored hashes.

Storing raw passwords, or an unsalted `SHA-256(password)`, is unsafe
even inside an encrypted ShibaDB: anyone who ever obtains the decrypted
contents (a legitimate operator, a memory dump, a future key compromise)
would see that two users share a password and could mount offline / precomputed-table
attacks. At-rest encryption and password hashing are complementary, not
substitutes: ShibaDB does the former; your application must do the latter.

## Response targets

Best-effort acknowledgement within 3 business days. Fix ETA depends
on severity and exploitability; the reporter will be kept informed.
Security-sensitive changes go through the full encryption,
fault-injection, sanitizer, ABI, and cross-platform gates before
release.

