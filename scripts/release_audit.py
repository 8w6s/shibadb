#!/usr/bin/env python3

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import tokenize

from release_evidence import load_ci_evidence, load_security_audit_evidence
from strip_c_comments import strip as strip_c_comments
from strip_py_comments import strip_comments as strip_py_comments

FORBIDDEN_DEPENDENCIES = re.compile(
    rb"sqlite|libcrypto|openssl|libsodium", re.IGNORECASE
)

def dependency_scan_bytes(path: Path) -> bytes:
    suffix = path.suffix.lower()
    if suffix in (".c", ".h"):
        try:
            return strip_c_comments(path.read_text(encoding="utf-8")).encode(
                "utf-8"
            )
        except (OSError, UnicodeDecodeError):
            return path.read_bytes()
    if suffix == ".py":
        try:
            source = path.read_text(encoding="utf-8")
            return strip_py_comments(source).encode("utf-8")
        except (
            OSError,
            UnicodeDecodeError,
            SyntaxError,
            ValueError,
            tokenize.TokenError,
        ):
            return path.read_bytes()
    return path.read_bytes()

def read_symbol_allowlist(path: Path) -> list[str]:
    symbols = []
    for line in path.read_text(encoding="ascii").splitlines():
        name = line.strip()
        if not name or name.startswith("#"):
            continue
        symbols.append(name.removeprefix("_"))
    return sorted(set(symbols))

def exported_public_symbols(nm_output: str) -> list[str]:
    symbols = []
    for line in nm_output.splitlines():
        fields = line.split()
        if not fields:
            continue
        name = fields[-1].removeprefix("_")
        if name.startswith("sdb_"):
            symbols.append(name)
    return sorted(set(symbols))

def ci_evidence(
    path: str | None,
    platform: str,
    suite: str,
    expected_commit: str | None,
) -> tuple[bool, str]:
    return load_ci_evidence(
        path,
        expected_platform=platform,
        expected_suite=suite,
        expected_commit=expected_commit,
    )

def attestation(
    path: str | None, gh: str, workflow: str
) -> tuple[bool, str]:
    if not path:
        return False, "evidence path is not configured"
    try:
        data = json.loads(Path(path).read_text(encoding="utf-8"))
        repository = data["repository"]
    except (OSError, ValueError, KeyError, TypeError) as error:
        return False, f"cannot identify evidence repository: {error}"
    signer = f"{repository}/.github/workflows/{workflow}"
    try:
        verified = subprocess.run(
            [
                gh,
                "attestation",
                "verify",
                path,
                "-R",
                repository,
                "--signer-workflow",
                signer,
            ],
            check=False,
            text=True,
            capture_output=True,
        )
    except OSError as error:
        return False, f"cannot execute gh: {error}"
    if verified.returncode != 0:
        detail = verified.stderr.strip() or verified.stdout.strip()
        return False, f"attestation verification failed: {detail}"
    return True, "passed"

