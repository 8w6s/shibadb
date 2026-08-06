# shibadb-c hardening — design spec

**Date**: 2026-07-28
**Status**: Design, pending user review
**Target**: shibadb-c (C11 embedded ACID KV DB) — đường tới depend-on-able 1.0

---

## 1. Vấn đề

Qua nhiều update, shibadb đang có dấu hiệu trượt vào cùng "vết xe đổ" của hvpdb: bump version cao (rc6, tiến 1.0), thêm doc report theo mốc thời gian (AUDIT_2026_07, COVERAGE_2026_07, SECURITY_REVIEW_2026_07_25), tồn tại `hvpdb-compatibility.md` trong docs — nhưng **chưa đủ vững để depend on cho dự án lớn**.

User diagnosis: chưa ổn định ở **cả 4 trụ correctness**:
- Durability & crash recovery (WAL, torn write, power loss)
- Concurrency & isolation (multi-writer, MVCC, deadlock)
- On-disk format stability (upgrade path, backward-compat)
- Performance predictability (regression, tail latency)

## 2. Positioning

**Embedded ACID key-value DB, encrypted-by-default, single-file, crash-safe, C11.**

Không đua SQLite ở general-purpose scale. Là **SQLite alternative khi encryption default matters**. Value prop = "an toàn mặc định + đủ vững để depend on".

Đích đến: SQLite-tier expectation cho:
1. Personal projects của user (mikochi/nexora/xtalk/…)
2. Public library open-source, aim 1.0 để external dev depend on
3. Foundation cho một sản phẩm cụ thể

## 3. Non-goals (hàng rào chính vs hvpdb-drift)

- Không thêm modality (giữ đúng 3: `kv`, `blob`, `document`)
- Không thêm dialect/language shim (không SQL, không Mongo, không Redis)
- Không thêm auth/RBAC/HTTP/replication/sync ở core (nếu cần → repo tách)
- Không re-add Python binding phase này (sub-project sau 1.0)
- Không multi-reader parallelism trong 1.0 (Track A single-mutex; Track B hoãn qua 1.1)
- Không operator library kiểu "380 op experimental"
- Không column-family, không graph, không JSON operators
- Không thêm verb thứ 4 cho một concept (đã có `put/get/delete`)

## 4. Bản Python legacy

`~/project/shibadb/shibadb/` (Python 2.0.0a3, SQLite + AES-GCM, 30k LOC, 548 tests) — **xóa toàn bộ**. Không maintain, không migrate binding, không giữ làm reference. Learnings đã được internalize vào design C.

## 5. Phase 0 — Sạch tuyệt đối (destructive, sequential, trước tất cả)

Mục tiêu: codebase sạch, git repo mới sole-authored, code không comment. "Trông như người viết", không giống AI-generated.

### 5.1 Chốt WIP hiện tại
- 3 file modified trong shibadb-c (`CMakeLists.txt`, `include/shibadb_engine.h`, `src/engine.c`): decide commit-or-revert với user trước khi wipe.
- 2 untracked (`docs/PROPOSAL_MULTI_READER.md`, `tests/test_enumerate.c`): decide keep-or-drop.

### 5.2 Xóa
| Path | Reason |
|---|---|
| `~/project/shibadb/shibadb/` | Python legacy, deprecated |
| `~/project/shibadb/refs/rust-v0/` | Historical reference không cần |
| `~/project/shibadb/refs/python-v0-docs/` | Historical reference không cần |
| `~/project/shibadb/shibadb-c/docs/hvpdb-compatibility.md` | Mầm scope creep |
| `~/project/shibadb/shibadb-c/docs/README_legacy.md` | Legacy orphan |
| `~/project/shibadb/shibadb-c/.git/` | Reset ownership, fresh init |
| `~/project/shibadb/shibadb-c/build*/` (11 dir) | Preset đã có, không cần build dir vương vãi |

### 5.3 Gộp docs duplicate
- `AUDIT.md` + `AUDIT_2026_07.md` → 1 file `AUDIT.md` live (update-in-place)
- `COVERAGE.md` + `COVERAGE_2026_07.md` → 1 file `COVERAGE.md` live
- `SECURITY_REVIEW.md` + `SECURITY_REVIEW_2026_07_25.md` → 1 file `SECURITY_REVIEW.md` live

### 5.4 Xóa TẤT CẢ comment trong code
- Files: `*.c`, `*.h`, `*.py` trong toàn bộ codebase.
- Xóa: `//` line comments, `/* ... */` block comments, docstrings Python.
- **Xóa kể cả license header** trong từng file.
- Giữ nguyên `docs/*.md` (không phải "code").
- Giữ `LICENSE` file ở root (MIT compliance qua file riêng, không cần in-file header).

