"""A small command-line interface for ShibaDB.

Usage:
    shibadb <database> [--password PW] <command> [args...]

Commands:
    create                         create a new database (then exit)
    put <ns> <key> <value>         store a KV value
    get <ns> <key>                 print a KV value
    del <ns> <key>                 delete a KV value
    scan <ns> [--prefix P]         print key=value for a namespace
    find <ns> <query-json>         print matches of a Mongo-style query
    namespaces                     list "kind<TAB>namespace" for populated ones
    stats                          print verify() counters
    backup <dest> [--replace]      copy the database to dest
    compact                        compact the database in place

Keys and values are treated as UTF-8 text on input; on output, values that
are not valid UTF-8 are shown as a repr of their bytes. The database is opened
for each invocation and closed on exit, so the CLI is safe to script.
"""

from __future__ import annotations

import argparse
import json
import sys

import shibadb


def _show(value: bytes) -> str:
    try:
        return value.decode("utf-8")
    except UnicodeDecodeError:
        return repr(value)


def _open(args, *, create: bool = False) -> shibadb.Database:
    opener = shibadb.Database.create if create else shibadb.Database.open
    return opener(args.database, password=args.password)


def _cmd_create(args) -> int:
    _open(args, create=True).close()
    return 0


def _cmd_put(args) -> int:
    with _open(args) as db:
        db.put(args.namespace, args.key, args.value)
    return 0


def _cmd_get(args) -> int:
    with _open(args) as db:
        try:
            value = db.get(args.namespace, args.key)
        except shibadb.NotFoundError:
            print(f"error: {args.key!r} not found", file=sys.stderr)
            return 1
    print(_show(value))
    return 0


def _cmd_del(args) -> int:
    with _open(args) as db:
        try:
            db.delete(args.namespace, args.key)
        except shibadb.NotFoundError:
            print(f"error: {args.key!r} not found", file=sys.stderr)
            return 1
    return 0


def _cmd_scan(args) -> int:
    prefix = args.prefix.encode("utf-8") if args.prefix else b""
    with _open(args) as db:
        for key, value in db.scan(args.namespace, prefix):
            print(f"{_show(key)}={_show(value)}")
    return 0


def _cmd_find(args) -> int:
    try:
        query = json.loads(args.query)
    except json.JSONDecodeError as error:
        print(f"error: invalid query JSON: {error}", file=sys.stderr)
        return 2
    if not isinstance(query, dict):
        print("error: query must be a JSON object", file=sys.stderr)
        return 2
    with _open(args) as db:
        for key, document in db.find_query(args.namespace, query):
            print(f"{_show(key)}\t{json.dumps(document)}")
    return 0


def _cmd_query(args) -> int:
    with _open(args) as db:
        try:
            result = db.query(args.statement)
        except (ValueError, NotImplementedError) as error:
            print(f"error: {error}", file=sys.stderr)
            return 2
    # SELECT hands back rows; INSERT a key; UPDATE/DELETE a count. Print rows
    # one JSON object per line so the output pipes into jq like find does.
    if isinstance(result, list):
        for document in result:
            print(json.dumps(document))
    elif isinstance(result, (bytes, bytearray)):
        print(_show(bytes(result)))
    else:
        print(result)
    return 0


_KIND_NAMES = {1: "kv", 2: "blob", 3: "document"}


def _cmd_namespaces(args) -> int:
    with _open(args) as db:
        for kind, name in db.list_namespaces():
            print(f"{_KIND_NAMES.get(kind, kind)}\t{_show(name)}")
    return 0


def _cmd_stats(args) -> int:
    with _open(args) as db:
        stats = db.verify()
    for name in sorted(stats):
        print(f"{name}={stats[name]}")
    return 0


def _cmd_backup(args) -> int:
    with _open(args) as db:
        written = db.backup(args.destination, replace=args.replace)
    print(f"{written}")
    return 0


def _cmd_compact(args) -> int:
    with _open(args) as db:
        db.compact(password=args.password)
    return 0


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(prog="shibadb", description=__doc__.split("\n")[0])
    parser.add_argument("database", help="path to the database file")
    parser.add_argument("--password", default=None, help="encryption password")
    sub = parser.add_subparsers(dest="command", required=True)

    sub.add_parser("create").set_defaults(func=_cmd_create)

    put = sub.add_parser("put")
    put.add_argument("namespace")
    put.add_argument("key")
    put.add_argument("value")
    put.set_defaults(func=_cmd_put)

    for name, func in (("get", _cmd_get), ("del", _cmd_del)):
        parser_kv = sub.add_parser(name)
        parser_kv.add_argument("namespace")
        parser_kv.add_argument("key")
        parser_kv.set_defaults(func=func)

    scan = sub.add_parser("scan")
    scan.add_argument("namespace")
    scan.add_argument("--prefix", default=None)
    scan.set_defaults(func=_cmd_scan)

    find = sub.add_parser("find")
    find.add_argument("namespace")
    find.add_argument("query", help="Mongo-style query as a JSON object")
    find.set_defaults(func=_cmd_find)

    query = sub.add_parser("query")
    query.add_argument("statement", help="SQL or Mongo-style statement")
    query.set_defaults(func=_cmd_query)

    sub.add_parser("namespaces").set_defaults(func=_cmd_namespaces)

    sub.add_parser("stats").set_defaults(func=_cmd_stats)

    backup = sub.add_parser("backup")
    backup.add_argument("destination")
    backup.add_argument("--replace", action="store_true")
    backup.set_defaults(func=_cmd_backup)

    sub.add_parser("compact").set_defaults(func=_cmd_compact)
    return parser


def main(argv: list[str] | None = None) -> int:
    parser = _build_parser()
    args = parser.parse_args(argv)
    try:
        return args.func(args)
    except shibadb.Error as error:
        print(f"error: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
