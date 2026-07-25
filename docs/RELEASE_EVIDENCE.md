# Release evidence

ShibaDB treats workflow configuration and release evidence as different
things. A configured macOS job does not prove that a particular commit passed
on macOS.

## Native and soak evidence

`scripts/release_evidence.py` writes schema-v2 JSON only after a workflow's
tests succeed. Evidence binds:

- repository, full Git ref, and 40-character commit SHA;
- workflow name, run ID, attempt, and canonical run URL;
- platform, native runner architecture, and compiler;
- complete-suite or encrypted-soak identity;
- positive CTest count and SHA-256 of the attested JUnit report;
- operation count for a soak, with a minimum accepted value of 10,000.

The release-candidate workflow produces `evidence/native.json` and
`evidence/ctest.xml` inside every native artifact. The soak workflow produces
the same evidence pair per platform. Both the JSON and the exact JUnit report
whose digest it contains are cryptographically attested with GitHub Artifact
Attestations.

Strict audit validates both the JSON schema and the attestation. It constrains
the signer to this repository's `release-candidate.yml` or `soak.yml`
workflow, so a JSON file created elsewhere is not accepted.

Download evidence for the exact release commit, then run:

```sh
export SDB_RELEASE_COMMIT=0123456789abcdef0123456789abcdef01234567
export SDB_MACOS_EVIDENCE=/path/to/macos/evidence/native.json
export SDB_WINDOWS_EVIDENCE=/path/to/windows/evidence/native.json
export SDB_LINUX_SOAK_EVIDENCE=/path/to/linux/soak.json
export SDB_MACOS_SOAK_EVIDENCE=/path/to/macos/soak.json
export SDB_WINDOWS_SOAK_EVIDENCE=/path/to/windows/soak.json
export SDB_SECURITY_AUDIT_EVIDENCE=/path/to/security-audit.json

python scripts/release_audit.py \
  --strict \
  --library build/libshibadb.so.0.1.0 \
  --json-output build/release-readiness.json
```

The GitHub CLI must be installed and authenticated so `gh attestation verify`
can retrieve and verify Sigstore provenance. GitHub documents verification at
<https://docs.github.com/en/actions/how-tos/secure-your-work/use-artifact-attestations/use-artifact-attestations>.

## Independent security evidence

Independent audit evidence uses
`https://shibadb.dev/schemas/security-audit-evidence-v1` and must identify the
exact commit, reviewer, organization, contact, independence declaration,
report URL, report SHA-256, deterministic security-review bundle SHA-256,
completion time, and both required scopes:
`cryptography` and `crash-consistency`.

Example shape:

```json
{
  "schema": "https://shibadb.dev/schemas/security-audit-evidence-v1",
  "status": "passed",
  "commit": "0123456789abcdef0123456789abcdef01234567",
  "auditor": {
    "name": "Reviewer Name",
    "organization": "Independent Organization",
    "contact": "reviewer@example.com",
    "independent": true
  },
  "scope": ["cryptography", "crash-consistency"],
  "report_sha256": "64 lowercase hexadecimal characters",
  "bundle_sha256": "SHA-256 from the release SHA256SUMS file",
  "report_url": "https://example.com/shibadb-audit.pdf",
  "completed_at": "2026-07-24T12:00:00Z"
}
```

This schema does not turn a self-review into an independent review. The
identity, report, hash, and independence claim remain externally auditable
release records.
