"""Polyglot query parsing: turn a SQL / Mongo / Redis / CQL / DynamoDB query
string into a plan dict, and turn a SQL WHERE clause into a matcher query
(see shibadb.query.matches_query).

This is a self-contained port — it depends only on re/json/shlex, never on the
storage engine. Database.query executes the SELECT plans it produces on top of
scan + find_query; the other dialects/ops parse into plans a caller can act on.
"""

from __future__ import annotations

import json
import re
import shlex
from typing import Any


def _clean_value(value: str) -> Any:
    """Coerce a SQL literal token to a Python value."""
    if value.isdigit():
        return int(value)
    if value.replace(".", "", 1).isdigit():
        return float(value)
    if len(value) >= 2 and value.startswith("'") and value.endswith("'"):
        return value[1:-1]
    lowered = value.lower()
    if lowered == "true":
        return True
    if lowered == "false":
        return False
    if lowered == "null":
        return None
    return value


def _split_top_level_and(where_clause: str) -> list[str]:
    """Split on ` AND ` (case-insensitive) but not inside a single-quoted
    string literal, so `name = 'Sanders AND Sons'` stays one predicate."""
    parts: list[str] = []
    buf: list[str] = []
    index = 0
    length = len(where_clause)
    in_string = False
    while index < length:
        char = where_clause[index]
        if char == "'":
            in_string = not in_string
            buf.append(char)
            index += 1
            continue
        if (not in_string and index + 5 <= length
                and where_clause[index:index + 5].upper() == " AND "):
            parts.append("".join(buf))
            buf = []
            index += 5
            continue
        buf.append(char)
        index += 1
    if buf:
        parts.append("".join(buf))
    return parts


def where_to_query(where_clause: str | None) -> dict:
    """Convert a SQL WHERE clause into a matches_query matcher dict. Supports
    =, !=/<>, >, <, >=, <=, LIKE (% -> .*, _ -> .), IN (...), joined by AND."""
    query: dict[str, Any] = {}
    if not where_clause:
        return query

    def add_op(field: str, operator: str, value: Any) -> None:
        existing = query.get(field)
        if isinstance(existing, dict):
            existing[operator] = value
        elif field in query:
            query[field] = {"$eq": existing, operator: value}
        else:
            query[field] = {operator: value}

    for raw in _split_top_level_and(where_clause):
        part = raw.strip()
        if not part:
            continue
        if ">=" in part:
            k, v = (x.strip() for x in part.split(">=", 1))
            add_op(k, "$gte", _clean_value(v))
        elif "<=" in part:
            k, v = (x.strip() for x in part.split("<=", 1))
            add_op(k, "$lte", _clean_value(v))
        elif "!=" in part or "<>" in part:
            token = "!=" if "!=" in part else "<>"
            k, v = (x.strip() for x in part.split(token, 1))
            add_op(k, "$ne", _clean_value(v))
        elif " like " in part.lower():
            try:
                k, v = re.split(r"\s+like\s+", part, flags=re.IGNORECASE, maxsplit=1)
                literal = _clean_value(v.strip())
                pattern = "^" + re.escape(str(literal)).replace(
                    "%", ".*"
                ).replace("_", ".") + "$"
                add_op(k.strip(), "$regex", pattern)
            except (ValueError, re.error):
                continue
        elif " in " in part.lower():
            match = re.match(r"(\w+)\s+in\s*\((.*)\)", part, re.IGNORECASE)
            if match:
                field, values = match.groups()
                add_op(
                    field,
                    "$in",
                    [_clean_value(x.strip()) for x in values.split(",")],
                )
        elif "=" in part:
            k, v = (x.strip() for x in part.split("=", 1))
            query[k] = _clean_value(v)
        elif ">" in part:
            k, v = (x.strip() for x in part.split(">", 1))
            add_op(k, "$gt", _clean_value(v))
        elif "<" in part:
            k, v = (x.strip() for x in part.split("<", 1))
            add_op(k, "$lt", _clean_value(v))
    return query


