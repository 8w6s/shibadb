from __future__ import annotations

import tempfile
from pathlib import Path

import shibadb


def test_ttl() -> None:
    with tempfile.TemporaryDirectory() as directory:
        path = Path(directory) / "ttl.sdb"
        with shibadb.Database.create(path) as db:
            # live (far-future ttl), expired (negative ttl -> already past),
            # and no-ttl documents
            db.put_json("cache", "live", {"v": 1}, ttl=3600)
            db.put_json("cache", "dead", {"v": 2}, ttl=-1)
            db.put_json("cache", "forever", {"v": 3})

            # get_json: live and forever readable, dead treated as missing
            live = db.get_json("cache", "live")
            assert live["v"] == 1 and "_expires_at" in live
            assert db.get_json("cache", "forever") == {"v": 3}
            try:
                db.get_json("cache", "dead")
            except shibadb.NotFoundError:
                pass
            else:
                raise AssertionError("expired document must read as NotFound")
            # but readable with include_expired
            assert db.get_json("cache", "dead", include_expired=True)["v"] == 2

            # find_query skips the expired document
            found = db.find_query("cache", {})
            ids = sorted(d["v"] for _k, d in found)
            assert ids == [1, 3], ids
            # include_expired brings it back
            all_docs = db.find_query("cache", {}, include_expired=True)
            assert sorted(d["v"] for _k, d in all_docs) == [1, 2, 3]

            # purge_expired reclaims only the dead one
            removed = db.purge_expired("cache")
            assert removed == 1
            assert db.purge_expired("cache") == 0  # idempotent
            remaining = sorted(d["v"] for _k, d in db.find_query("cache", {}, include_expired=True))
            assert remaining == [1, 3]

    print("ttl: ok")


if __name__ == "__main__":
    test_ttl()
