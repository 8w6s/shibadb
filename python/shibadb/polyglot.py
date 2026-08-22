"""
 * Dialect front-ends that lower onto the KV primitives.
 *
 * Each parse() returns a plan dict, never a result: the parser knows nothing
 * about a database. That split keeps the text-handling surface -- the part worth
 * fuzzing -- independent of anything that touches the pager.
 *
 * There is no query planner underneath. A SELECT lowers to an ordered prefix
 * scan plus a client-side filter, so a WHERE clause costs a full namespace walk
 * regardless of how selective it looks. The dialects exist to make external
 * commands expressible, not to imply cost that the engine cannot deliver.
 */
"""

from __future__ import annotations

import json
import re
from typing import Any

_NUMBER = re.compile(r"^-?\d+(\.\d+)?$")


def _literal(text: str) -> Any:
    """Read a SQL literal: quoted string, number, boolean, or bare identifier."""
    token = text.strip()
    if len(token) >= 2 and token[0] == token[-1] and token[0] in "'\"":
        return token[1:-1]
    if _NUMBER.match(token):
        return float(token) if "." in token else int(token)
    lowered = token.lower()
    if lowered in ("true", "false"):
        return lowered == "true"
    if lowered == "null":
        return None
    return token


def _split_outside_quotes(text: str, separator: str) -> list[str]:
    """
     * Split on a separator that is not inside a quoted literal.
     *
     * A naive split breaks on values like 'Sanders AND Sons', which is the exact
     * case where a predicate silently changes meaning rather than failing.
     */
    """
    parts: list[str] = []
    depth = 0
    quote: str | None = None
    current: list[str] = []
    index = 0
    lowered = text.lower()
    needle = separator.lower()
    while index < len(text):
        char = text[index]
        if quote:
            if char == quote:
                quote = None
            current.append(char)
            index += 1
            continue
        if char in "'\"":
            quote = char
            current.append(char)
            index += 1
            continue
        if char == "(":
            depth += 1
        elif char == ")":
            depth -= 1
        if depth == 0 and lowered.startswith(needle, index):
            # Word separators such as AND need a boundary check so a field named
            # "brand" is not split; punctuation separators such as "," do not,
            # and applying the check to them would leave "1, 2" unsplit.
            if needle.isalpha():
                before = index == 0 or not text[index - 1].isalnum()
                after_at = index + len(needle)
                after = after_at >= len(text) or not text[after_at].isalnum()
            else:
                before = after = True
            if before and after:
                parts.append("".join(current))
                current = []
                index += len(needle)
                continue
        current.append(char)
        index += 1
    parts.append("".join(current))
    return [part.strip() for part in parts if part.strip()]


_OPERATORS = [
    (">=", "$gte"),
    ("<=", "$lte"),
    ("!=", "$ne"),
    ("<>", "$ne"),
    (">", "$gt"),
    ("<", "$lt"),
]


def where_to_query(where: str | None) -> dict:
    """
     * Translate a SQL WHERE clause into the matcher dict from query.py.
     *
     * Only conjunctions are handled. OR would need a disjunctive matcher, and
     * silently dropping the second branch of an OR returns a superset of the
     * intended rows -- so an unsupported clause raises instead.
     */
    """
    if not where:
        return {}
    query: dict[str, Any] = {}
    if _split_outside_quotes(where, "OR")[1:]:
        raise ValueError(f"OR is not supported in a WHERE clause: {where!r}")

    for clause in _split_outside_quotes(where, "AND"):
        field, operator, raw = _split_predicate(clause)
        if operator == "=":
            query[field] = _literal(raw)
            continue
        if operator == "IN":
            items = raw.strip().lstrip("(").rstrip(")")
            query[field] = [
                _literal(item) for item in _split_outside_quotes(items, ",")
            ]
            query[field] = {"$in": query[field]}
            continue
        if operator == "LIKE":
            pattern = _literal(raw)
            if not isinstance(pattern, str):
                raise ValueError(f"LIKE needs a string pattern: {clause!r}")
            query[field] = {"$regex": _like_to_regex(pattern)}
            continue
        # Range operators accumulate so "age >= 18 AND age <= 65" becomes one
        # field carrying both bounds rather than the second overwriting the first.
        existing = query.get(field)
        if isinstance(existing, dict):
            existing[operator] = _literal(raw)
        else:
            query[field] = {operator: _literal(raw)}
    return query


