"""
 * Client-side document matcher for KV namespaces holding JSON values.
 *
 * The engine indexes keys, not value contents: a secondary index answers exact
 * equality on a document term and nothing else. Anything richer -- ranges, set
 * membership, negation -- has to be evaluated after the rows come back, so this
 * module is deliberately a filter over an ordered scan rather than anything that
 * pretends to be a query planner.
 *
 * The operator vocabulary is Mongo's because it is already the shape callers
 * reach for, and because it maps cleanly onto SQL predicates when polyglot.py
 * translates a WHERE clause.
 */
"""

from __future__ import annotations

import re
from typing import Any

_COMPARATORS = {
    "$gt": lambda value, want: value > want,
    "$gte": lambda value, want: value >= want,
    "$lt": lambda value, want: value < want,
    "$lte": lambda value, want: value <= want,
}


def _compare(operator: str, value: Any, want: Any) -> bool:
    """
     * Apply one comparison operator, treating type mismatch as "no match".
     *
     * JSON documents in a schemaless store routinely disagree about a field's
     * type, and Python raises TypeError on int > str. A raise here would abort
     * the whole scan over one malformed row, so an incomparable pair simply
     * fails to match.
     */
    """
    compare = _COMPARATORS.get(operator)
    if compare is None:
        return False
    try:
        return compare(value, want)
    except TypeError:
        return False


def _matches_operators(value: Any, criteria: dict) -> bool:
    for operator, want in criteria.items():
        if operator in _COMPARATORS:
            if not _compare(operator, value, want):
                return False
        elif operator == "$ne":
            if value == want:
                return False
        elif operator == "$in":
            if value not in want:
                return False
        elif operator == "$nin":
            if value in want:
                return False
        elif operator == "$exists":
            # Absence is modelled by the caller passing _MISSING, so a present
            # value satisfies $exists: True and fails $exists: False.
            present = value is not _MISSING
            if bool(want) != present:
                return False
        elif operator == "$regex":
            if not isinstance(value, str):
                return False
            if re.search(want, value) is None:
                return False
        else:
            # An unknown operator must not silently widen the result set.
            return False
    return True


class _Missing:
    """Sentinel for a field the document does not carry."""

    __slots__ = ()

    def __repr__(self) -> str:
        return "<missing>"


_MISSING = _Missing()


def matches_query(document: dict, query: dict) -> bool:
    """
     * Test one document against a Mongo-style query dict.
     *
     * Top-level keys are ANDed. A plain value means equality; a dict value is
     * read as an operator map. Dotted field names walk into nested objects.
     */
    """
    if not query:
        return True
    for field, criteria in query.items():
        # Logical combinators take a whole sub-query rather than a field value,
        # so they are dispatched before any field lookup happens.
        if field == "$or":
            if not any(matches_query(document, branch) for branch in criteria):
                return False
            continue
        if field == "$and":
            if not all(matches_query(document, branch) for branch in criteria):
                return False
            continue
        if field == "$not":
            if matches_query(document, criteria):
                return False
            continue

        value = _lookup(document, field)
        if isinstance(criteria, dict) and any(
            key.startswith("$") for key in criteria
        ):
            if not _matches_operators(value, criteria):
                return False
        elif value is _MISSING or value != criteria:
            return False
    return True


def _lookup(document: dict, field: str) -> Any:
    """Resolve "a.b.c" against nested dicts, returning _MISSING if absent."""
    current: Any = document
    for part in field.split("."):
        if not isinstance(current, dict) or part not in current:
            return _MISSING
        current = current[part]
    return current
