#!/usr/bin/env python3
"""Regression tests for the deterministic independent-review bundle."""

from __future__ import annotations

import json
from pathlib import Path
import sys
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))

from package_release import main as package_main
from verify_security_bundle import verify


with tempfile.TemporaryDirectory() as directory:
    output = Path(directory) / "artifacts"
    original_argv = sys.argv
    sys.argv = [
        "package_release.py",
        str(output),
        "--epoch",
        "1700000000",
        "--commit",
        "a" * 40,
    ]
    try:
        assert package_main() == 0
    finally:
        sys.argv = original_argv

    bundle = output / "shibadb-0.1.0-security-review.zip"
    manifest = verify(bundle)
    assert manifest["commit"] == "a" * 40
    assert {"cryptography", "crash-consistency"}.issubset(
        manifest["review_areas"]
    )
    paths = {item["path"] for item in manifest["files"]}
    assert "src/xchacha20poly1305.c" in paths
    assert "src/wal.c" in paths
    assert "src/pager.c" in paths
    assert "tests/test_public_transaction_crash.c" in paths
    assert "docs/SECURITY_REVIEW.md" in paths

    tampered = Path(directory) / "tampered.zip"
    with zipfile.ZipFile(bundle, "r") as source, zipfile.ZipFile(
        tampered, "w", zipfile.ZIP_DEFLATED
    ) as destination:
        for member in source.infolist():
            content = source.read(member.filename)
            if member.filename.endswith("/src/wal.c"):
                content += b"\n/* tampered */\n"
            destination.writestr(member, content)
    try:
        verify(tampered)
    except SystemExit as error:
        assert "mismatch" in str(error)
    else:
        raise AssertionError("tampered bundle passed verification")

    sums = (output / "SHA256SUMS").read_text(encoding="ascii")
    assert "shibadb-0.1.0-security-review.zip" in sums
    spdx = json.loads(
        (output / "shibadb-0.1.0.spdx.json").read_text(encoding="utf-8")
    )
    assert spdx["spdxVersion"] == "SPDX-2.3"

print("security-review bundle tests: ok")