### 5.5 Move `shibadb-c/` content lên root
- Sau khi xóa `.git`, `shibadb-c/` không còn ý nghĩa subdir.
- Move tất cả nội dung `shibadb-c/*` lên `~/project/shibadb/` root.
- Xóa thư mục `shibadb-c/` rỗng.

### 5.6 Init git repo mới
- `git init` tại `~/project/shibadb/`.
- Config author = user (satoharuki), no other contributors.
- `.gitignore` chuẩn C:
  ```
  build*/
  *.o
  *.a
  *.so
  *.gcda
  *.gcno
  *.profraw
  *.profdata
  .cache/
  compile_commands.json
  ```
- Commit đầu tiên: "shibadb 1.0.0 baseline" — bao gồm cả spec file này.

## 6. Phase 1 — 4 trụ hardening (parallel, 4 team)

### 6.1 Durability & crash recovery

**Gap hiện tại**: có WAL + CRC 3 lớp + fault-injection framework gated `SDB_TESTING`, nhưng chưa có power-loss test rig thực sự (SIGKILL/disk-fill/EIO systematic).

**Deliverable**:
- `tests/crash/` — 15+ test files:
  - SIGKILL giữa WAL append (before commit trailer, after body write, midway header)
  - SIGKILL giữa pager flush (page dirty, WAL sealed)
  - `fsync` error injection (`SDB_TESTING` extend)
  - ENOSPC trên WAL append
  - EIO trên WAL read (recovery time)
  - Partial commit trailer (torn write oracle)
  - Partial page write (torn page)
  - Disk fill mid-transaction
  - Concurrent crash (kill giữa multi-thread active ops)
  - Property-based test: random ops → random kill → replay → verify content vs oracle
- `docs/CRASH_MODEL.md` — spec hình thức về guarantees (ACI-D breakdown per crash class)
- CI job `crash-suite` — foreground trên Linux; optional macOS via `soak.yml`

**Verification**: 24h soak clean trên `crash-suite`.

### 6.2 Concurrency & isolation

**Gap hiện tại**: single-mutex Track A hoạt động, `test_engine_concurrency.c` 362 LOC, nhưng không multi-process test, không TSAN thường xuyên trong CI, deadlock property chưa formal.

**Deliverable**:
- Confirm contract 1.0: single-writer/serialized-reader (Track A). Update `docs/CONCURRENCY.md`.
- `test_multi_process_open.c` — 2 process cùng cố mở, verify reject/serialize semantics.
- Extend `test_engine_concurrency.c` — stress N thread × random ops × M giây, verify no deadlock, no data race under TSAN.
- Property test deadlock hunt.
- CI job `tsan` regular (mỗi PR, không chỉ tag).

**Verification**: TSAN 24h soak clean.

### 6.3 On-disk format stability

**Gap hiện tại**: `SDB_FORMAT_VERSION_V1 = 1` duy nhất, `sdb_database_migrate` là stub reject-non-V1. Chưa có golden files, chưa có backward-compat test.

**Deliverable**:
- Freeze V1: contract 1.0. Change format = V2 với migration path đầy đủ.
- `tests/goldens/v1/` — 5-10 DB file mẫu (encrypted + plaintext modes) từ commit baseline.
- `test_backward_compat.c` — mở tất cả golden, verify content match reference.
- `sdb_database_migrate` — extend với test rằng reject cleanly V2/V3/invalid.
- `docs/FORMAT_STABILITY.md` — invariant list, promise, bump policy.
- CI job `backward-compat` — mỗi PR mở tất cả golden.

**Verification**: golden compat CI green qua 4 tuần liên tiếp.

### 6.4 Performance predictability

**Gap lớn nhất — hiện tại ZERO**: không `benchmarks/`, không CI perf gate, không baseline lưu trong repo.

**Deliverable**:
- `benchmarks/` directory:
  - `bench_kv.c` — put/get/delete P50/P95/P99 latency, throughput với 10k/100k/1M records
  - `bench_blob.c` — streaming large values
  - `bench_document.c` — structured access
  - `bench_batch_vs_autocommit.c` — comparison
  - `bench_encrypted_vs_plaintext.c` — overhead của encryption
  - `bench_concurrent_readers.c` — Track A serialized-reader throughput
