from __future__ import annotations

import json
import tempfile
from pathlib import Path

import shibadb
from shibadb.query import matches_query


def test_matcher_operators() -> None:
    doc = {"name": "alice", "age": 30, "tags": ["a", "b"], "addr": {"city": "x"}}
    # exact match
    assert matches_query(doc, {"name": "alice"})
    assert not matches_query(doc, {"name": "bob"})
    # comparison
    assert matches_query(doc, {"age": {"$gt": 20}})
    assert matches_query(doc, {"age": {"$gte": 30, "$lt": 40}})
    assert not matches_query(doc, {"age": {"$gt": 30}})
    # $in / $nin / $ne
    assert matches_query(doc, {"name": {"$in": ["alice", "eve"]}})
    assert matches_query(doc, {"name": {"$nin": ["bob"]}})
    assert matches_query(doc, {"name": {"$ne": "bob"}})
    assert not matches_query(doc, {"name": {"$ne": "alice"}})
    # $exists
    assert matches_query(doc, {"name": {"$exists": True}})
    assert matches_query(doc, {"missing": {"$exists": False}})
    assert not matches_query(doc, {"missing": {"$exists": True}})
    # $regex
    assert matches_query(doc, {"name": {"$regex": "^al"}})
    assert not matches_query(doc, {"name": {"$regex": "^bo"}})
    # logical
    assert matches_query(doc, {"$or": [{"name": "bob"}, {"age": 30}]})
    assert matches_query(doc, {"$and": [{"name": "alice"}, {"age": 30}]})
    assert matches_query(doc, {"$not": {"name": "bob"}})
    assert not matches_query(doc, {"$not": {"name": "alice"}})
    # dot notation into nested object (improvement over the HVPDB original)
    assert matches_query(doc, {"addr.city": "x"})
    assert not matches_query(doc, {"addr.city": "y"})
    assert matches_query(doc, {"addr.zip": {"$exists": False}})
    # missing field never matches a concrete value
    assert not matches_query(doc, {"missing": "anything"})
    print("matcher operators: ok")


def test_scan_and_find_query() -> None:
    with tempfile.TemporaryDirectory() as directory:
        path = Path(directory) / "scan.sdb"
        with shibadb.Database.create(path) as db:
            db.put("users", "user:1", json.dumps({"name": "alice", "age": 30}))
            db.put("users", "user:2", json.dumps({"name": "bob", "age": 25}))
            db.put("users", "post:1", json.dumps({"title": "hi"}))
            db.put("other", "user:9", json.dumps({"name": "zzz"}))

            # scan whole namespace, ascending key order, isolated
            pairs = db.scan("users")
            keys = [k for k, _ in pairs]
            assert keys == [b"post:1", b"user:1", b"user:2"], keys

            # prefix scan
            prefixed = db.scan("users", b"user:")
            assert [k for k, _ in prefixed] == [b"user:1", b"user:2"]

            # find_query: client-side JSON filter over the ordered scan
            found = db.find_query("users", {"age": {"$gte": 30}})
            assert len(found) == 1
            assert found[0][0] == b"user:1"
            assert found[0][1]["name"] == "alice"

            found2 = db.find_query("users", {"name": {"$regex": "^b"}})
            assert [k for k, _ in found2] == [b"user:2"]

            # prefix + query combined
            none = db.find_query("users", {"age": {"$lt": 0}})
            assert none == []
        print("scan + find_query: ok")


def test_resolve_ref() -> None:
    with tempfile.TemporaryDirectory() as directory:
        path = Path(directory) / "ref.sdb"
        with shibadb.Database.create(path) as db:
            db.put("authors", "a1", json.dumps({"name": "alice"}))
            assert db.resolve_ref({"$ref": "authors", "$id": "a1"}) == {
                "name": "alice"
            }
            assert db.resolve_ref({"$ref": "authors", "$id": "missing"}) is None
            assert db.resolve_ref({"malformed": True}) is None
            raw = db.resolve_ref(
                {"$ref": "authors", "$id": "a1"}, as_json=False
            )
            assert json.loads(raw) == {"name": "alice"}
        print("resolve_ref: ok")


def test_list_namespaces() -> None:
    with tempfile.TemporaryDirectory() as directory:
        path = Path(directory) / "ns.sdb"
        with shibadb.Database.create(path) as db:
            db.put("users", "u1", "a")
            db.put("users", "u2", "b")  # duplicate namespace -> one entry
            db.put("logs", "l1", "c")
            db.put_blob("files", "f1", b"x")
            names = db.list_namespaces()
            # KV (kind 1) before blob (kind 2); within KV, logs before users
            assert names == [
                (1, b"logs"),
                (1, b"users"),
                (2, b"files"),
            ], names
        print("list_namespaces: ok")


def main() -> None:
    test_matcher_operators()
    test_scan_and_find_query()
    test_resolve_ref()
    test_list_namespaces()
    print("scan/query python: ok")


if __name__ == "__main__":
    main()