class PolyglotParser:
    """Parse a query string in one of several dialects into a plan dict."""

    def parse(self, query: str) -> dict | None:
        query = query.strip()
        if not query:
            return None
        upper = query.upper()
        if upper.startswith((
            "SELECT", "INSERT", "UPDATE", "DELETE", "CREATE", "DROP", "ALTER",
        )):
            return self._parse_sql(query)
        if query.startswith("db."):
            return self._parse_mongo(query)
        head = upper.split()[0] if upper.split() else ""
        if head in (
            "GET", "SET", "DEL", "KEYS", "HGET", "HSET", "HGETALL", "HDEL",
            "EXISTS", "LPUSH", "RPUSH", "LPOP", "RPOP", "LLEN", "LRANGE",
        ):
            return self._parse_redis(query)
        if upper.startswith(("DESCRIBE", "USE", "TRUNCATE")):
            return self._parse_cql(query)
        if upper.startswith(("GET-ITEM", "PUT-ITEM", "SCAN", "QUERY")):
            return self._parse_dynamo(query)
        if "=" in query:
            return {"type": "search", "query": query}
        return None

    def _parse_sql(self, query: str) -> dict | None:
        lower = query.lower()
        if lower.startswith("select"):
            pattern = (
                r"select\s+(.*?)\s+from\s+(\w+)(?:\s+where\s+(.*?))?"
                r"(?:\s+order\s+by\s+(.*?))?(?:\s+limit\s+(\d+))?"
                r"(?:\s+offset\s+(\d+))?$"
            )
            match = re.match(pattern, query, re.IGNORECASE | re.DOTALL)
            if match:
                fields, table, where, order_by, limit, offset = match.groups()
                return {
                    "lang": "sql", "op": "select", "table": table,
                    "fields": fields.strip(),
                    "where": where.strip() if where else None,
                    "order_by": order_by.strip() if order_by else None,
                    "limit": int(limit) if limit else None,
                    "offset": int(offset) if offset else None,
                }
        elif lower.startswith("delete"):
            fixed = re.sub(
                r"delete\s+\*\s+from", "delete from", query, flags=re.IGNORECASE
            )
            match = re.match(
                r"delete\s+from\s+(\w+)(?:\s+where\s+(.*))?", fixed,
                re.IGNORECASE,
            )
            if match:
                table, where = match.groups()
                return {
                    "lang": "sql", "op": "delete", "table": table,
                    "where": where.strip() if where else None,
                }
        elif lower.startswith("update"):
            match = re.match(
                r"update\s+(\w+)\s+set\s+(.*?)(?:\s+where\s+(.*))?$", query,
                re.IGNORECASE,
            )
            if match:
                table, sets, where = match.groups()
                return {
                    "lang": "sql", "op": "update", "table": table,
                    "data": self._parse_assignments(sets),
                    "where": where.strip() if where else None,
                }
        elif lower.startswith("insert"):
            match_json = re.match(
                r"insert\s+into\s+(\w+)\s+json\s+'(.*)'", query, re.IGNORECASE
            )
            if match_json:
                table, json_str = match_json.groups()
                for candidate in (json_str, json_str.replace("'", '"')):
                    try:
                        return {
                            "lang": "sql", "op": "insert", "table": table,
                            "data": json.loads(candidate),
                        }
                    except json.JSONDecodeError:
                        continue
            match_values = re.match(
                r"insert\s+into\s+(\w+)\s*\((.*?)\)\s*values\s*\((.*?)\)",
                query, re.IGNORECASE,
            )
            if match_values:
                table, cols_str, vals_str = match_values.groups()
                cols = [c.strip() for c in cols_str.split(",")]
                vals = [_clean_value(v.strip()) for v in vals_str.split(",")]
                if len(cols) == len(vals):
                    return {
                        "lang": "sql", "op": "insert", "table": table,
                        "data": dict(zip(cols, vals)),
                    }
        return None

    def _parse_mongo(self, query: str) -> dict | None:
        match = re.match(r"db\.(\w+)\.(\w+)\((.*)\)", query)
        if not match:
            return None
        collection, op, args = match.groups()
        arg_data: Any = {}
        if args.strip():
            candidate = args
            if "'" in candidate and '"' not in candidate:
                candidate = candidate.replace("'", '"')
            try:
                arg_data = json.loads(candidate)
            except (json.JSONDecodeError, ValueError):
                arg_data = {}
        return {
            "lang": "mongo", "collection": collection, "op": op,
            "args": arg_data,
        }

    def _parse_redis(self, query: str) -> dict | None:
        parts = shlex.split(query)
        if not parts:
            return None
        op = parts[0].upper()
        cmd: dict[str, Any] = {"lang": "redis", "op": op}
        if len(parts) > 1:
            cmd["key"] = parts[1]
        if len(parts) > 2:
            if op == "SET":
                cmd["value"] = parts[2]
            elif op == "HSET":
                cmd["field"] = parts[2]
                if len(parts) > 3:
                    cmd["value"] = parts[3]
            elif op in ("HGET", "HDEL", "HEXISTS"):
                cmd["field"] = parts[2]
            elif op in ("LPUSH", "RPUSH"):
                cmd["values"] = parts[2:]
        return cmd

    def _parse_cql(self, query: str) -> dict | None:
        if query.upper().startswith("TRUNCATE"):
            match = re.match(r"truncate\s+(\w+)", query, re.IGNORECASE)
            if match:
                return {"lang": "cql", "op": "truncate", "table": match.group(1)}
        return None

    def _parse_dynamo(self, query: str) -> dict | None:
        parts = shlex.split(query)
        if not parts:
            return None
        cmd: dict[str, Any] = {"lang": "dynamo", "op": parts[0].lower()}
        it = iter(parts[1:])
        for arg in it:
            if arg.startswith("--"):
                try:
                    cmd[arg[2:]] = next(it)
                except StopIteration:
                    pass
        return cmd

    def _parse_assignments(self, sets_str: str) -> dict:
        data: dict[str, Any] = {}
        for assignment in sets_str.split(","):
            if "=" in assignment:
                k, v = (x.strip() for x in assignment.split("=", 1))
                data[k] = _clean_value(v)
        return data