- `benchmarks/baseline.json` — median current main sau Phase 0 commit
- `benchmarks/reference/` — SQLite raw + LMDB comparison numbers (không đua, chỉ để biết position)
- `.github/workflows/perf.yml`:
  - Fail nếu P99 tụt >5% hoặc throughput tụt >3% so baseline
  - Post kết quả lên PR comment
- `scripts/perf_diff.py` — compare local vs baseline

**Verification**: baseline stable qua 4 tuần liên tiếp, không regression.

### 6.5 CI infrastructure (thiếu, phải setup mới)

**Gap hiện tại**: `ci.yml` chỉ có Release trên Linux (GCC + Clang). CMake preset đã có 9 (`base`, `gcc`, `clang`, `release`, `sanitize`, `tsan`, `soak`, `fuzz`, `coverage`) nhưng chỉ preset `release` vào CI thường xuyên. Sanitizer/fuzz/coverage chỉ chạy manual hoặc on tag.

**Deliverable** (workflow files trong `.github/workflows/`):
- `ci.yml` (extend): thêm job `asan` (preset `sanitize`), `tsan` (preset `tsan`), `coverage` (preset `coverage` + upload codecov)
- `fuzz.yml` (mới): nightly fuzz với preset `fuzz`, corpus persistent
- `crash.yml` (mới): job `crash-suite` — chạy `tests/crash/` với các fault-injection matrix
- `perf.yml` (mới): perf regression gate, section 6.4 đã spec chi tiết
- `backward-compat.yml` (mới): mở tất cả golden files, section 6.3 đã spec

**Success**: 7 CI job green mỗi PR: `linux-gcc`, `linux-clang`, `asan`, `tsan`, `coverage`, `crash-suite`, `perf`, `backward-compat` (thực tế 8 job, target ≥7). Nightly: `fuzz`.

**Verification**: 7 job green liên tiếp 4 tuần.

## 7. Phase 2 — Dogfood target

Chọn 1 use-case nội bộ để migrate real. Ứng viên:

| App | Fit | Note |
|---|---|---|
| **xtalk** state persistence | Cao — có JSON messages phù hợp `document` API, cross-session state | Node/TS, cần binding CFFI |
| **mikochi** metadata | Trung — Discord bot state, đơn giản hơn | Node/TS, cần binding CFFI |
| **nexora-paid** session store | Thấp — đã có infra khác | Bun/Astro |

**Recommend**: xtalk (matches "encrypted + document" strengths của shibadb).

**Plan**:
1. Chọn app target sau khi Phase 0 xong (user quyết định)
2. Viết minimal binding cho stack app đó (Node.js FFI hoặc equivalent)
3. Migrate schema + existing data (script one-shot)
4. Chạy prod ≥ 30 ngày, log mọi error/warning liên quan shibadb
5. Bug fix trong shibadb-c dựa trên real usage
6. Không thêm feature không cần cho app đó

## 8. Phase 3 — Release gate 1.0

1.0 chỉ tag khi **tất cả** pass:

- [ ] Crash suite: 100% pass, 24h soak clean
- [ ] TSAN 24h soak clean
- [ ] Golden compat CI green qua 4 tuần liên tiếp
- [ ] Perf baseline stable qua 4 tuần liên tiếp, không regression
- [ ] Dogfood app ≥ 30 ngày prod không data corruption
- [ ] External audit từ ≥1 crypto reviewer + ≥1 storage engineer (AI reviewer OK nếu không budget)
- [ ] README bỏ warning "Do not use for production data yet"

## 9. API surface (không cắt, chỉ fix 2 gap consistency)

Confirm 44 hàm SDB_API hiện tại là canonical:
- `sdb_database_*` (9): create/open/close/backup/compact/migrate/verify + options × 2
- `sdb_transaction_*` (14): begin/commit/rollback/close + kv/blob/document × put/get/delete + index_create
- `sdb_{kv,blob,document}_*` auto-commit (9): put/get/delete × 3
- `sdb_index_*` (2): create/visit
- `sdb_list_*` (3): collections/indexes/namespaces
- `sdb_superblock_store_*` (3): create/read/update
- Misc (5): abi_version/version_number/version_string/status/status_string

**Fix 2 inconsistency**:
- Thêm `sdb_index_create` auto-commit variant (hiện chỉ transaction có)
- Thêm `sdb_index_delete` (drop index — hiện chưa có)

Sau Phase 1 xong, API surface **freeze**. Bất kỳ change nào = major version.

## 10. Anti-patterns cấm rõ (chống hvpdb-drift, enforce qua lint/CI)

