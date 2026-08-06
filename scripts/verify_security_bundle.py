#!/usr/bin/env python3

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import zipfile

SCHEMA = "https://shibadb.dev/schemas/security-review-bundle-v1"
SHA = re.compile(r"^[0-9a-f]{40}$")
SHA256 = re.compile(r"^[0-9a-f]{64}$")
MAX_UNCOMPRESSED_SIZE = 100 * 1024 * 1024

def fail(message: str) -> None:
    raise SystemExit(f"invalid security-review bundle: {message}")

def verify(path: Path) -> dict[str, object]:
    try:
        archive = zipfile.ZipFile(path, "r")
    except (OSError, zipfile.BadZipFile) as error:
        fail(str(error))
    with archive:
        members = archive.infolist()
        names = [member.filename for member in members]
        if len(names) != len(set(names)):
            fail("duplicate ZIP member")
        if not names:
            fail("empty archive")
        if sum(member.file_size for member in members) > MAX_UNCOMPRESSED_SIZE:
            fail("uncompressed size exceeds review limit")
        roots = set()
        for name in names:
            pure = PurePosixPath(name)
            if pure.is_absolute() or ".." in pure.parts or len(pure.parts) < 2:
                fail("unsafe member path")
            roots.add(pure.parts[0])
        if len(roots) != 1:
            fail("archive must contain exactly one root directory")
        root = next(iter(roots))
        manifest_name = f"{root}/MANIFEST.json"
        if manifest_name not in names:
            fail("MANIFEST.json is missing")
        try:
            manifest = json.loads(archive.read(manifest_name))
        except (KeyError, UnicodeDecodeError, ValueError) as error:
            fail(f"cannot parse manifest: {error}")
        if not isinstance(manifest, dict) or manifest.get("schema") != SCHEMA:
            fail("unsupported manifest schema")
        if manifest.get("status") != "unaudited":
            fail("bundle must not claim an audit result")
        commit = manifest.get("commit")
        if not isinstance(commit, str) or SHA.fullmatch(commit) is None:
            fail("invalid commit")
        if not isinstance(manifest.get("source_epoch"), int):
            fail("invalid source epoch")
        review_areas = manifest.get("review_areas")
        if not isinstance(review_areas, dict) or not {
            "cryptography",
            "crash-consistency",
        }.issubset(review_areas):
            fail("required review areas are missing")
        algorithms = manifest.get("algorithms")
        if not isinstance(algorithms, list) or not {
            "SHA-256",
            "HMAC-SHA256",
            "PBKDF2-HMAC-SHA256",
            "HChaCha20",
            "XChaCha20-Poly1305",
        }.issubset(
            item for item in algorithms if isinstance(item, str)
        ):
            fail("algorithm inventory is incomplete")
        for area_name in ("cryptography", "crash-consistency"):
            area = review_areas[area_name]
            entry_points = area.get("entry_points") \
                if isinstance(area, dict) else None
            if (
                not isinstance(entry_points, list)
                or not entry_points
                or len(entry_points) != len(set(entry_points))
                or any(
                    not isinstance(item, str) or not item
                    for item in entry_points
                )
            ):
                fail(f"invalid entry-point inventory: {area_name}")
        files = manifest.get("files")
        if not isinstance(files, list) or not files:
            fail("file manifest is empty")
        expected_members = {manifest_name}
        previous_path = ""
        for item in files:
            if not isinstance(item, dict):
                fail("invalid file entry")
            relative = item.get("path")
            digest = item.get("sha256")
            size = item.get("size")
            if not isinstance(relative, str):
                fail("file path is missing")
            pure = PurePosixPath(relative)
            if pure.is_absolute() or ".." in pure.parts or not pure.parts:
                fail("unsafe manifest path")
            if relative <= previous_path:
                fail("manifest paths are not strictly sorted")
            previous_path = relative
            if not isinstance(digest, str) or SHA256.fullmatch(digest) is None:
                fail("invalid file digest")
            if not isinstance(size, int) or isinstance(size, bool) or size < 0:
                fail("invalid file size")
            member_name = f"{root}/{relative}"
            expected_members.add(member_name)
            try:
                content = archive.read(member_name)
            except KeyError:
                fail(f"missing scoped file: {relative}")
            if len(content) != size:
                fail(f"size mismatch: {relative}")
            if hashlib.sha256(content).hexdigest() != digest:
                fail(f"digest mismatch: {relative}")
        if set(names) != expected_members:
            fail("archive and manifest file sets differ")
        return manifest

def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("bundle", type=Path)
    arguments = parser.parse_args()
    manifest = verify(arguments.bundle)
    print(
        "security-review bundle: ok "
        f"({len(manifest['files'])} files, commit {manifest['commit']})"
    )
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