def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--library", type=Path)
    parser.add_argument("--nm", default="nm")
    parser.add_argument("--readelf", default="readelf")
    parser.add_argument("--strict", action="store_true")
    parser.add_argument("--gh", default="gh")
    parser.add_argument("--json-output", type=Path)
    arguments = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    expected_commit = os.getenv("SDB_RELEASE_COMMIT")
    macos_native, macos_native_detail = ci_evidence(
        os.getenv("SDB_MACOS_EVIDENCE"),
        "macos",
        "complete",
        expected_commit,
    )
    windows_native, windows_native_detail = ci_evidence(
        os.getenv("SDB_WINDOWS_EVIDENCE"),
        "windows",
        "complete",
        expected_commit,
    )
    linux_soak, linux_soak_detail = ci_evidence(
        os.getenv("SDB_LINUX_SOAK_EVIDENCE"),
        "linux",
        "encrypted-soak",
        expected_commit,
    )
    macos_soak, macos_soak_detail = ci_evidence(
        os.getenv("SDB_MACOS_SOAK_EVIDENCE"),
        "macos",
        "encrypted-soak",
        expected_commit,
    )
    windows_soak, windows_soak_detail = ci_evidence(
        os.getenv("SDB_WINDOWS_SOAK_EVIDENCE"),
        "windows",
        "encrypted-soak",
        expected_commit,
    )
    security_audit, security_audit_detail = load_security_audit_evidence(
        os.getenv("SDB_SECURITY_AUDIT_EVIDENCE"),
        expected_commit=expected_commit,
    )
    attestation_results: dict[str, tuple[bool, str]] = {}
    if arguments.strict:
        for name, path, workflow in (
            (
                "macos_native",
                os.getenv("SDB_MACOS_EVIDENCE"),
                "release-candidate.yml",
            ),
            (
                "windows_native",
                os.getenv("SDB_WINDOWS_EVIDENCE"),
                "release-candidate.yml",
            ),
            (
                "linux_soak",
                os.getenv("SDB_LINUX_SOAK_EVIDENCE"),
                "soak.yml",
            ),
            (
                "macos_soak",
                os.getenv("SDB_MACOS_SOAK_EVIDENCE"),
                "soak.yml",
            ),
            (
                "windows_soak",
                os.getenv("SDB_WINDOWS_SOAK_EVIDENCE"),
                "soak.yml",
            ),
        ):
            attestation_results[name] = attestation(
                path, arguments.gh, workflow
            )
    else:
        attestation_results = {
            name: (False, "checked only by strict audit")
            for name in (
                "macos_native",
                "windows_native",
                "linux_soak",
                "macos_soak",
                "windows_soak",
            )
        }
    scanned = []
    forbidden_hits = []
    for directory in ("src", "include", "python"):
        for path in (root / directory).rglob("*"):
            if path.is_file() and "__pycache__" not in path.parts:
                relative = str(path.relative_to(root))
                scanned.append(relative)
                if FORBIDDEN_DEPENDENCIES.search(
                    dependency_scan_bytes(path)
                ):
                    forbidden_hits.append(relative)
    checks: dict[str, bool] = {
        "no_forbidden_database_or_crypto_dependency": not forbidden_hits,
        "distribution_license_selected": (root / "LICENSE").is_file(),
        "macos_native_evidence": macos_native,
        "windows_native_evidence": windows_native,
        "linux_soak_evidence": linux_soak,
        "macos_soak_evidence": macos_soak,
        "windows_soak_evidence": windows_soak,
        "macos_native_attestation": attestation_results["macos_native"][0],
        "windows_native_attestation": attestation_results[
            "windows_native"
        ][0],
        "linux_soak_attestation": attestation_results["linux_soak"][0],
        "macos_soak_attestation": attestation_results["macos_soak"][0],
        "windows_soak_attestation": attestation_results[
            "windows_soak"
        ][0],
        "independent_security_audit": security_audit,
    }
    details: dict[str, object] = {
        "scanned_files": len(scanned),
        "forbidden_hits": forbidden_hits,
        "external_evidence": {
            "macos_native": macos_native_detail,
            "windows_native": windows_native_detail,
            "linux_soak": linux_soak_detail,
            "macos_soak": macos_soak_detail,
            "windows_soak": windows_soak_detail,
            "security_audit": security_audit_detail,
            "attestations": {
                name: result[1]
                for name, result in attestation_results.items()
            },
        },
    }
    if arguments.library:
        nm = subprocess.run(
            [arguments.nm, "-D", "--defined-only", str(arguments.library)],
            check=True,
            text=True,
            capture_output=True,
        )
        symbols = exported_public_symbols(nm.stdout)
        expected = read_symbol_allowlist(root / "abi" / "symbols-v1.txt")
        checks["shared_symbol_allowlist"] = symbols == expected
        if symbols != expected:
            details["symbol_allowlist_diff"] = {
                "exported_not_allowlisted": sorted(
                    set(symbols) - set(expected)
                ),
                "allowlisted_not_exported": sorted(
                    set(expected) - set(symbols)
                ),
            }
        dynamic = subprocess.run(
            [arguments.readelf, "-d", str(arguments.library)],
            check=True,
            text=True,
            capture_output=True,
        )
        needed = re.findall(r"Shared library: \[([^\]]+)\]", dynamic.stdout)
        checks["linux_runtime_dependencies"] = all(
            name == "libc.so.6" for name in needed
        )
        details["needed_libraries"] = needed
    local_names = {
        "no_forbidden_database_or_crypto_dependency",
        "shared_symbol_allowlist",
        "linux_runtime_dependencies",
    }
    local_passed = all(
        value for name, value in checks.items() if name in local_names
    )
    strict_passed = local_passed and all(checks.values())
    report = {
        "status": "passed" if (
            strict_passed if arguments.strict else local_passed
        ) else "failed",
        "strict": arguments.strict,
        "checks": checks,
        "details": details,
    }
    rendered = json.dumps(report, indent=2, sort_keys=True) + "\n"
    if arguments.json_output:
        arguments.json_output.write_text(rendered, encoding="utf-8")
    print(rendered, end="")
    return 0 if report["status"] == "passed" else 1

if __name__ == "__main__":
    raise SystemExit(main())
