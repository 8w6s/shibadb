from pathlib import Path
import tempfile

import shibadb

def main() -> None:
    assert shibadb.abi_version() == 1
    assert shibadb.version() == "1.0.0"
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        path = root / "database.sdb"
        backup = root / "backup.sdb"
        large = bytes((index * 37) & 0xFF for index in range(50000))
        with shibadb.Database.create(path, password="first-password") as db:
            with db.transaction() as transaction:
                transaction.put("atomic", "setting", b"committed")
                transaction.put_blob("atomic", "blob", large[:7000])
                transaction.create_index(
                    "atomic-users", "email", unique=True
                )
                transaction.put_document(
                    "atomic-users",
                    "atomic-doc",
                    b'{"atomic":true}',
                    terms=[("email", "atomic@example.test")],
                )
                assert transaction.get(
                    "atomic", "setting"
                ) == b"committed"
                assert transaction.get_blob(
                    "atomic", "blob"
                ) == large[:7000]
                assert transaction.get_document(
                    "atomic-users", "atomic-doc"
                ) == b'{"atomic":true}'
                try:
                    db.get("atomic", "setting")
                except shibadb.NotFoundError:
                    pass
                else:
                    raise AssertionError("uncommitted value escaped")
                try:
                    db.close()
                except shibadb.BusyError:
                    pass
                else:
                    raise AssertionError("active transaction allowed close")
            assert db.get("atomic", "setting") == b"committed"
            assert db.get_blob("atomic", "blob") == large[:7000]
            assert db.get_document(
                "atomic-users", "atomic-doc"
            ) == b'{"atomic":true}'

            try:
                with db.transaction() as transaction:
                    transaction.put("atomic", "rollback", b"hidden")
                    assert transaction.get(
                        "atomic", "rollback"
                    ) == b"hidden"
                    raise ValueError("force rollback")
            except ValueError:
                pass
            else:
                raise AssertionError("context exception was swallowed")
            try:
                db.get("atomic", "rollback")
            except shibadb.NotFoundError:
                pass
            else:
                raise AssertionError("rollback value persisted")

            transaction = db.transaction()
            try:
                db.transaction()
            except shibadb.BusyError:
                pass
            else:
                raise AssertionError("second transaction was allowed")
            transaction.put("atomic", "explicit", b"rollback")
            transaction.rollback()
            try:
                transaction.get("atomic", "explicit")
            except RuntimeError:
                pass
            else:
                raise AssertionError("closed transaction remained usable")

            transaction = db.transaction()
            transaction.put("atomic", "close", b"rollback")
            transaction.close()
            try:
                db.get("atomic", "close")
            except shibadb.NotFoundError:
                pass
            else:
                raise AssertionError("transaction close did not roll back")

            db.put(b"settings\x00", b"binary\x00key", b"value\x00data")
            assert db.get(b"settings\x00", b"binary\x00key") == b"value\x00data"
            db.put_blob("media", "large", large)
            assert db.get_blob("media", "large") == large
            db.create_index("users", "email", unique=True)
            db.put_document(
                "users",
                "doc-1",
                b'{"name":"one"}',
                terms=[("email", "one@example.test")],
            )
            assert db.get_document("users", "doc-1") == b'{"name":"one"}'
            assert db.find(
                "users", "email", "one@example.test"
            ) == [b"doc-1"]
            report = db.verify()
            assert report["object_count"] >= 3
            assert report["logical_byte_count"] >= len(large)
            assert db.backup(backup) > 0
            compact = db.compact(password="first-password")
            assert compact["raw_entries_after"] <= compact["raw_entries_before"]
            db.migrate(
                1, password="second-password", page_size=8192
            )
            assert db.get_blob("media", "large") == large
        with shibadb.Database.open(
            path, password="second-password"
        ) as reopened:
            assert reopened.get_blob("media", "large") == large
            assert reopened.verify()["object_count"] >= 3
        with shibadb.Database.open(
            backup, password="first-password"
        ) as snapshot:
            assert snapshot.get_blob("media", "large") == large
    test_close_clears_handle_on_error()
    print("python binding tests: ok")

def test_close_clears_handle_on_error() -> None:
    """Regression (adversarial finding IMPORTANT #3): a close that returns a
    non-OK status must still clear the cached handle.

    Once close reaches teardown, it consumes (frees) the native handle even
    when it returns a late error (e.g. a checkpoint-on-close SDB_E_IO).
    SDB_E_INVALID_ARGUMENT and SDB_E_BUSY are early, non-consuming returns.
    The binding used to null self._handle only when status == 0, so a late
    close failure left the freed pointer in place; a later __del__ -> close()
    would then call sdb_database_close on the freed handle — a use-after-free
    / double-free.
    """
    with tempfile.TemporaryDirectory() as directory:
        path = Path(directory) / "close-error.sdb"
        db = shibadb.Database.create(path, password="pw")
        saved_handle = db._handle
        original = shibadb._lib.sdb_database_close
        calls = []

        def fake_close(handle):
            # First emulate the early, non-consuming BUSY return; then emulate
            # a late consuming error. Do not actually free in either case so
            # the saved native handle remains valid for cleanup below.
            calls.append(handle)
            return 13 if len(calls) == 1 else 5

        shibadb._lib.sdb_database_close = fake_close
        try:
            raised = False
            try:
                db.close()
            except Exception:
                raised = True
            assert raised, "BUSY close status must raise"
            assert db._handle, "BUSY close incorrectly consumed the handle"

            raised = False
            try:
                db.close()
            except Exception:
                raised = True
            assert raised, "late close error must raise"
            # A late error occurs after teardown, so the handle must be clear.
            assert not db._handle, "handle not cleared on error close"
            # A second close and a GC/__del__ must NOT re-invoke native on
            # the (already freed) handle.
            db.close()
            db.__del__()
            assert len(calls) == 2, (
                "close retried the native call on a freed handle"
            )
        finally:
            shibadb._lib.sdb_database_close = original
            # fake_close never freed the real handle; free it now so the
            # process does not leak the open database or its file lock.
            original(saved_handle)
    print("python binding: close clears handle on error: ok")

if __name__ == "__main__":
    main()
