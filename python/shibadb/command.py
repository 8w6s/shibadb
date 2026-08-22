"""
 * Inline command layer over the Python binding.
 *
 * A command is one line of text: a verb, then space-separated tokens. There is
 * no expression grammar and no query planner behind it -- every command maps
 * onto exactly one binding call, so the surface stays small enough to fuzz and
 * the cost of a command stays legible from the text.
 *
 * Paths live in the KEY, never in the namespace. The engine matches a namespace
 * for equality on an opaque byte string, while keys sit in a B+Tree and support
 * lower-bound seeks -- so a segment tree can only be indexed on the key side.
 * Namestrings therefore expand to a key prefix, and -ns picks which template a
 * command walks.
 *
 * Depth notation: with a namestring bound the intermediate segments are already
 * known, so a command names only its leaf and inherits the template. >N instead
 * truncates the template to N segments, which is the one case where a depth
 * count carries information the namestring does not. Spelling out >>> next to a
 * bound namestring would duplicate what the template already says, and a
 * miscount there fails silently -- returning another level's rows rather than
 * an error.
 */
"""

from __future__ import annotations

import shlex
from typing import Any

SEP = "/"


class CommandError(ValueError):
    """A command failed to parse, or named something that does not exist."""


class Namestring:
    """
     * A named key-prefix template, verified once when it is defined.
     *
     * Verification is deliberately not repeated per use: confirming a path
     * costs a prefix scan, and re-running it on every command would put that
     * cost on the hot path for no new information. A template that goes stale
     * because someone removed the folder degrades to an empty result, never to
     * wrong rows -- only the prefix string is cached, never any rows.
     */
    """

    __slots__ = ("name", "segments")

    def __init__(self, name: str, segments: list[str]):
        self.name = name
        self.segments = segments

    def prefix(self, depth: int | None = None) -> str:
        """Key prefix for the whole template, or truncated to `depth` segments."""
        if depth is None:
            depth = len(self.segments)
        if depth < 1:
            raise CommandError(f"depth must be >= 1, got {depth}")
        if depth > len(self.segments):
            raise CommandError(
                f"namestring {self.name!r} has {len(self.segments)} segments, "
                f"cannot truncate to {depth}"
            )
        return SEP.join(self.segments[:depth]) + SEP

    def __repr__(self) -> str:
        return f"Namestring({self.name!r}, {SEP.join(self.segments)!r})"


def split_segments(spec: str) -> list[str]:
    """Parse "a,b,c" or "a/b/c" into segments, rejecting anything ambiguous."""
    # Both spellings are accepted, but not mixed: "a,b/c" could mean three
    # segments or two, and picking one silently would file keys under a prefix
    # the caller did not write.
    if "," in spec and SEP in spec:
        raise CommandError(
            f"namestring spec {spec!r} mixes ',' and {SEP!r}; pick one separator"
        )
    raw = spec.replace(SEP, ",") if SEP in spec else spec
    segments = [s.strip() for s in raw.split(",")]
    for segment in segments:
        if not segment:
            raise CommandError(f"empty segment in namestring spec {spec!r}")
        if SEP in segment:
            raise CommandError(
                f"segment {segment!r} contains {SEP!r}; the separator is reserved"
            )
    return segments


def parse_depth(token: str) -> int | None:
    """
     * Read a depth token: ">2" or ">>" both mean two levels.
     *
     * The numeric form is the one to use past three levels; ">>>>>>" cannot be
     * counted reliably by eye, and an off-by-one there produces rows from the
     * wrong level instead of an error.
     */
    """
    if not token.startswith(">"):
        return None
    rest = token.lstrip(">")
    arrows = len(token) - len(rest)
    if rest == "":
        return arrows
    if arrows != 1 or not rest.isdigit():
        raise CommandError(f"malformed depth token {token!r}")
    depth = int(rest)
    if depth < 1:
        raise CommandError(f"depth must be >= 1, got {token!r}")
    return depth


