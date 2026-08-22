# Python binding

The `python/shibadb` package is a dependency-free `ctypes` binding to C ABI
v1. It never imports or uses SQLite.

The wheel is intentionally platform-independent and does not duplicate the
native engine. Install `libshibadb` through CMake or a system package, then
either place it on the platform library search path or set
`SHIBADB_LIBRARY` to its exact path.

## Build and install the wheel

```sh
python3 python/build_wheel.py dist
python3 -m pip install dist/shibadb-1.0.0-py3-none-any.whl
export SHIBADB_LIBRARY=/path/to/libshibadb.so.1
```

The wheel builder uses only the Python standard library.

## Example

```python
import shibadb

with shibadb.Database.create("app.sdb", password="secret") as db:
    with db.transaction() as transaction:
        transaction.put("settings", "theme", b"dark")
        transaction.put_blob("media", "avatar", image_bytes)
        transaction.create_index("users", "email", unique=True)
        transaction.put_document(
            "users",
            "user-1",
            b'{"name":"A"}',
            terms=[("email", "a@example.test")],
        )
        assert transaction.get("settings", "theme") == b"dark"

    # A normal exit commits all operations together.
    assert db.get("settings", "theme") == b"dark"
    assert db.find("users", "email", "a@example.test") == [b"user-1"]

    report = db.verify()
    db.backup("app-backup.sdb")
```

Names, keys, values, documents, and index terms accept `bytes`; names may
also be UTF-8 `str`. Native status codes are mapped to Python exceptions such
as `NotFoundError`, `ConflictError`, `BusyError`, and
`AuthenticationError`.

`Database.scan(namespace, prefix=b"", *, reverse=False, limit=0)` returns the
`(key, value)` pairs whose key begins with `prefix` (an empty prefix walks the
whole namespace). Both `reverse` and `limit` are passed to the native
`sdb_kv_scan_prefix` through an `sdb_scan_options` struct rather than applied in
Python, so `limit` bounds the work the engine does and `reverse` with `limit`
returns the *greatest* n keys — not the smallest n reversed, which is what
reversing the returned list would give. `limit=0` means unlimited; a negative
limit raises `ValueError`. The scan holds a read snapshot for its duration, so it
excludes writers on that handle until it returns. The bundled
`python -m shibadb.cli … scan` exposes both as `--reverse` and `--limit`.

Encrypted create, compact, and migration operations default to 600,000
PBKDF2-HMAC-SHA256 iterations. Applications may pass `kdf_iterations`
explicitly after benchmarking their target hardware.

`Database.transaction()` returns an owned `Transaction`. A normal context
manager exit commits; an exception rolls back and is not swallowed. Explicit
`commit()`, `rollback()`, and `close()` are also available. `close()` rolls
back an active Python transaction, while using a completed transaction raises
`RuntimeError`. The transaction keeps its database object alive and the native
engine rejects database close or a second transaction until it terminates.

Transaction methods cover KV, blob, document, and index creation. Index
`find()` remains a database-level committed-view query; transaction point gets
provide read-your-writes.

Compact and migration target options are explicit. Passing no password to
`compact` creates a plaintext target, so encrypted applications should pass
the intended target password.

## Package gate

The test suite:

- builds the wheel without setuptools or network access;
- creates a clean virtual environment;
- installs the wheel with pip and no dependencies;
- loads the freshly built shared library by ABI;
- runs encrypted transactional commit/rollback/isolation, KV, blob,
  document/index, verify, backup, compact, migration/rekey, close, and reopen
  conformance.
