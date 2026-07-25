#!/usr/bin/env python3
"""Create deterministic ShibaDB source archives and an SPDX file manifest."""

from __future__ import annotations

import argparse
import gzip
import hashlib
import json
import os
from pathlib import Path
import stat
import tarfile
import tempfile
import time
import zipfile

VERSION = "1.0.0"
PREFIX = f"shibadb-{VERSION}"
ROOT_FILES = {
    ".gitignore",
    "CMakeLists.txt",
    "CMakePresets.json",
    "README.md",
    "RELEASE.md",
    "ROADMAP.md",
    "SECURITY.md",
}
SOURCE_DIRECTORIES = {
    ".github",
    "abi",
    "cmake",
    "docs",
    "fuzz",
    "include",
    "python",
    "scripts",
    "src",
    "tests",
}
SECURITY_REVIEW_ROOT = f"{PREFIX}-security-review"
SECURITY_REVIEW_DOCS = {
    "docs/BTREE_FORMAT.md",
    "docs/CONCURRENCY.md",
    "docs/ENCRYPTION.md",
    "docs/ENGINE_API.md",
    "docs/OPERATIONS.md",
    "docs/PUBLIC_TRANSACTIONS.md",
    "docs/SECURITY_REVIEW.md",
    "docs/WAL_FORMAT.md",
}
SECURITY_REVIEW_TESTS = {
    "tests/test_btree_batch.c",
    "tests/test_btree_property.c",
    "tests/test_crypto.c",
    "tests/test_encryption.c",
    "tests/test_encryption_fault.c",
    "tests/test_engine_fault.c",
    "tests/test_engine_operations.c",
    "tests/test_pager_fault.c",
    "tests/test_public_transaction.c",
    "tests/test_public_transaction_crash.c",
    "tests/test_public_transaction_fault.c",
    "tests/test_public_transaction_property.c",
    "tests/test_transaction.c",
    "tests/test_transaction_crash.c",
}


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def source_files(root: Path) -> list[Path]:
    files = []
    for path in root.rglob("*"):
        relative = path.relative_to(root)
        if not path.is_file():
            continue
        if relative.as_posix() in ROOT_FILES:
            files.append(relative)
        elif relative.parts and relative.parts[0] in SOURCE_DIRECTORIES:
            if "__pycache__" not in relative.parts and not path.name.endswith(
                (".pyc", ".pyo")
            ):
                files.append(relative)
    return sorted(files, key=lambda item: item.as_posix())


def tar_bytes(root: Path, files: list[Path], epoch: int) -> bytes:
    with tempfile.TemporaryFile() as raw:
        with tarfile.open(fileobj=raw, mode="w", format=tarfile.PAX_FORMAT) as tar:
            for relative in files:
                data = (root / relative).read_bytes()
                info = tarfile.TarInfo(f"{PREFIX}/{relative.as_posix()}")
                info.size = len(data)
                info.mtime = epoch
                info.uid = 0
                info.gid = 0
                info.uname = "root"
                info.gname = "root"
                executable = relative.suffix == ".py"
                info.mode = 0o755 if executable else 0o644
                tar.addfile(info, __import__("io").BytesIO(data))
        raw.seek(0)
        buffer = __import__("io").BytesIO()
        with gzip.GzipFile(
            filename="", mode="wb", fileobj=buffer, mtime=epoch
        ) as compressed:
            compressed.write(raw.read())
        return buffer.getvalue()


def zip_bytes(root: Path, files: list[Path], epoch: int) -> bytes:
    minimum = 315532800
    timestamp = time.gmtime(max(epoch, minimum))[:6]
    buffer = __import__("io").BytesIO()
    with zipfile.ZipFile(
        buffer, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9
    ) as archive:
        for relative in files:
            info = zipfile.ZipInfo(
                f"{PREFIX}/{relative.as_posix()}", timestamp
            )
            info.create_system = 3
            executable = relative.suffix == ".py"
            mode = 0o755 if executable else 0o644
            info.external_attr = (
                (stat.S_IFREG | mode) << 16
            )
            info.compress_type = zipfile.ZIP_DEFLATED
            archive.writestr(info, (root / relative).read_bytes())
    return buffer.getvalue()


def spdx(root: Path, files: list[Path], epoch: int) -> bytes:
    entries = []
    for index, relative in enumerate(files, start=1):
        data = (root / relative).read_bytes()
        entries.append(
            {
                "SPDXID": f"SPDXRef-File-{index}",
                "fileName": f"./{relative.as_posix()}",
                "checksums": [
                    {"algorithm": "SHA256", "checksumValue": sha256(data)}
                ],
                "licenseConcluded": "NOASSERTION",
                "copyrightText": "NOASSERTION",
            }
        )
    document = {
        "spdxVersion": "SPDX-2.3",
        "dataLicense": "CC0-1.0",
        "SPDXID": "SPDXRef-DOCUMENT",
        "name": f"ShibaDB-{VERSION}-source",
        "documentNamespace":
            f"https://shibadb.invalid/spdx/{VERSION}/{epoch}",
        "creationInfo": {
            "created": time.strftime(
                "%Y-%m-%dT%H:%M:%SZ", time.gmtime(epoch)
            ),
            "creators": ["Tool: ShibaDB-package_release.py"],
        },
        "packages": [
            {
                "SPDXID": "SPDXRef-Package",
                "name": "ShibaDB",
                "versionInfo": VERSION,
                "downloadLocation": "NOASSERTION",
                "filesAnalyzed": True,
                "licenseConcluded": "NOASSERTION",
                "licenseDeclared": "NOASSERTION",
                "copyrightText": "NOASSERTION",
            }
        ],
        "files": entries,
        "relationships": [
            {
                "spdxElementId": "SPDXRef-Package",
                "relationshipType": "CONTAINS",
                "relatedSpdxElement": entry["SPDXID"],
            }
            for entry in entries
        ],
    }
    return (
        json.dumps(document, sort_keys=True, separators=(",", ":"))
        + "\n"
    ).encode()


