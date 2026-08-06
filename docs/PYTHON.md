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

Compact and migration target options are explicit. An encrypted database
requires a target password: `compact()`/`migrate()` without one is refused
with `InvalidArgument` (dropping encryption on a rewrite would silently
produce a world-readable file). Pass the intended target password — the same
one to keep it, or a new one to re-encrypt (rotate) during the rewrite.

## Scanning, queries, and enumeration

Because the V2 key layout stores a namespace contiguously in user-key order,
the binding can iterate it:

```python
# ordered (key, value) pairs; empty prefix = whole namespace;
# reverse=True yields descending key order
for key, value in db.scan("users", b"user:"):
    ...

# the namespaces (tables) that currently hold data: (kind, name),
# kind 1=KV, 2=blob, 3=document
db.list_namespaces()

# JSON documents matching a Mongo-style query (client-side over the scan)
db.find_query("users", {"age": {"$gte": 30}, "name": {"$regex": "^a"}})

# resolve a DBRef {"$ref": namespace, "$id": key}
db.resolve_ref({"$ref": "authors", "$id": "a1"})
```

`shibadb.query.matches_query(doc, query)` is the standalone matcher
(`$or/$and/$not/$gt/$lt/$gte/$lte/$ne/$in/$nin/$exists/$regex`, dotted field
paths). `Database.query(sql)` runs a SQL statement (`SELECT` with
`WHERE/ORDER BY/LIMIT/OFFSET`, plus `INSERT/UPDATE/DELETE` keyed on each
document's `_id`) or a Mongo statement (`db.<collection>.find/findOne/insert/
deleteMany/count(...)`) over a KV namespace of JSON documents.
`shibadb.polyglot.PolyglotParser` parses SQL/Mongo/Redis/CQL/DynamoDB strings
into plan dicts.

## TTL

`put_json(ns, key, doc, ttl=seconds)` stamps a document with an absolute
`_expires_at`. `get_json` and `find_query` then treat it as gone once that time
passes (`include_expired=True` overrides), and `purge_expired(ns)` deletes the
lapsed ones. Expiry is lazy and lives in the Python layer; raw `get/put/delete`
and the C engine are untouched, and a document without a numeric `_expires_at`
never expires.

## Migrating a legacy V1 database

A database written by a pre-2026-08 build uses the V1 key layout and is refused
by `open` with `SDB_E_UNSUPPORTED_VERSION`. `shibadb.migrate_file(source, dest,
source_password=..., target_password=...)` writes a fresh V2 file (the source is
cloned first and left untouched) that opens normally.

## Package gate

The test suite:

- builds the wheel without setuptools or network access;
- creates a clean virtual environment;
- installs the wheel with pip and no dependencies;
- loads the freshly built shared library by ABI;
- runs encrypted transactional commit/rollback/isolation, KV, blob,
  document/index, verify, backup, compact, migration/rekey, close, and reopen
  conformance.
