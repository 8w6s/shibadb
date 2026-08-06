#!/usr/bin/env python3

from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
from typing import Any
import xml.etree.ElementTree as ET

SCHEMA = "https://shibadb.dev/schemas/release-evidence-v2"
SECURITY_SCHEMA = "https://shibadb.dev/schemas/security-audit-evidence-v1"
PLATFORMS = {"linux", "macos", "windows"}
SUITES = {"complete", "encrypted-soak"}
SHA = re.compile(r"^[0-9a-f]{40}$")
POSITIVE_INTEGER = re.compile(r"^[1-9][0-9]*$")
SHA256 = re.compile(r"^[0-9a-f]{64}$")

def _nonempty_text(value: Any) -> bool:
    return isinstance(value, str) and bool(value.strip())

def validate_ci_evidence(
    data: Any,
    *,
    expected_platform: str,
    expected_suite: str,
    expected_commit: str | None = None,
) -> tuple[bool, str]:
    if not isinstance(data, dict):
        return False, "evidence must be a JSON object"
    if data.get("schema") != SCHEMA:
        return False, "unsupported evidence schema"
    if data.get("status") != "passed":
        return False, "evidence status is not passed"
    if data.get("platform") != expected_platform:
        return False, "platform does not match the required gate"
    if data.get("suite") != expected_suite:
        return False, "suite does not match the required gate"
    if data.get("platform") not in PLATFORMS or data.get("suite") not in SUITES:
        return False, "unknown platform or suite"
    for field in (
        "repository",
        "ref",
        "workflow",
        "compiler",
        "architecture",
        "run_url",
    ):
        if not _nonempty_text(data.get(field)):
            return False, f"{field} is missing"
    repository = data["repository"]
    if repository.count("/") != 1 or any(
        not part for part in repository.split("/")
    ):
        return False, "repository must be an owner/name pair"
    commit = data.get("commit")
    if not isinstance(commit, str) or SHA.fullmatch(commit) is None:
        return False, "commit must be a lowercase 40-character Git SHA"
    if expected_commit is not None and commit != expected_commit:
        return False, "commit does not match the release candidate"
    if not data["ref"].startswith("refs/"):
        return False, "ref is not a fully qualified Git ref"
    for field in ("run_id", "run_attempt"):
        value = data.get(field)
        if not isinstance(value, str) or POSITIVE_INTEGER.fullmatch(value) is None:
            return False, f"{field} must be a positive integer string"
    test_count = data.get("test_count")
    if not isinstance(test_count, int) or isinstance(test_count, bool) \
            or test_count < 1:
        return False, "test_count must be a positive integer"
    test_report_sha256 = data.get("test_report_sha256")
    if not isinstance(test_report_sha256, str) \
            or SHA256.fullmatch(test_report_sha256) is None:
        return False, "test_report_sha256 is invalid"
    expected_url = (
        f"https://github.com/{repository}/actions/runs/{data['run_id']}"
    )
    if data["run_url"] != expected_url:
        return False, "run_url does not match repository and run_id"
    generated_at = data.get("generated_at")
    if not isinstance(generated_at, str):
        return False, "generated_at is missing"
    try:
        parsed_time = datetime.fromisoformat(generated_at.replace("Z", "+00:00"))
    except ValueError:
        return False, "generated_at is not ISO-8601"
    if parsed_time.tzinfo is None:
        return False, "generated_at must include a timezone"
    operations = data.get("operations")
    if expected_suite == "encrypted-soak":
        if not isinstance(operations, int) or operations < 10000:
            return False, "encrypted soak requires at least 10000 operations"
    elif operations is not None:
        return False, "complete-suite evidence must not contain operations"
    return True, "passed"