def security_review_files(files: list[Path]) -> list[Path]:
    selected = []
    for relative in files:
        name = relative.as_posix()
        if (
            relative.parts[0] in {"src", "include", "fuzz"}
            or name in SECURITY_REVIEW_DOCS
            or name in SECURITY_REVIEW_TESTS
            or name in {
                "CMakeLists.txt",
                "RELEASE.md",
                "ROADMAP.md",
                "SECURITY.md",
                "scripts/release_audit.py",
                "scripts/release_evidence.py",
            }
        ):
            selected.append(relative)
    return selected


def security_review_bundle(
    root: Path, files: list[Path], epoch: int, commit: str
) -> bytes:
    selected = security_review_files(files)
    manifest = {
        "schema": "https://shibadb.dev/schemas/security-review-bundle-v1",
        "status": "unaudited",
        "commit": commit,
        "source_epoch": epoch,
        "algorithms": [
            "SHA-256",
            "HMAC-SHA256",
            "PBKDF2-HMAC-SHA256",
            "HChaCha20",
            "XChaCha20-Poly1305",
        ],
        "review_areas": {
            "cryptography": {
                "entry_points": [
                    "sdb_pbkdf2_hmac_sha256",
                    "sdb_xchacha20poly1305_encrypt",
                    "sdb_xchacha20poly1305_decrypt",
                    "sdb_key_wrap",
                    "sdb_key_unwrap",
                    "sdb_encrypted_page_encode",
                    "sdb_encrypted_page_decrypt",
                ]
            },
            "crash-consistency": {
                "entry_points": [
                    "sdb_txn_commit",
                    "sdb_wal_write",
                    "sdb_wal_recover",
                    "sdb_btree_batch_commit",
                    "sdb_transaction_commit",
                    "sdb_database_backup",
                    "sdb_database_compact",
                ]
            },
        },
        "files": [
            {
                "path": relative.as_posix(),
                "sha256": sha256((root / relative).read_bytes()),
                "size": len((root / relative).read_bytes()),
            }
            for relative in selected
        ],
    }
    manifest_bytes = (
        json.dumps(manifest, indent=2, sort_keys=True) + "\n"
    ).encode()
    timestamp = time.gmtime(max(epoch, 315532800))[:6]
    buffer = __import__("io").BytesIO()
    with zipfile.ZipFile(
        buffer, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9
    ) as archive:
        for relative in selected:
            info = zipfile.ZipInfo(
                f"{SECURITY_REVIEW_ROOT}/{relative.as_posix()}", timestamp
            )
            info.create_system = 3
            info.external_attr = (stat.S_IFREG | 0o644) << 16
            info.compress_type = zipfile.ZIP_DEFLATED
            archive.writestr(info, (root / relative).read_bytes())
        info = zipfile.ZipInfo(
            f"{SECURITY_REVIEW_ROOT}/MANIFEST.json", timestamp
        )
        info.create_system = 3
        info.external_attr = (stat.S_IFREG | 0o644) << 16
        info.compress_type = zipfile.ZIP_DEFLATED
        archive.writestr(info, manifest_bytes)
    return buffer.getvalue()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("output", type=Path)
    parser.add_argument(
        "--epoch",
        type=int,
        default=int(__import__("os").environ.get("SOURCE_DATE_EPOCH", "0")),
    )
    parser.add_argument(
        "--commit",
        default=os.environ.get("GITHUB_SHA", "0" * 40),
    )
    arguments = parser.parse_args()
    if (
        len(arguments.commit) != 40
        or any(character not in "0123456789abcdef"
               for character in arguments.commit)
    ):
        parser.error("--commit must be a lowercase 40-character Git SHA")
    root = Path(__file__).resolve().parents[1]
    output = arguments.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    files = source_files(root)
    artifacts = {
        f"{PREFIX}.tar.gz": tar_bytes(root, files, arguments.epoch),
        f"{PREFIX}.zip": zip_bytes(root, files, arguments.epoch),
        f"{PREFIX}.spdx.json": spdx(root, files, arguments.epoch),
        f"{PREFIX}-security-review.zip": security_review_bundle(
            root, files, arguments.epoch, arguments.commit
        ),
    }
    checksums = []
    for name, data in sorted(artifacts.items()):
        (output / name).write_bytes(data)
        checksums.append(f"{sha256(data)}  {name}")
    (output / "SHA256SUMS").write_text(
        "\n".join(checksums) + "\n", encoding="ascii"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