class CommandSession:
    """
     * Command executor holding one session's namestring table.
     *
     * The table lives in memory only. Persisting it would turn a convenience
     * into an on-disk format commitment, and format v2 is frozen -- anything
     * written to the file has to survive backup, compact, and migrate.
     */
    """

    READ_VERBS = frozenset({"get", "ls", "count", "exists", "find"})
    WRITE_VERBS = frozenset({"put", "del"})

    def __init__(self, database, *, readonly: bool = False):
        self.db = database
        self.readonly = readonly
        self.namestrings: dict[str, Namestring] = {}

    def define(self, pairs: dict[str, str]) -> list[str]:
        """
         * Bind several namestrings at once, all-or-nothing.
         *
         * A partial bind would leave later commands resolving against a table
         * that is only half the caller's intent, so nothing is installed until
         * every spec parses.
         */
        """
        staged: dict[str, Namestring] = {}
        for name, spec in pairs.items():
            if name in self.namestrings or name in staged:
                raise CommandError(f"namestring {name!r} is already defined")
            staged[name] = Namestring(name, split_segments(spec))
        self.namestrings.update(staged)
        return list(staged)

    def resolve(self, name: str) -> Namestring:
        try:
            return self.namestrings[name]
        except KeyError:
            known = ", ".join(sorted(self.namestrings)) or "(none defined)"
            raise CommandError(
                f"unknown namestring {name!r}; defined: {known}"
            ) from None

    @staticmethod
    def _extract_ns(command: str) -> tuple[str | None, str]:
        """
         * Peel a leading `-ns <name>` off the command line.
         *
         * The namestring is a separate argument rather than the first path
         * segment because the engine treats the two differently: a namespace is
         * compared for equality, a key prefix is seeked in the B+Tree. Folding
         * them into one path would make the syntax imply a tree level that the
         * storage layer does not have.
         */
        """
        text = command.strip()
        if not text.startswith("-ns"):
            return None, text
        rest = text[3:].lstrip()
        if not rest:
            raise CommandError("-ns needs a namestring name")
        parts = rest.split(None, 1)
        name = parts[0].strip('"\'')
        remainder = parts[1] if len(parts) > 1 else ""
        return name, remainder.strip().strip('"\'')

    @staticmethod
    def _bind(tokens: list[str], params: tuple[Any, ...]) -> list[str]:
        """
         * Substitute positional `?` placeholders after tokenisation.
         *
         * Binding happens post-split on purpose: a value spliced into the text
         * before tokenising could carry a quote or a separator and re-shape the
         * line into a different operation, which is the same failure mode as SQL
         * injection.
         */
        """
        wanted = sum(1 for token in tokens if token == "?")
        if wanted != len(params):
            raise CommandError(
                f"command has {wanted} placeholder(s) but {len(params)} "
                f"parameter(s) were passed"
            )
        out: list[str] = []
        supplied = iter(params)
        for token in tokens:
            out.append(str(next(supplied)) if token == "?" else token)
        return out

    def run(self, command: str, *params: Any) -> Any:
        """
         * Execute one command line: `[-ns NAME] VERB NAMESPACE [LEAF] [>N]`.
         *
         * Values reach the command through *params bound to `?`, never spliced
         * into the text.
         */
        """
        ns_name, rest = self._extract_ns(command)
        try:
            tokens = shlex.split(rest)
        except ValueError as exc:
            raise CommandError(f"could not tokenise {rest!r}: {exc}") from None
        if not tokens:
            raise CommandError("empty command")

        tokens = self._bind(tokens, params)
        verb, args = tokens[0].lower(), tokens[1:]

        if verb in self.WRITE_VERBS and self.readonly:
            raise CommandError(f"{verb!r} writes; this session is read-only")
        if verb not in self.READ_VERBS | self.WRITE_VERBS:
            known = ", ".join(sorted(self.READ_VERBS | self.WRITE_VERBS))
            raise CommandError(f"unknown verb {verb!r}; known verbs: {known}")

        depth = None
        if args and args[-1].startswith(">"):
            depth = parse_depth(args.pop())

        template = self.resolve(ns_name) if ns_name else None
        if depth is not None and template is None:
            raise CommandError("a depth token needs -ns to truncate against")

        return getattr(self, f"_verb_{verb}")(args, template, depth)

    def _prefix(self, template: Namestring | None, depth: int | None) -> str:
        return "" if template is None else template.prefix(depth)

    def _deepest_live_depth(self, namespace: str, template: Namestring) -> int:
        """
         * Walk the template back to the deepest level that still holds data.
         *
         * This is what separates "you named a level that does not exist" from
         * "the level is real but empty". Without it both look identical -- a
         * prefix scan returns [] either way -- and shibadb has no schema to
         * catch the typo for you.
         */
        """
        for level in range(len(template.segments), 0, -1):
            if self.db.scan(namespace, template.prefix(level).encode()):
                return level
        return 0

    def _verb_ls(
        self, args: list[str], template: Namestring | None, depth: int | None
    ) -> list[tuple[bytes, bytes]]:
        if not args:
            raise CommandError("ls needs a namespace: 'ls <namespace> [leaf]'")
        namespace = args[0]
        prefix = self._prefix(template, depth)
        if len(args) > 1:
            prefix += args[1].rstrip(SEP) + SEP
        rows = self.db.scan(namespace, prefix.encode())
        if rows or template is None:
            return rows
        # Empty under a namestring: report which level actually broke instead of
        # handing back an indistinguishable [].
        live = self._deepest_live_depth(namespace, template)
        asked = depth or len(template.segments)
        if live < asked:
            bad = template.segments[live] if live < len(template.segments) else "?"
            raise CommandError(
                f"path breaks at level {live + 1} ({bad!r}): "
                f"{template.prefix(live + 1)!r} holds nothing in {namespace!r}"
            )
        return rows

    def _verb_get(
        self, args: list[str], template: Namestring | None, depth: int | None
    ) -> bytes:
        if len(args) != 2:
            raise CommandError("get needs 'get <namespace> <leaf>'")
        namespace, leaf = args
        return self.db.get(namespace, (self._prefix(template, depth) + leaf).encode())

    def _verb_put(
        self, args: list[str], template: Namestring | None, depth: int | None
    ) -> None:
        if len(args) != 3:
            raise CommandError("put needs 'put <namespace> <leaf> <value>'")
        namespace, leaf, value = args
        self.db.put(namespace, (self._prefix(template, depth) + leaf).encode(), value)

    def _verb_del(
        self, args: list[str], template: Namestring | None, depth: int | None
    ) -> None:
        if len(args) != 2:
            raise CommandError("del needs 'del <namespace> <leaf>'")
        namespace, leaf = args
        self.db.delete(namespace, (self._prefix(template, depth) + leaf).encode())

    def _verb_exists(
        self, args: list[str], template: Namestring | None, depth: int | None
    ) -> bool:
        if len(args) != 2:
            raise CommandError("exists needs 'exists <namespace> <leaf>'")
        namespace, leaf = args
        key = (self._prefix(template, depth) + leaf).encode()
        try:
            self.db.get(namespace, key)
        except Exception as exc:  # NotFoundError is the expected miss here
            if type(exc).__name__ == "NotFoundError":
                return False
            raise
        return True

    def _verb_count(
        self, args: list[str], template: Namestring | None, depth: int | None
    ) -> int:
        if not args:
            raise CommandError("count needs a namespace")
        prefix = self._prefix(template, depth)
        if len(args) > 1:
            prefix += args[1].rstrip(SEP) + SEP
        return len(self.db.scan(args[0], prefix.encode()))

    def _verb_find(
        self, args: list[str], template: Namestring | None, depth: int | None
    ) -> list[bytes]:
        """
         * Document lookup through a secondary index.
         *
         * Templates do not apply: find addresses a document collection by index
         * value, which is a different addressing scheme from the KV key prefix a
         * namestring expands to.
         */
        """
        del template, depth
        if len(args) != 3:
            raise CommandError("find needs 'find <collection> <index> <value>'")
        collection, index_name, value = args
        return self.db.find(collection, index_name, value)
