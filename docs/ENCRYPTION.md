# Encryption design

## Scope

Encrypted databases use XChaCha20-Poly1305 authenticated encryption for every
allocated page, including freelist pages. WAL records contain the already
encrypted full-page images, so user keys and values do not appear in either
the database page area or WAL.

The superblock remains visible and exposes operational metadata such as page
size, allocation counters, file identity, KDF parameters, and the wrapped data
key. Encryption does not hide file size, update timing, page access patterns,
or data present in process memory.

This implementation has deterministic standard-vector, tamper, fuzz, nonce,
rotation, crash, and sanitizer coverage. It has not received an independent
third-party cryptographic audit.

## Key hierarchy

Creation obtains a random 256-bit data-encryption key from the operating
system CSPRNG. A password-derived wrapping key encrypts that data key in the
mirrored superblock:

```text
password --PBKDF2-HMAC-SHA256--> wrapping key
                                       |
random data key --XChaCha20-Poly1305----+--> wrapped key + tag
       |
       +--XChaCha20-Poly1305--> database pages and WAL page images
```

The PBKDF2 iteration count is stored in the superblock, bounded during parsing,
and may be increased during password rotation. Applications should benchmark
and choose the highest acceptable value for their deployment.

The parser enforces `SDB_MIN_KDF_ITERATIONS` (600,000) as the lower bound and
`SDB_MAX_KDF_ITERATIONS` (10,000,000) as the upper bound on every open, create,
and rewrite. The creation default `SDB_DEFAULT_KDF_ITERATIONS` is also 600,000
— matching the current OWASP PBKDF2-HMAC-SHA256 work-factor recommendation.
Because the work factor is stored per database, increasing a future default
does not make old files unreadable *unless* the file's stored value falls
below the parser minimum. Databases created before 2026-07 with iteration
counts below 600,000 must be migrated via password rotation to a compliant
work factor before upgrading — see `sdb_database_rotate_password` in
[`ENGINE_API.md`](ENGINE_API.md). The engine will refuse to open a database
whose stored `kdf_iterations` is outside `[SDB_MIN_KDF_ITERATIONS,
SDB_MAX_KDF_ITERATIONS]` with `SDB_E_CORRUPT`.

Password rotation derives a new wrapping key and rewraps the unchanged random
data key. Only the mirrored superblock changes, making rotation atomic without
rewriting all pages. The wrap nonce is the 128-bit file ID followed by the
64-bit wrap generation.

## Page envelope

The ordinary 32-byte page header remains outside encryption and identifies an
encrypted page. Its payload starts with:

| Offset | Size | Field |
|---:|---:|---|
| 0 | 4 | `SEN1` magic |
| 4 | 2 | envelope version |
| 6 | 2 | encrypted logical page type |
| 8 | 24 | random XChaCha20 nonce |
| 32 | 4 | plaintext payload size |
| 36 | 16 | Poly1305 authentication tag |
| 52 | variable | ciphertext |

Associated data binds the file ID, page ID, page LSN, logical page type, and
plaintext size. Moving ciphertext to another database, page, LSN, or type
therefore fails authentication.

Plaintext is released only after constant-time tag verification. A failed
authentication leaves the caller's output buffer unchanged. In-memory data
keys and staged encrypted-transaction payloads are explicitly zeroed before
release.

## Nonce policy

Every page encryption requests a fresh 192-bit nonce from the operating-system
CSPRNG. The large nonce is intentional: XChaCha20 was designed to make random
nonces suitable for long-lived keys. The reliability suite records and checks
128 consecutive page-update nonces for duplicates.

## Standards basis

- RFC 8439 defines ChaCha20-Poly1305 and its normative test vectors:
  <https://www.rfc-editor.org/rfc/rfc8439>
- The CFRG XChaCha draft defines HChaCha20, the 192-bit construction, and the
  XChaCha20-Poly1305 vectors used by the test suite:
  <https://datatracker.ietf.org/doc/html/draft-irtf-cfrg-xchacha>
- NIST SP 800-132 specifies password-based derivation for storage:
  <https://doi.org/10.6028/NIST.SP.800-132>
- OWASP publishes the current PBKDF2-HMAC-SHA256 deployment work factor:
  <https://cheatsheetseries.owasp.org/cheatsheets/Password_Storage_Cheat_Sheet.html>
