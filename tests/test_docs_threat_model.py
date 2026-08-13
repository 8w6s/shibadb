#!/usr/bin/env python3

from __future__ import annotations

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
text = (ROOT / "docs" / "ENCRYPTION.md").read_text(encoding="utf-8")
lower = text.lower()

# The encryption doc must explicitly distinguish the two modes' guarantees.
assert "threat model" in lower, "missing threat-model section"

# Unencrypted mode: CRC is accidental-damage detection only, not authenticity,
# and is forgeable by an offline tamperer; no confidentiality.
assert "damage detection, not authenticity" in text
assert "no confidentiality" in lower
assert "recompute a matching crc" in lower

# Encrypted mode: authenticity comes from the Poly1305 tag, not the CRC.
assert "the cryptographic guarantee comes from the tag, not the crc" in lower

print("docs threat-model tests: ok")
