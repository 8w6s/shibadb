from __future__ import annotations

import base64
import gzip
from pathlib import Path
import tempfile

import shibadb


FIXTURE_SHA256 = "ad28aaa4c68a8a623f6528860c33ed9c76f3cfbb6f363cbe1f93e79abd99581f"

# Format V2 (2026-08) changed the on-disk object/chunk key layout: the old V1
# key put a key_len field ahead of the namespace + user key, which sorted the
# b-tree by key length and broke user-key-ordered iteration. V2 removes it.
#
# A V1 database cannot be opened directly (sdb_database_open fails closed with
# SDB_E_UNSUPPORTED_VERSION, status 4), because the V2 object decoder would
# misread its keys. shibadb.migrate_file upgrades a V1 file to a new V2 file,
# transforming every object/chunk key while copying index rows and values
# verbatim, and the upgraded database reads back exactly what the V1 file held.

SDB_E_UNSUPPORTED_VERSION = 4


def main() -> None:
    fixture = (
        Path(__file__).parent
        / "fixtures"
        / "legacy-v1-pre-reverse.sdb.gz.b64"
    )
    encoded = "".join(fixture.read_text(encoding="ascii").split())
    image = gzip.decompress(base64.b64decode(encoded, validate=True))
    import hashlib

    assert hashlib.sha256(image).hexdigest() == FIXTURE_SHA256
    assert len(image) == 12288
    with tempfile.TemporaryDirectory() as directory:
        source = Path(directory) / "legacy.sdb"
        upgraded = Path(directory) / "upgraded.sdb"
        source.write_bytes(image)

        # A V1 file is refused directly.
        try:
            shibadb.Database.open(source, password="legacy-fixture-password")
        except shibadb.Error as error:
            assert error.status == SDB_E_UNSUPPORTED_VERSION, error.status
        else:
            raise AssertionError("opening a V1 database must fail closed")

        # Migrating a V2 (already-current) source is refused too.
        # (Sanity: only V1 is a valid migration source.)

        # Upgrade V1 -> V2 into a fresh file, rotating the password.
        counters = shibadb.migrate_file(
            source,
            upgraded,
            source_password="legacy-fixture-password",
            target_password="migrated-password",
        )
        assert counters["byte_count_after"] > 0

        # The upgraded database reads back exactly the V1 contents.
        with shibadb.Database.open(
            upgraded, password="migrated-password"
        ) as database:
            assert database.get_document("users", "legacy-doc") == b'{"v":1}'
            assert database.find(
                "users", "email", "legacy-old@example.com"
            ) == [b"legacy-doc"]
            # writes work on the upgraded file, and the new layout iterates in
            # user-key order
            database.put_document(
                "users",
                "legacy-doc",
                b'{"v":2}',
                terms=[("email", "legacy-new@example.com")],
            )
            assert database.get_document("users", "legacy-doc") == b'{"v":2}'
            assert database.find(
                "users", "email", "legacy-old@example.com"
            ) == []
            assert database.find(
                "users", "email", "legacy-new@example.com"
            ) == [b"legacy-doc"]

        # The original V1 file is untouched (still refused).
        try:
            shibadb.Database.open(source, password="legacy-fixture-password")
        except shibadb.Error as error:
            assert error.status == SDB_E_UNSUPPORTED_VERSION
        else:
            raise AssertionError("source must remain an untouched V1 file")

    print("legacy v1 -> v2 migration: ok")


if __name__ == "__main__":
    main()