def _split_predicate(clause: str) -> tuple[str, str, str]:
    """Break "age >= 30" into field, operator, and raw right-hand side."""
    upper = clause.upper()
    for keyword in (" NOT IN ", " IN ", " LIKE "):
        position = upper.find(keyword)
        if position != -1:
            field = clause[:position].strip()
            raw = clause[position + len(keyword):]
            token = keyword.strip()
            if token == "NOT IN":
                raise ValueError(f"NOT IN is not supported: {clause!r}")
            return field, token, raw

    for symbol, mongo in _OPERATORS:
        position = clause.find(symbol)
        if position != -1:
            return (
                clause[:position].strip(),
                mongo,
                clause[position + len(symbol):],
            )

    position = clause.find("=")
    if position == -1:
        raise ValueError(f"could not parse predicate {clause!r}")
    return clause[:position].strip(), "=", clause[position + 1:]


def _like_to_regex(pattern: str) -> str:
    """Convert SQL LIKE wildcards to an anchored regex."""
    out = ["^"]
    for char in pattern:
        if char == "%":
            out.append(".*")
        elif char == "_":
            out.append(".")
        else:
            out.append(re.escape(char))
    out.append("$")
    return "".join(out)


class PolyglotParser:
    """
     * Recognise a statement's dialect and lower it to a plan dict.
     *
     * Dialects are probed in order of how specific their prefix is, so a Mongo
     * call ("db.x.find(...)") is never mistaken for a Redis command whose verb
     * happens to start the line.
     */
    """

    def parse(self, statement: str) -> dict | None:
        text = statement.strip().rstrip(";").strip()
        if not text:
            return None
        for handler in (
            self._parse_mongo,
            self._parse_sql,
            self._parse_cql,
            self._parse_redis,
        ):
            plan = handler(text)
            if plan is not None:
                return plan
        return None

    # ---- SQL --------------------------------------------------------------

    def _parse_sql(self, text: str) -> dict | None:
        upper = text.upper()
        if upper.startswith("SELECT "):
            return self._parse_select(text)
        if upper.startswith("INSERT INTO "):
            return self._parse_insert(text)
        if upper.startswith("UPDATE "):
            return self._parse_update(text)
        if upper.startswith("DELETE FROM "):
            return self._parse_delete(text)
        return None

    @staticmethod
    def _take_clause(text: str, keyword: str) -> tuple[str, str | None]:
        """Split off a trailing clause, returning (head, clause_or_None)."""
        match = re.search(rf"\s{keyword}\s+", text, re.IGNORECASE)
        if match is None:
            return text, None
        return text[: match.start()], text[match.end():].strip()

    def _parse_select(self, text: str) -> dict:
        body = text[len("SELECT "):]
        columns, _, remainder = body.partition(" FROM ")
        if not remainder:
            raise ValueError(f"SELECT needs a FROM clause: {text!r}")

        remainder, offset = self._take_clause(remainder, "OFFSET")
        remainder, limit = self._take_clause(remainder, "LIMIT")
        remainder, order_by = self._take_clause(remainder, "ORDER BY")
        table, where = self._take_clause(remainder, "WHERE")

        return {
            "lang": "sql",
            "op": "select",
            "columns": columns.strip(),
            "table": table.strip(),
            "where": where,
            "order_by": order_by,
            "limit": int(limit) if limit else None,
            "offset": int(offset) if offset else None,
        }

    def _parse_delete(self, text: str) -> dict:
        remainder = text[len("DELETE FROM "):]
        table, where = self._take_clause(remainder, "WHERE")
        return {
            "lang": "sql",
            "op": "delete",
            "table": table.strip(),
            "where": where,
        }

    def _parse_insert(self, text: str) -> dict:
        remainder = text[len("INSERT INTO "):].strip()
        table, _, rest = remainder.partition(" ")
        rest = rest.strip()

        # JSON form: INSERT INTO t json '{...}'
        if rest.lower().startswith("json "):
            payload = _literal(rest[len("json "):].strip())
            if isinstance(payload, str):
                payload = json.loads(payload)
            if not isinstance(payload, dict):
                raise ValueError(f"INSERT json needs an object: {text!r}")
            return {"lang": "sql", "op": "insert", "table": table, "data": payload}

        if not rest.startswith("("):
            raise ValueError(f"could not parse INSERT: {text!r}")
        close = rest.index(")")
        columns = _split_outside_quotes(rest[1:close], ",")
        values_part = rest[close + 1:].strip()
        if not values_part.upper().startswith("VALUES"):
            raise ValueError(f"INSERT needs a VALUES clause: {text!r}")
        tuple_part = values_part[len("VALUES"):].strip()
        if not (tuple_part.startswith("(") and tuple_part.endswith(")")):
            raise ValueError(f"malformed VALUES tuple: {text!r}")
        values = [
            _literal(item)
            for item in _split_outside_quotes(tuple_part[1:-1], ",")
        ]
        if len(columns) != len(values):
            raise ValueError(
                f"INSERT has {len(columns)} column(s) but {len(values)} value(s)"
            )
        return {
            "lang": "sql",
            "op": "insert",
            "table": table,
            "data": dict(zip(columns, values)),
        }

    def _parse_update(self, text: str) -> dict:
        remainder = text[len("UPDATE "):]
        head, where = self._take_clause(remainder, "WHERE")
        table, _, assignments = head.partition(" SET ")
        if not assignments:
            raise ValueError(f"UPDATE needs a SET clause: {text!r}")
        data: dict[str, Any] = {}
        for pair in _split_outside_quotes(assignments, ","):
            field, _, raw = pair.partition("=")
            if not raw:
                raise ValueError(f"malformed assignment {pair!r}")
            data[field.strip()] = _literal(raw)
        return {
            "lang": "sql",
            "op": "update",
            "table": table.strip(),
            "where": where,
            "data": data,
        }

    # ---- Mongo ------------------------------------------------------------

    _MONGO = re.compile(r"^db\.(?P<collection>[A-Za-z_][\w-]*)\.(?P<op>\w+)\((?P<args>.*)\)$", re.DOTALL)

    def _parse_mongo(self, text: str) -> dict | None:
        match = self._MONGO.match(text)
        if match is None:
            return None
        raw = match.group("args").strip()
        args: Any = {}
        if raw:
            # Only the first argument is honoured; a projection or options object
            # would imply behaviour this layer does not implement, so accepting
            # it silently would overpromise.
            args = json.loads(raw) if raw.startswith("{") else _literal(raw)
        return {
            "lang": "mongo",
            "collection": match.group("collection"),
            "op": match.group("op"),
            "args": args,
        }

    # ---- CQL / Redis ------------------------------------------------------

    def _parse_cql(self, text: str) -> dict | None:
        upper = text.upper()
        if upper.startswith("TRUNCATE "):
            return {
                "lang": "cql",
                "op": "truncate",
                "table": text[len("TRUNCATE "):].strip(),
            }
        return None

    _REDIS_VERBS = frozenset({"SET", "GET", "DEL", "EXISTS", "INCR", "DECR"})

    def _parse_redis(self, text: str) -> dict | None:
        parts = text.split(None, 2)
        verb = parts[0].upper()
        if verb not in self._REDIS_VERBS:
            return None
        plan: dict[str, Any] = {"lang": "redis", "op": verb}
        if len(parts) > 1:
            plan["key"] = parts[1]
        if len(parts) > 2:
            plan["value"] = parts[2]
        return plan
