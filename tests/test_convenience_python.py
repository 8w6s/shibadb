from __future__ import annotations

import struct
import tempfile
from pathlib import Path

import shibadb


def test_exists_and_count() -> None:
    with tempfile.TemporaryDirectory() as directory:
        path = Path(directory) / "conv.sdb"
        with shibadb.Database.create(path) as db:
            assert db.exists("kv", "missing") is False
            db.put("kv", "a", b"1")
            db.put("kv", "b", b"2")
            db.put("kv", "prefixed:x", b"3")
            assert db.exists("kv", "a") is True
            assert db.exists("kv", "missing") is False
            assert db.count("kv") == 3
            assert db.count_prefix("kv", b"prefixed:") == 1
            assert db.count_prefix("kv") == 3
        print("exists + count: ok")


def test_put_if_absent_and_cas() -> None:
    with tempfile.TemporaryDirectory() as directory:
        path = Path(directory) / "cas.sdb"
        with shibadb.Database.create(path) as db:
            assert db.put_if_absent("kv", "k", b"first") is True
            assert db.put_if_absent("kv", "k", b"second") is False
            assert db.get("kv", "k") == b"first"
            # CAS with a wrong expected value must not write.
            assert db.compare_and_swap("kv", "k", b"wrong", b"new") is False
            assert db.get("kv", "k") == b"first"
            # CAS with the right expected value swaps.
            assert db.compare_and_swap("kv", "k", b"first", b"new") is True
            assert db.get("kv", "k") == b"new"
            # CAS on an absent key is a conflict, not a create.
            assert db.compare_and_swap("kv", "absent", b"", b"x") is False
        print("put_if_absent + compare_and_swap: ok")


def test_increment() -> None:
    with tempfile.TemporaryDirectory() as directory:
        path = Path(directory) / "inc.sdb"
        with shibadb.Database.create(path) as db:
            # A missing counter starts at zero.
            assert db.increment("counters", "hits") == 1
            assert db.increment("counters", "hits", 4) == 5
            assert db.increment("counters", "hits", -2) == 3
            # Stored as an 8-byte little-endian counter.
            assert db.get("counters", "hits") == struct.pack("<q", 3)
            # A non-8-byte value is rejected.
            db.put("counters", "bad", b"xyz")
            try:
                db.increment("counters", "bad")
            except shibadb.Error as error:
                assert error.status == 1  # SDB_E_INVALID_ARGUMENT
            else:
                raise AssertionError("expected Error on non-8-byte counter")
        print("increment: ok")


def test_info() -> None:
    with tempfile.TemporaryDirectory() as directory:
        path = Path(directory) / "info.sdb"
        with shibadb.Database.create(path, page_size=8192) as db:
            info = db.info()
            assert info["page_size"] == 8192
            assert info["encrypted"] is False
            assert info["kdf_iterations"] == 0
            gen0 = info["generation"]
            db.put("kv", "a", b"1")
            # A commit bumps the superblock generation.
            assert db.info()["generation"] > gen0
        path2 = Path(directory) / "enc.sdb"
        with shibadb.Database.create(path2, password="secret") as db:
            info = db.info()
            assert info["encrypted"] is True
            assert info["kdf_iterations"] == 600000
        print("info: ok")


def test_options_split() -> None:
    # cache_bytes / synchronous are exposed by splitting the reserved block; a
    # database created with non-default values round-trips and reads back.
    assert shibadb.SDB_SYNCHRONOUS_NORMAL == 1
    with tempfile.TemporaryDirectory() as directory:
        path = Path(directory) / "opt.sdb"
        with shibadb.Database.create(
            path,
            cache_bytes=shibadb.SDB_CACHE_AUTO,
            synchronous=shibadb.SDB_SYNCHRONOUS_NORMAL,
        ) as db:
            db.put("kv", "k", b"v")
            assert db.get("kv", "k") == b"v"
        with shibadb.Database.open(
            path, synchronous=shibadb.SDB_SYNCHRONOUS_NORMAL
        ) as db:
            assert db.get("kv", "k") == b"v"
    print("options split: ok")


def main() -> None:
    test_exists_and_count()
    test_put_if_absent_and_cas()
    test_increment()
    test_info()
    test_options_split()
    print("convenience python: ok")


if __name__ == "__main__":
    main()
