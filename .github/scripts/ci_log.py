#!/usr/bin/env python3

import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path


def log_path(stage):
    log_dir = Path(os.environ.get("RUNNER_TEMP", tempfile.gettempdir()))
    return log_dir / f"shibadb-ci-{stage}.log"


def run_logged(stage, command):
    path = log_path(stage)
    path.parent.mkdir(parents=True, exist_ok=True)
    try:
        process = subprocess.Popen(
            command,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            encoding="utf-8",
            errors="replace",
            bufsize=1,
        )
    except OSError as error:
        print(f"Could not start command: {error}")
        return 127

    with path.open("w", encoding="utf-8", errors="replace") as log:
        if process.stdout is not None:
            for line in process.stdout:
                sys.stdout.write(line)
                sys.stdout.flush()
                log.write(line)
                log.flush()
    return process.wait()


def report_failure(stage):
    path = log_path(stage)
    if not path.is_file():
        print(f"::error title=CI {stage} diagnostics::Log file is missing")
        return 0

    lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    pattern = re.compile(
        r"FAILED:|fatal error:|error:|undefined reference|"
        r"The following tests FAILED|\*\*\*Failed|"
        r"AddressSanitizer|UndefinedBehaviorSanitizer|ThreadSanitizer|"
        r"Segmentation fault|Assertion .* failed|Error C[0-9]+",
        re.IGNORECASE,
    )
    matching = [
        index
        for index, line in enumerate(lines)
        if pattern.search(line)
    ]
    selected = set()
    for index in matching[-20:]:
        selected.update(range(max(0, index - 2), min(len(lines), index + 3)))
    if not selected:
        selected.update(range(max(0, len(lines) - 30), len(lines)))

    excerpts = [lines[index].strip()[:500] for index in sorted(selected)]
    message = "\n".join(excerpts)[-12000:]
    message = message.replace("%", "%25").replace("\r", "%0D")
    message = message.replace("\n", "%0A")
    print(f"::error title=CI {stage} diagnostics::{message}")
    return 0


def main(arguments):
    if len(arguments) < 2:
        print("Usage: ci_log.py run|report stage [-- command ...]")
        return 2

    action, stage = arguments[:2]
    if not re.fullmatch(r"[a-z][a-z0-9_-]*", stage):
        print("Invalid log stage")
        return 2
    if action == "report" and len(arguments) == 2:
        return report_failure(stage)
    if action == "run" and len(arguments) > 3 and arguments[2] == "--":
        return run_logged(stage, arguments[3:])
    print("Usage: ci_log.py run|report stage [-- command ...]")
    return 2


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
