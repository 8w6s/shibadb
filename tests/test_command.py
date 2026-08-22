"""Conformance test for the inline command layer (shibadb.command).

Covers the parser in isolation and the executor against a real database, with
emphasis on the failure modes the layer exists to catch: a mistyped path level
returning [] indistinguishably from an empty one, and a parameter value
re-tokenising the command line.
"""

from __future__ import annotations

import itertools
import os
import tempfile

import pytest

from shibadb import Database
from shibadb.command import (
    CommandError,
    CommandSession,
    Namestring,
    parse_depth,
    split_segments,
)


def test_split_segments() -> None:
    assert split_segments("a,b,c") == ["a", "b", "c"]
    assert split_segments("a/b/c") == ["a", "b", "c"]
    assert split_segments(" a , b ") == ["a", "b"]

    # Mixed separators are ambiguous about how many levels were meant.
    with pytest.raises(CommandError):
        split_segments("a,b/c,d")
    with pytest.raises(CommandError):
        split_segments("a,,b")
    with pytest.raises(CommandError):
        split_segments("")


def test_parse_depth() -> None:
    assert parse_depth(">") == 1
    assert parse_depth(">>") == 2
    assert parse_depth(">>>") == 3
    assert parse_depth(">3") == 3
    assert parse_depth(">12") == 12
    assert parse_depth("users") is None

    with pytest.raises(CommandError):
        parse_depth(">0")
    with pytest.raises(CommandError):
        parse_depth(">>2")
    with pytest.raises(CommandError):
        parse_depth(">x")


def test_namestring_prefix() -> None:
    ns = Namestring("R", ["vn", "south", "city", "district"])
    assert ns.prefix() == "vn/south/city/district/"
    assert ns.prefix(1) == "vn/"
    assert ns.prefix(2) == "vn/south/"

    with pytest.raises(CommandError):
        ns.prefix(9)
    with pytest.raises(CommandError):
        ns.prefix(0)


def test_define_is_all_or_nothing() -> None:
    session = CommandSession(None)
    session.define({"a": "x,y"})

    # The second pair is malformed, so neither pair may be installed.
    with pytest.raises(CommandError):
        session.define({"b": "p,q", "c": "bad,,spec"})
    assert "b" not in session.namestrings

    with pytest.raises(CommandError):
        session.define({"a": "other"})
    with pytest.raises(CommandError):
        session.resolve("nope")


def test_extract_ns() -> None:
    assert CommandSession._extract_ns('-ns R "ls users"') == ("R", "ls users")
    assert CommandSession._extract_ns("ls users") == (None, "ls users")
    assert CommandSession._extract_ns("  -ns  R   count  ") == ("R", "count")

    with pytest.raises(CommandError):
        CommandSession._extract_ns("-ns")


def test_bind_placeholders() -> None:
    assert CommandSession._bind(["put", "?"], ("v",)) == ["put", "v"]

    with pytest.raises(CommandError):
        CommandSession._bind(["put", "?"], ())
    with pytest.raises(CommandError):
        CommandSession._bind(["put"], ("v",))


def _fixture(path: str) -> Database:
    database = Database.create(path)
    for region, side in itertools.product(["vn", "jp"], ["south", "north"]):
        for leaf in ["alice", "bob"]:
            database.put(
                "users", f"{region}/{side}/city/district/{leaf}", b"v-" + leaf.encode()
            )
    return database


def test_executor_against_a_real_database() -> None:
    with tempfile.TemporaryDirectory() as directory:
        path = os.path.join(directory, "cmd.sdb")
        with _fixture(path) as database:
            session = CommandSession(database)
            session.define(
                {"R": "vn,south,city,district", "broken": "vn,south,city,NOPE"}
            )

            assert len(session.run('-ns R "ls users"')) == 2
            assert session.run('-ns R "get users alice"') == b"v-alice"
            assert session.run('-ns R "count users"') == 2

            # Truncation widens the prefix one level at a time.
            assert len(session.run('-ns R "ls users >2"')) == 2
            assert len(session.run('-ns R "ls users >1"')) == 4
            assert len(session.run("ls users")) == 8

            assert session.run('-ns R "exists users alice"') is True
            assert session.run('-ns R "exists users ghost"') is False

            session.run('-ns R "put users carol ?"', "v-carol")
            assert session.run('-ns R "get users carol"') == b"v-carol"
            session.run('-ns R "del users carol"')
            assert session.run('-ns R "exists users carol"') is False

            # A value carrying spaces, quotes and the separator must survive
            # binding without re-shaping the command.
            hostile = 'a b "c" d/e'
            session.run('-ns R "put users tricky ?"', hostile)
            assert session.run('-ns R "get users tricky"') == hostile.encode()

            # The point of the layer: a wrong level names the level that broke
            # instead of returning an empty list.
            with pytest.raises(CommandError, match="level 4"):
                session.run('-ns broken "ls users"')

            with pytest.raises(CommandError):
                session.run("ls users >2")
            with pytest.raises(CommandError):
                session.run("drop users")


def test_readonly_session_refuses_writes() -> None:
    with tempfile.TemporaryDirectory() as directory:
        path = os.path.join(directory, "ro.sdb")
        with _fixture(path) as database:
            session = CommandSession(database, readonly=True)
            session.define({"R": "vn,south,city,district"})

            assert len(session.run('-ns R "ls users"')) == 2
            with pytest.raises(CommandError):
                session.run('-ns R "put users x y"')
            with pytest.raises(CommandError):
                session.run('-ns R "del users alice"')


def test_find_uses_a_secondary_index() -> None:
    with tempfile.TemporaryDirectory() as directory:
        path = os.path.join(directory, "docs.sdb")
        with Database.create(path) as database:
            database.create_index("cust", "city")
            database.put_document(
                "cust", "c1", '{"city":"Hanoi"}', terms=[("city", "Hanoi")]
            )
            database.put_document(
                "cust", "c2", '{"city":"Tokyo"}', terms=[("city", "Tokyo")]
            )
            session = CommandSession(database)
            assert session.run("find cust city Hanoi") == [b"c1"]
            assert session.run("find cust city Osaka") == []