def load_ci_evidence(
    path: str | None,
    *,
    expected_platform: str,
    expected_suite: str,
    expected_commit: str | None = None,
) -> tuple[bool, str]:
    if not path:
        return False, "evidence path is not configured"
    evidence_path = Path(path)
    try:
        data = json.loads(evidence_path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as error:
        return False, f"cannot read evidence: {error}"
    valid, reason = validate_ci_evidence(
        data,
        expected_platform=expected_platform,
        expected_suite=expected_suite,
        expected_commit=expected_commit,
    )
    if not valid:
        return valid, reason
    report_path = evidence_path.with_name("ctest.xml")
    try:
        report_bytes = report_path.read_bytes()
        report_count = _junit_test_count(report_path)
    except (OSError, SystemExit) as error:
        return False, f"cannot verify CTest report: {error}"
    if hashlib.sha256(report_bytes).hexdigest() != data["test_report_sha256"]:
        return False, "CTest report digest does not match evidence"
    if report_count != data["test_count"]:
        return False, "CTest report count does not match evidence"
    return True, "passed"

def validate_security_audit_evidence(
    data: Any, *, expected_commit: str | None = None
) -> tuple[bool, str]:
    if not isinstance(data, dict):
        return False, "evidence must be a JSON object"
    if data.get("schema") != SECURITY_SCHEMA:
        return False, "unsupported security evidence schema"
    if data.get("status") != "passed":
        return False, "security audit status is not passed"
    commit = data.get("commit")
    if not isinstance(commit, str) or SHA.fullmatch(commit) is None:
        return False, "commit must be a lowercase 40-character Git SHA"
    if expected_commit is not None and commit != expected_commit:
        return False, "security audit commit does not match the release"
    auditor = data.get("auditor")
    if not isinstance(auditor, dict):
        return False, "auditor identity is missing"
    for field in ("name", "organization", "contact"):
        if not _nonempty_text(auditor.get(field)):
            return False, f"auditor {field} is missing"
    if auditor.get("independent") is not True:
        return False, "auditor has not declared independence"
    scope = data.get("scope")
    if not isinstance(scope, list) or not {
        "cryptography",
        "crash-consistency",
    }.issubset(set(item for item in scope if isinstance(item, str))):
        return False, "audit scope must cover cryptography and crash consistency"
    report_sha256 = data.get("report_sha256")
    if (
        not isinstance(report_sha256, str)
        or re.fullmatch(r"[0-9a-f]{64}", report_sha256) is None
    ):
        return False, "report_sha256 is invalid"
    bundle_sha256 = data.get("bundle_sha256")
    if (
        not isinstance(bundle_sha256, str)
        or SHA256.fullmatch(bundle_sha256) is None
    ):
        return False, "bundle_sha256 is invalid"
    report_url = data.get("report_url")
    if not isinstance(report_url, str) or not report_url.startswith("https://"):
        return False, "report_url must use HTTPS"
    completed_at = data.get("completed_at")
    if not isinstance(completed_at, str):
        return False, "completed_at is missing"
    try:
        parsed_time = datetime.fromisoformat(completed_at.replace("Z", "+00:00"))
    except ValueError:
        return False, "completed_at is not ISO-8601"
    if parsed_time.tzinfo is None:
        return False, "completed_at must include a timezone"
    return True, "passed"

def load_security_audit_evidence(
    path: str | None, *, expected_commit: str | None = None
) -> tuple[bool, str]:
    if not path:
        return False, "security evidence path is not configured"
    try:
        data = json.loads(Path(path).read_text(encoding="utf-8"))
    except (OSError, ValueError) as error:
        return False, f"cannot read security evidence: {error}"
    return validate_security_audit_evidence(
        data, expected_commit=expected_commit
    )

def _required_environment(name: str) -> str:
    value = os.getenv(name)
    if not value:
        raise SystemExit(f"{name} is required")
    return value

def _junit_test_count(path: Path) -> int:
    try:
        root = ET.parse(path).getroot()
    except (OSError, ET.ParseError) as error:
        raise SystemExit(f"cannot parse CTest JUnit report: {error}") from error
    if root.tag == "testsuite":
        candidates = [root]
    elif root.tag == "testsuites":
        candidates = list(root.findall("testsuite"))
    else:
        raise SystemExit("CTest JUnit report has an unsupported root element")
    try:
        count = sum(int(item.attrib["tests"]) for item in candidates)
    except (KeyError, ValueError) as error:
        raise SystemExit("CTest JUnit report has an invalid test count") from error
    if count < 1:
        raise SystemExit("CTest JUnit report contains no tests")
    return count

def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--platform", required=True, choices=sorted(PLATFORMS))
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--architecture", required=True)
    parser.add_argument("--suite", required=True, choices=sorted(SUITES))
    parser.add_argument("--operations", type=int)
    parser.add_argument("--test-report", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    arguments = parser.parse_args()
    if arguments.suite == "encrypted-soak" and (
        arguments.operations is None or arguments.operations < 10000
    ):
        parser.error("encrypted-soak evidence requires --operations >= 10000")
    if arguments.suite == "complete" and arguments.operations is not None:
        parser.error("--operations is only valid for encrypted-soak")
    repository = _required_environment("GITHUB_REPOSITORY")
    run_id = _required_environment("GITHUB_RUN_ID")
    try:
        report_bytes = arguments.test_report.read_bytes()
    except OSError as error:
        raise SystemExit(f"cannot read CTest JUnit report: {error}") from error
    evidence: dict[str, object] = {
        "schema": SCHEMA,
        "status": "passed",
        "repository": repository,
        "commit": _required_environment("GITHUB_SHA"),
        "ref": _required_environment("GITHUB_REF"),
        "workflow": _required_environment("GITHUB_WORKFLOW"),
        "run_id": run_id,
        "run_attempt": _required_environment("GITHUB_RUN_ATTEMPT"),
        "run_url": f"https://github.com/{repository}/actions/runs/{run_id}",
        "platform": arguments.platform,
        "compiler": arguments.compiler,
        "architecture": arguments.architecture,
        "suite": arguments.suite,
        "test_count": _junit_test_count(arguments.test_report),
        "test_report_sha256": hashlib.sha256(report_bytes).hexdigest(),
        "generated_at": datetime.now(timezone.utc)
        .isoformat()
        .replace("+00:00", "Z"),
    }
    if arguments.operations is not None:
        evidence["operations"] = arguments.operations
    valid, reason = validate_ci_evidence(
        evidence,
        expected_platform=arguments.platform,
        expected_suite=arguments.suite,
    )
    if not valid:
        raise SystemExit(f"refusing to write invalid evidence: {reason}")
    arguments.output.parent.mkdir(parents=True, exist_ok=True)
    arguments.output.write_text(
        json.dumps(evidence, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
