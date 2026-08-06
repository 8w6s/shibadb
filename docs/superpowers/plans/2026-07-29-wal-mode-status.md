# WAL mode — trạng thái & sẵn sàng merge (2026-07-29)

Sub-project 1 của roadmap write-throughput. Branch `feat/wal-mode` (fork từ
`main` @ `0cf59a4`). `main` chưa bị đụng, chưa push.

## Đã hoàn thành

WAL mode đầy đủ (commit = append WAL + 2 fsync; đọc xuyên WAL index; checkpoint
theo ngưỡng + khi close; recovery đa-txn all-or-nothing, dừng ở torn tail).

| Task | Nội dung | Commit |
|---|---|---|
| T1 | WAL v3 commit-record encode/decode | `77decef` |
| T2 | in-memory wal_index | `81743da` |
| T3 | multi-txn append (2 fsync ordering) | `b6c6b86` |
| T4 | multi-txn recovery, stop at torn tail | `e8fe064`+`6ae71e1` |
| T7 | pager foundation: append-commit + open recovery | `3d79f24` |
| T5+T6 | read-through + checkpoint (on threshold + on close) | `c38bd8e` |
| — | fix: compact WAL clear + checkpoint threshold + backup | `1c4e787` |
| T8 | crash-matrix đa-txn (MERGE GATE 1) | `7d95912` |
| T9 | fuzz v3 harness + docs (WAL_FORMAT, OPERATIONS) | `4c91cc1` |
| — | final-review fixes: 2 data-loss + Python UAF | `5892923` |

`ctest` 41/41 xanh, build strict warning-clean (`-Werror`).

## Bằng chứng verify & review

- **Review từng task**: clean (T4 qua 1 fix round).
- **Integration review** (adversarial, opus): tìm & vá **1 CRITICAL** — compact
  nhận source WAL không clear → reopen replay stale frames → corruption.
- **Merge gate 1 — crash-matrix đa-txn**: 0 bug durability; fault-injection tại
  mọi boundary commit + checkpoint, assert all-or-nothing (test genuine, đã review).
- **Final review** (5-lens adversarial, opus): tìm & vá **2 CRITICAL + 1 Important**:
  1. backup + partial-checkpoint → mất txn kế tiếp (regression của fix trước).
  2. `COMMIT_SIZE` 24→44 phá legacy v1/v2 recovery → mất txn khi upgrade+crash.
  3. checkpoint-on-close `SDB_E_IO` sau free → Python double-close use-after-free.
  Fix wave `5892923` + scoped re-review: **tất cả addressed, crash-safe xác nhận
  bằng trace end-to-end recover_all**, không breakage mới. Encryption lens: clean.
- **Re-verify runtime** (`5892923`): TSan + ASan-soak + encrypted-soak + fuzz 30′
  — _đang chạy; điền kết quả khi xong._

Tổng: **4 bug nghiêm trọng (3 data-loss + 1 memory-safety) bị bắt trước merge.**

## Sẵn sàng merge

- **Điều kiện merge**: (a) final review clean ✅; (b) re-verify green ⏳ (đang chạy).
- **API công khai KHÔNG đổi** — code hiện có chạy y nguyên, chỉ nhanh hơn.
- **On-disk data page format KHÔNG đổi** (chỉ WAL format + commit/checkpoint logic).
- `main` xanh, không đụng. Merge là quyết định của bạn khi re-verify xanh.
- **Chưa dùng production tới khi có track record** (theo cảnh báo định vị: đây là
  bản đang xây độ tin cậy, không cắm vào hệ thống tiền thật ngay).

## Backlog (KHÔNG chặn merge — hardening/dọn dẹp)

- **recover_all thiếu WAL-header file_id gate**: cửa sổ crash hẹp (1 fsync) trong
  compact; nên thêm file_id gate hoặc reorder `replace_finish` (unlink WAL trước
  marker) — cần crash-injection coverage. Đã đóng path vận hành bằng defense-in-depth.
- **Python version drift**: `test_python_binding.py` assert `0.1.0` nhưng lib báo
  `1.0.0` (pre-existing, không liên quan WAL mode).
- Vài minor deferred (comment, test paranoia) ghi trong ledger SDD.

## Roadmap tiếp (write-throughput, sau khi merge WAL mode)

- **Sub-project 2 — coalescing writer**: group commit ở tầng an toàn (nhiều luồng
  submit closure atomic, block tới khi durable, gom vào 1 fsync). Đạt mục tiêu
  1000-5000 commit-bền/s cho 10-50 luồng song song. Cần WAL mode làm nền (đã có).
- **Sub-project 3 — MVCC snapshot reads** (tùy chọn): reader song song không khóa,
  để thi read-concurrency với SQLite WAL.
- Mỗi sub-project: spec → plan → TDD → 2 merge gate riêng.
