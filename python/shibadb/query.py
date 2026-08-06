"""MongoDB-style document query matching, decoupled from any storage engine.

This is a self-contained port of the query matcher: it takes a decoded document
(a dict) and a query dict, and answers whether the document matches. It has no
dependency on the database — a caller scans a namespace, decodes each value,
and filters with matches_query. Supported operators:

    $or $and $not (logical)
    $gt $lt $gte $lte $ne $in $nin $exists $regex (field-level)

Field names may use dot notation to reach into nested objects: "a.b.c" reads
document["a"]["b"]["c"]. A segment that is missing, or whose parent is not a
dict, resolves to the sentinel _MISSING so $exists is false and every value
comparison fails (matching MongoDB's "missing field" semantics).
"""

from __future__ import annotations

import re
from typing import Any


class _Missing:
    __slots__ = ()

    def __repr__(self) -> str:  # pragma: no cover - debug aid
        return "<missing>"


_MISSING = _Missing()


def _resolve(document: Any, field: str) -> Any:
    """Resolve a possibly dotted field path against a document."""
    if "." not in field:
        if isinstance(document, dict) and field in document:
            return document[field]
        return _MISSING
    current: Any = document
    for segment in field.split("."):
        if not isinstance(current, dict) or segment not in current:
            return _MISSING
        current = current[segment]
    return current


def _match_operators(doc_value: Any, operators: dict) -> bool:
    for op, operand in operators.items():
        if op == "$regex":
            if not isinstance(operand, (str, bytes)) or doc_value is _MISSING:
                return False
            try:
                if not re.search(str(operand), str(doc_value)):
                    return False
            except re.error:
                return False
        elif op == "$gt":
            if doc_value is _MISSING or not _safe_cmp(doc_value, operand, ">"):
                return False
        elif op == "$lt":
            if doc_value is _MISSING or not _safe_cmp(doc_value, operand, "<"):
                return False
        elif op == "$gte":
            if doc_value is _MISSING or not _safe_cmp(doc_value, operand, ">="):
                return False
        elif op == "$lte":
            if doc_value is _MISSING or not _safe_cmp(doc_value, operand, "<="):
                return False
        elif op == "$in":
            if not isinstance(operand, (list, tuple, set)) \
                    or doc_value is _MISSING or doc_value not in operand:
                return False
        elif op == "$nin":
            if not isinstance(operand, (list, tuple, set)):
                return False
            if doc_value is not _MISSING and doc_value in operand:
                return False
        elif op == "$ne":
            if doc_value is not _MISSING and doc_value == operand:
                return False
            if doc_value is _MISSING and operand is None:
                # missing field is treated as absent, not equal to any operand
                pass
        elif op == "$exists":
            exists = doc_value is not _MISSING
            if bool(operand) != exists:
                return False
        elif op == "$not" and isinstance(operand, dict):
            if _match_operators(doc_value, operand):
                return False
        else:
            # Unknown operator: no document can satisfy it.
            return False
    return True


def _safe_cmp(left: Any, right: Any, op: str) -> bool:
    """Ordered comparison that treats a TypeError (incomparable types) as a
    non-match rather than raising, so a query never crashes on mixed data."""
    try:
        if op == ">":
            return left > right
        if op == "<":
            return left < right
        if op == ">=":
            return left >= right
        return left <= right
    except TypeError:
        return False


def matches_query(document: dict, query: dict) -> bool:
    """Return True iff document satisfies the Mongo-style query."""
    if not isinstance(query, dict):
        return False
    for key, condition in query.items():
        if key == "$or" and isinstance(condition, list):
            if not any(matches_query(document, q) for q in condition):
                return False
            continue
        if key == "$and" and isinstance(condition, list):
            if not all(matches_query(document, q) for q in condition):
                return False
            continue
        if key == "$not" and isinstance(condition, dict):
            if matches_query(document, condition):
                return False
            continue

        doc_value = _resolve(document, key)
        if isinstance(condition, dict) and condition \
                and all(str(op).startswith("$") for op in condition):
            if not _match_operators(doc_value, condition):
                return False
        else:
            # Exact match. A missing field never equals a concrete value.
            if doc_value is _MISSING or doc_value != condition:
                return False
    return True