| Cấm | Enforce |
|---|---|
| File `*_YYYY_MM_DD.md` trong docs | CI script grep, fail nếu tìm thấy |
| Build dir mới ngoài `build/` root và presets | `.gitignore` + CI check |
| Verb thứ 4 cho một concept (VD: `sdb_kv_set` / `sdb_kv_store`) | CI script whitelist verb per prefix |
| Subpackage / sub-CMakeLists ngoài `include/`, `src/`, `tests/`, `benchmarks/`, `docs/`, `scripts/` | CI structural check |
| Dialect / query language ở core | Code review rule |
| Release-note kiểu `*_COMPLETE.md`, `*_SUMMARY.txt` | CI script fail nếu tồn tại |
| Comment trong `*.c/*.h` | Pre-commit hook: `grep -nE '^\s*(//\|/\*)' *.c *.h` fail (chỉ match line-comment/block-comment C, tránh false-match `#include`/`#define`/`#pragma`) |
| Comment trong `*.py` | Pre-commit hook: `grep -nE '^\s*#(?![!])' *.py` fail (bỏ shebang `#!`) |
| Docstring trong `*.py` | AST scan tìm `Expr(Constant(str))` ở top của module/class/def |
| License header trong file source | Pre-commit hook script kiểm dòng 1-10 không chứa "Copyright" / "License" / "SPDX" |

## 11. Metrics dashboard

Sau mỗi phase, đo và log vào `docs/METRICS.md`:

| Metric | Target | Hvpdb (red line) |
|---|---|---|
| LOC test / LOC production | ≥30% | 3% |
| Duplicate doc file count | 0 | 16 file .md release cùng nói v1.0.8 |
| CI job count | ≥7 (crash, tsan, asan, fuzz, coverage, perf, backward-compat) | — |
| Untracked WIP >2 tuần | 0 | — |
| API verb count per concept | ≤3 (put/get/delete) | insert=append=save=store=create=add=make=put (8) |
| Comment line count in `*.c/*.h/*.py` | 0 | — |
| Number of packages/modality | 1 (shibadb-c core) + 3 modality | 10 subpackage |

## 12. Team dispatch (autonomous execution, ultracode)

Sau khi spec approved, invoke `writing-plans` skill để expand thành concrete plan. Plan sẽ execute qua Workflow tool multi-phase:

**Phase 0 (sequential, single agent)**:
- 1 agent chuyên `git`/`rm`/cleanup, chốt WIP trước → wipe → init fresh
- Isolation: git worktree không cần (destructive, one-way)

**Phase 1 (parallel, 4 team, isolation = worktree)**:
- **Team Durability** — 1 lead + 2 sub-agent (test writer, doc writer)
- **Team Concurrency** — 1 lead + 2 sub-agent
- **Team Format** — 1 lead + 1 sub-agent
- **Team Perf** — 1 lead + 2 sub-agent (bench writer, CI configurer)

**Phase 2 (research → migrate)**:
- 1 agent research binding options cho app target
- 1 agent viết binding scaffold
- 1 agent migrate data
- (User confirm target trước khi start)

**Phase 3 (verification, judge panel)**:
- Multi-lens review: correctness, security, perf, ergonomics
- Adversarial verify mỗi finding
- Loop-until-dry: rerun cho tới khi 2 round không mới

Budget: ultracode on, không giới hạn token.

## 13. Rủi ro & mitigation

| Rủi ro | Impact | Mitigation |
|---|---|---|
| Xóa `.git` mất history 7 commit shibadb-c | Không thể trace lịch sử decision | User confirm rõ; spec này ghi lại decision quan trọng |
| Xóa comment tuyệt đối gây ambiguity cho maintain sau | Khó hiểu code khi quay lại | `docs/*.md` gánh toàn bộ giải thích invariant |
| Track A single-mutex là bottleneck perf | Users complain read throughput | Roadmap 1.1 = Track B multi-reader (proposal đã có) |
| Dogfood app hit edge case shibadb chưa handle | Bug prod | Log everything, quick fix cycle, không blame user app |
| External audit không tìm được | Chậm 1.0 tag | AI reviewer từ nhiều model làm audit panel |
| Format V1 lộ bug cần V2 sớm | Break backward-compat | Migration path đã có stub, chỉ cần fill impl |

## 14. Success criteria

- shibadb-c là dependency của ≥1 app user chạy prod ≥30 ngày, không data issue
- README có thể bỏ pre-1.0 warning
- CI 7 job green liên tục 4 tuần
- User (và ≥1 external reviewer) tự tin recommend cho dự án lớn
- Codebase KHÔNG có bất kỳ signal nào của hvpdb-drift (metrics dashboard section 11 tất cả xanh)
