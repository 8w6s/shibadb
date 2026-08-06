# Overnight autonomous work — document-subsystem reclaim fix

Started while you slept. Goal: close the remaining churn leaks in the DOCUMENT
subsystem, safely.

## Confirmed gaps (from workload-diversity + code reading)
1. **Document overwrite leaks old chunks.** `document_put_in_mutation` calls
   `object_plan`/`object_write_chunks` directly (NOT `object_put`), so the
   object_put overwrite chunk-reclaim fix does NOT apply to documents.
2. **Document delete leaks index rows.** `document_delete` → `object_delete`
   cleans metadata+chunks but not the per-term `index_entry`(0x51) /
   `unique_guard`(0x52) rows.
3. **Document overwrite leaks index rows** (same reason).

Already fixed + committed: kv/blob + document metadata/chunks on delete;
kv/blob overwrite chunk reclaim. Space leak only (find() stays correct);
`compact()` reclaims all of it.

## Plan (reverse-row approach — the only design that is both correct and O(1))
- New row `SDB_INDEX_REVERSE_PREFIX` (0x53): key = pair_key(0x53, DOCUMENT,
  collection, doc_id); value = [generation(8)][term_count(2)][per term:
  name_size(2), name, value_size(2), value]. No generation in the KEY, so it is
  updated in place on overwrite (never orphaned) and removed on delete.
- put (overwrite): read old reverse -> delete old index_entry(old gen) + old
  chunks(old gen) + old guard(if owned) -> write new index rows + new reverse.
- delete: read reverse -> delete index rows + reverse -> object_delete
  (metadata+chunks).
- Integrations (correctness-critical): `row_generation` (0x53 -> gen from
  value), `entry_is_live` (0x53 -> live iff doc metadata present and gens
  match), so `verify` counts it right and `compact` copies it.

## Safety discipline (conservative mandate)
- Strong TDD gate: tests/test_index_churn_reclaim.c — find() exact + unique
  enforced + stale bounded across delete/overwrite churn; extended with compact
  + reopen.
- Verify: gate green + targeted ctest + ASan + reopen + compact.
- Adversarial workflow review of the change in FRESH contexts (my context is
  saturated) — liveness, guard-ownership, generation, verify/compact
  integration, find/unique.
- **COMMIT ONLY IF EVERYTHING IS CLEAN.** On any uncertainty or failure, revert
  to the last committed state (the chunk fix) and leave a report — do NOT ship
  an unverified core-index/format change.

This file is deleted on success (folded into the commit message / AUDIT) or
kept with a status note if the work is left incomplete.
