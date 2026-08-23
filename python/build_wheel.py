#!/usr/bin/env python3

from __future__ import annotations

import base64

import csv

import hashlib

import io

from pathlib import Path

import sys

import zipfile

NAME = "shibadb"

VERSION = "0.1.0"

def digest(data: bytes) -> str:

    encoded = base64.urlsafe_b64encode(hashlib.sha256(data).digest())

    return "sha256=" + encoded.rstrip(b"=").decode("ascii")

def main() -> int:

    root = Path(__file__).resolve().parents[1]

    output = Path(sys.argv[1] if len(sys.argv) > 1 else root / "dist")

    output.mkdir(parents=True, exist_ok=True)

    wheel = output / f"{NAME}-{VERSION}-py3-none-any.whl"

    dist_info = f"{NAME}-{VERSION}.dist-info"

    package = root / "python" / NAME

    # Every .py in the package, not just __init__.py. __init__ imports .query
    # and .polyglot lazily inside find_query()/query(), so a wheel carrying
    # only __init__.py imports fine and then raises ImportError the first time
    # a caller touches either method — the break does not show up until run
    # time. Glob so a new module cannot be forgotten here again.
    files = {
        f"{NAME}/{source.name}": source.read_bytes()
        for source in sorted(package.glob("*.py"))
    }

    files.update({

        f"{dist_info}/METADATA": (

            "Metadata-Version: 2.1\n"

            "Name: shibadb\n"

            f"Version: {VERSION}\n"

            "Summary: Python binding for the ShibaDB C ABI\n"

            "Requires-Python: >=3.10\n"

        ).encode(),

        f"{dist_info}/WHEEL": (

            "Wheel-Version: 1.0\n"

            "Generator: shibadb-build-wheel\n"

            "Root-Is-Purelib: true\n"

            "Tag: py3-none-any\n"

        ).encode(),

        f"{dist_info}/top_level.txt": b"shibadb\n",

    })

    rows = [

        (path, digest(data), str(len(data))) for path, data in files.items()

    ]

    record_path = f"{dist_info}/RECORD"

    stream = io.StringIO(newline="")

    writer = csv.writer(stream, lineterminator="\n")

    writer.writerows(rows)

    writer.writerow((record_path, "", ""))

    files[record_path] = stream.getvalue().encode()

    with zipfile.ZipFile(wheel, "w", zipfile.ZIP_DEFLATED) as archive:

        for path in sorted(files):

            archive.writestr(path, files[path])

    print(wheel)

    return 0

if __name__ == "__main__":

    raise SystemExit(main())
