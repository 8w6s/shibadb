from __future__ import annotations

import json
import tempfile
from pathlib import Path

import shibadb
from shibadb.polyglot import PolyglotParser, where_to_query


def test_parser_dialects() -> None:
    p = PolyglotParser()

    sel = p.parse("SELECT * FROM users WHERE age > 30 ORDER BY name DESC LIMIT 5 OFFSET 2")
    assert sel["lang"] == "sql" and sel["op"] == "select"
    assert sel["table"] == "users" and sel["where"] == "age > 30"
    assert sel["order_by"] == "name DESC" and sel["limit"] == 5 and sel["offset"] == 2

    ins = p.parse("INSERT INTO users (name, age) VALUES ('bob', 25)")
    assert ins["op"] == "insert" and ins["data"] == {"name": "bob", "age": 25}

    dele = p.parse("DELETE FROM users WHERE name = 'bob'")
    assert dele["op"] == "delete" and dele["table"] == "users"

    mon = p.parse('db.users.find({"age": 30})')
    assert mon["lang"] == "mongo" and mon["collection"] == "users"
    assert mon["op"] == "find" and mon["args"] == {"age": 30}

    red = p.parse("SET mykey myval")
    assert red["lang"] == "redis" and red["op"] == "SET"
    assert red["key"] == "mykey" and red["value"] == "myval"

    assert p.parse("TRUNCATE logs") == {"lang": "cql", "op": "truncate", "table": "logs"}
    assert p.parse("") is None


def test_where_to_query() -> None:
    assert where_to_query("age > 30") == {"age": {"$gt": 30}}
    assert where_to_query("age >= 18 AND age <= 65") == {
        "age": {"$gte": 18, "$lte": 65}
    }
    assert where_to_query("name = 'alice'") == {"name": "alice"}
    assert where_to_query("name != 'bob'") == {"name": {"$ne": "bob"}}
    assert where_to_query("status IN (1, 2, 3)") == {"status": {"$in": [1, 2, 3]}}
    assert where_to_query("name LIKE 'al%'") == {"name": {"$regex": "^al.*$"}}
    # AND inside a quoted literal must not split
    assert where_to_query("name = 'Sanders AND Sons'") == {
        "name": "Sanders AND Sons"
    }
    assert where_to_query(None) == {}


def test_query_select() -> None:
    with tempfile.TemporaryDirectory() as directory:
        path = Path(directory) / "q.sdb"
        with shibadb.Database.create(path) as db:
            db.put("users", "u1", json.dumps({"name": "alice", "age": 30}))
            db.put("users", "u2", json.dumps({"name": "bob", "age": 25}))
            db.put("users", "u3", json.dumps({"name": "carol", "age": 42}))

            rows = db.query("SELECT * FROM users WHERE age >= 30")
            names = sorted(r["name"] for r in rows)
            assert names == ["alice", "carol"], names

            ordered = db.query("SELECT * FROM users ORDER BY age DESC LIMIT 2")
            assert [r["name"] for r in ordered] == ["carol", "alice"]

            offset = db.query("SELECT * FROM users ORDER BY age ASC OFFSET 1")
            assert [r["name"] for r in offset] == ["alice", "carol"]

            none = db.query("SELECT * FROM users WHERE name LIKE 'z%'")
            assert none == []

        print("query select: ok")


def test_query_crud() -> None:
    with tempfile.TemporaryDirectory() as directory:
        path = Path(directory) / "crud.sdb"
        with shibadb.Database.create(path) as db:
            # INSERT (VALUES form) uses the _id column as the key
            key = db.query(
                "INSERT INTO users (_id, name, age) VALUES ('u1', 'dave', 50)"
            )
            assert key == b"u1", key
            db.query("INSERT INTO users (_id, name, age) VALUES ('u2', 'eve', 22)")

            # INSERT (JSON form)
            db.query(
                'INSERT INTO users json \'{"_id": "u3", "name": "finn", "age": 40}\''
            )

            rows = db.query("SELECT * FROM users WHERE age >= 40")
            assert sorted(r["name"] for r in rows) == ["dave", "finn"]

            # INSERT requires _id
            try:
                db.query("INSERT INTO users (name) VALUES ('nokey')")
            except ValueError:
                pass
            else:
                raise AssertionError("INSERT without _id must fail")

            # UPDATE merges SET fields into matches
            affected = db.query("UPDATE users SET age = 51 WHERE name = 'dave'")
            assert affected == 1
            dave = db.query("SELECT * FROM users WHERE name = 'dave'")
            assert dave[0]["age"] == 51 and dave[0]["_id"] == "u1"

            # DELETE by predicate
            deleted = db.query("DELETE FROM users WHERE age < 30")
            assert deleted == 1
            assert db.query("SELECT * FROM users WHERE name = 'eve'") == []

            # remaining rows intact
            names = sorted(r["name"] for r in db.query("SELECT * FROM users"))
            assert names == ["dave", "finn"]
        print("query crud: ok")


def test_query_mongo() -> None:
    with tempfile.TemporaryDirectory() as directory:
        path = Path(directory) / "mongo.sdb"
        with shibadb.Database.create(path) as db:
            db.query('db.users.insertOne({"_id": "u1", "name": "alice", "age": 30})')
            db.query('db.users.insertOne({"_id": "u2", "name": "bob", "age": 25})')

            assert db.query("db.users.count()") == 2

            found = db.query('db.users.find({"age": {"$gte": 30}})')
            assert [d["name"] for d in found] == ["alice"]

            one = db.query('db.users.findOne({"name": "bob"})')
            assert one["_id"] == "u2"
            assert db.query('db.users.findOne({"name": "zzz"})') is None

            deleted = db.query('db.users.deleteMany({"age": {"$lt": 28}})')
            assert deleted == 1
            assert db.query("db.users.count()") == 1

    print("query mongo: ok")


def main() -> None:
    test_parser_dialects()
    test_where_to_query()
    test_query_select()
    test_query_crud()
    test_query_mongo()
    print("polyglot: ok")


if __name__ == "__main__":
    main()
