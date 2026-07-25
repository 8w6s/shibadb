#!/usr/bin/env python3
"""Regression tests for release-evidence validation."""

from __future__ import annotations

import copy
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))

from release_evidence import (
    SCHEMA,
    SECURITY_SCHEMA,
    validate_ci_evidence,
    validate_security_audit_evidence,
    load_ci_evidence,
)
from release_audit import attestation


def sample() -> dict[str, object]:
    return {
        "schema": SCHEMA,
        "status": "passed",
        "repository": "example/shibadb",
        "commit": "a" * 40,
        "ref": "refs/tags/v1.0.0",
        "workflow": "Release candidate",
        "run_id": "12345",
        "run_attempt": "1",
        "run_url": "https://github.com/example/shibadb/actions/runs/12345",
        "platform": "macos",
        "compiler": "AppleClang",
        "architecture": "ARM64",
        "suite": "complete",
        "test_count": 35,
        "test_report_sha256": "c" * 64,
        "generated_at": "2026-07-24T12:00:00Z",
    }


def valid(data: object, platform: str = "macos", suite: str = "complete") -> bool:
    return validate_ci_evidence(
        data,
        expected_platform=platform,
        expected_suite=suite,
        expected_commit="a" * 40,
    )[0]


assert valid(sample())

for field in (
    "schema",
    "status",
    "repository",
    "commit",
    "ref",
    "workflow",
    "run_id",
    "run_attempt",
    "run_url",
    "platform",
    "compiler",
    "architecture",
    "suite",
    "test_count",
    "test_report_sha256",
    "generated_at",
):
    broken = copy.deepcopy(sample())
    broken.pop(field)
    assert not valid(broken), field

broken = sample()
broken["commit"] = "b" * 40
assert not valid(broken)

broken = sample()
broken["platform"] = "windows"
assert not valid(broken)

broken = sample()
broken["operations"] = 20000
assert not valid(broken)

for invalid_count in (0, -1, True, "35"):
    broken = sample()
    broken["test_count"] = invalid_count
    assert not valid(broken)

broken = sample()
broken["test_report_sha256"] = "not-a-digest"
assert not valid(broken)

soak = sample()
soak["platform"] = "windows"
soak["suite"] = "encrypted-soak"
soak["operations"] = 20000
assert valid(soak, "windows", "encrypted-soak")

soak["operations"] = 9999
assert not valid(soak, "windows", "encrypted-soak")

security = {
    "schema": SECURITY_SCHEMA,
    "status": "passed",
    "commit": "a" * 40,
    "auditor": {
        "name": "Security Reviewer",
        "organization": "Independent Labs",
        "contact": "reviewer@example.test",
        "independent": True,
    },
    "scope": ["cryptography", "crash-consistency"],
    "report_sha256": "b" * 64,
    "bundle_sha256": "d" * 64,
    "report_url": "https://example.test/shibadb-audit.pdf",
    "completed_at": "2026-07-24T12:00:00Z",
}
assert validate_security_audit_evidence(
    security, expected_commit="a" * 40
)[0]

for field in (
    "schema",
    "status",
    "commit",
    "auditor",
    "scope",
    "report_sha256",
    "bundle_sha256",
    "report_url",
    "completed_at",
):
    broken = copy.deepcopy(security)
    broken.pop(field)
    assert not validate_security_audit_evidence(
        broken, expected_commit="a" * 40
    )[0], field

broken = copy.deepcopy(security)
broken["auditor"]["independent"] = False
assert not validate_security_audit_evidence(broken)[0]

broken = copy.deepcopy(security)
broken["scope"] = ["cryptography"]
assert not validate_security_audit_evidence(broken)[0]

with tempfile.TemporaryDirectory() as directory:
    evidence_path = Path(directory) / "native.json"
    evidence_path.write_text(json.dumps(sample()), encoding="utf-8")
    completed = subprocess.CompletedProcess(
        args=[], returncode=0, stdout="verified", stderr=""
    )
    with patch("release_audit.subprocess.run", return_value=completed) as run:
        ok, reason = attestation(
            str(evidence_path), "gh", "release-candidate.yml"
        )
    assert ok and reason == "passed"
    command = run.call_args.args[0]
    assert command[:3] == ["gh", "attestation", "verify"]
    assert command[-2:] == [
        "--signer-workflow",
        "example/shibadb/.github/workflows/release-candidate.yml",
    ]

    failed = subprocess.CompletedProcess(
        args=[], returncode=1, stdout="", stderr="not verified"
    )
    with patch("release_audit.subprocess.run", return_value=failed):
        ok, reason = attestation(str(evidence_path), "gh", "soak.yml")
    assert not ok and "not verified" in reason

    report_path = Path(directory) / "ctest.xml"
    generated_path = Path(directory) / "generated.json"
    report_path.write_text(
        '<testsuites><testsuite name="all" tests="35"/></testsuites>',
        encoding="utf-8",
    )
    environment = os.environ.copy()
    environment.update(
        {
            "GITHUB_REPOSITORY": "example/shibadb",
            "GITHUB_SHA": "a" * 40,
            "GITHUB_REF": "refs/tags/v1.0.0",
            "GITHUB_WORKFLOW": "Release candidate",
            "GITHUB_RUN_ID": "12345",
            "GITHUB_RUN_ATTEMPT": "1",
        }
    )
    subprocess.run(
        [
            sys.executable,
            str(Path(__file__).resolve().parents[1]
                / "scripts" / "release_evidence.py"),
            "--platform",
            "macos",
            "--compiler",
            "AppleClang",
            "--architecture",
            "ARM64",
            "--suite",
            "complete",
            "--test-report",
            str(report_path),
            "--output",
            str(generated_path),
        ],
        check=True,
        env=environment,
    )
    generated = json.loads(generated_path.read_text(encoding="utf-8"))
    assert generated["test_count"] == 35
    assert valid(generated)
    ok, reason = load_ci_evidence(
        str(generated_path),
        expected_platform="macos",
        expected_suite="complete",
        expected_commit="a" * 40,
    )
    assert ok and reason == "passed"
    report_path.write_text(
        '<testsuites><testsuite name="all" tests="34"/></testsuites>',
        encoding="utf-8",
    )
    assert not load_ci_evidence(
        str(generated_path),
        expected_platform="macos",
        expected_suite="complete",
        expected_commit="a" * 40,
    )[0]

print("release evidence tests: ok")
