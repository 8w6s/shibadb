# Python binding

The `python/shibadb` package is a dependency-free `ctypes` binding to C ABI
v1. It never imports or uses SQLite.

The wheel is intentionally platform-independent and does not duplicate the
native engine. Install `libshibadb` through CMake or a system package, then
either place it on the platform library search path or set
`SHIBADB_LIBRARY` to its exact path.

## Versioning

The binding is versioned **independently** of the C library, and the two numbers
are not expected to match:

| Component | Version | Declared in |
|---|---|---|
| C library / on-disk format | `1.0.0` | `CMakeLists.txt` `project(... VERSION)`, `SDB_VERSION_STRING` |
| C ABI | `1` | `SDB_ABI_VERSION` |
| Python binding | `0.1.0` | `python/pyproject.toml`, `python/shibadb/__init__.py` |

The binding's compatibility promise is against the **ABI**, not against the
library's release number, and it records that with
`[tool.shibadb] native-abi = 1` in `pyproject.toml`. `0.1.0` means the binding
surface itself is still pre-1.0 and may change; it says nothing about the
maturity of the engine underneath it.

This is a deliberate decision, not drift — but it is worth restating here
because the mismatch looks like a bug at first glance, and because an earlier
revision of this page documented the wheel as `shibadb-1.0.0-...`, which was
never the filename the build produced. The `python_wheel` CTest globs
`shibadb-*.whl`, so it passed either way and the discrepancy went unnoticed.

If the binding is ever promoted to track the library's version number, change
`pyproject.toml`, `__init__.py` and this table together.

## Build and install the wheel

```sh
python3 python/build_wheel.py dist
python3 -m pip install dist/shibadb-0.1.0-py3-none-any.whl
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
