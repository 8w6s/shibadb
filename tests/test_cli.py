from __future__ import annotations

import contextlib
import io
import tempfile
from pathlib import Path

from shibadb import cli


def run(argv: list[str]) -> tuple[int, str]:
    out = io.StringIO()
    with contextlib.redirect_stdout(out):
        code = cli.main(argv)
    return code, out.getvalue()


def main() -> None:
    with tempfile.TemporaryDirectory() as directory:
        db = str(Path(directory) / "cli.sdb")

        assert run([db, "create"])[0] == 0
        assert run([db, "put", "users", "u1", '{"name":"alice","age":30}'])[0] == 0
        assert run([db, "put", "users", "u2", '{"name":"bob","age":25}'])[0] == 0

        code, out = run([db, "get", "users", "u1"])
        assert code == 0 and '"alice"' in out

        # missing key -> exit 1
        assert run([db, "get", "users", "nope"])[0] == 1

        # scan prints key=value lines in order
        code, out = run([db, "scan", "users"])
        assert code == 0
        assert out.splitlines()[0].startswith("u1=")
        assert len(out.splitlines()) == 2

        # prefix scan
        code, out = run([db, "scan", "users", "--prefix", "u1"])
        assert code == 0 and len(out.splitlines()) == 1

        # find with a Mongo-style query
        code, out = run([db, "find", "users", '{"age":{"$gte":30}}'])
        assert code == 0
        assert out.count("\n") == 1 and "alice" in out

        # invalid query JSON -> exit 2
        assert run([db, "find", "users", "{bad"])[0] == 2

        # query runs SQL over the JSON values
        code, out = run([db, "query", "SELECT * FROM users WHERE age >= 30"])
        assert code == 0 and "alice" in out
        # a Mongo count returns a scalar
        code, out = run([db, "query", "db.users.count()"])
        assert code == 0 and out.strip() == "2"
        # invalid statement exits 2
        assert run([db, "query", "NOTASTATEMENT"])[0] == 2

        # namespaces lists populated (kind, namespace)
        code, out = run([db, "namespaces"])
        assert code == 0
        assert "kv\tusers" in out

        # stats prints verify counters
        code, out = run([db, "stats"])
        assert code == 0 and "=" in out

        # delete then get misses
        assert run([db, "del", "users", "u1"])[0] == 0
        assert run([db, "get", "users", "u1"])[0] == 1

    print("cli: ok")


if __name__ == "__main__":
    main()
