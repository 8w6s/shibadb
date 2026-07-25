#!/usr/bin/env python3
"""Build source packages twice and require byte-identical output."""

from pathlib import Path
import subprocess
import sys
import tempfile


def main() -> int:
    root = Path(__file__).resolve().parents[1]
    packager = root / "scripts" / "package_release.py"
    with tempfile.TemporaryDirectory() as directory:
        first = Path(directory) / "first"
        second = Path(directory) / "second"
        command = [sys.executable, str(packager)]
        subprocess.run(command + [str(first), "--epoch", "1700000000"], check=True)
        subprocess.run(command + [str(second), "--epoch", "1700000000"], check=True)
        first_files = sorted(path.name for path in first.iterdir())
        second_files = sorted(path.name for path in second.iterdir())
        if first_files != second_files:
            raise SystemExit("artifact list differs")
        for name in first_files:
            if (first / name).read_bytes() != (second / name).read_bytes():
                raise SystemExit(f"artifact is not reproducible: {name}")
    print("release reproducibility: ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
