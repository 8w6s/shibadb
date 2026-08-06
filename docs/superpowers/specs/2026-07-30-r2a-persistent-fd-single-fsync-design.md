# R2a — Persistent WAL fd + collapse two commit fsyncs to one

Track scale, roadmap rank 2 (sau R1). Bỏ open()+close() WAL mỗi commit và gộp
hai fsync commit thành một, ~2× tốc độ ghi đơn luồng và mở đường R4 group
commit. Base `main` @ `47fb8cf`. Branch `feat/r2a-persistent-fd`.

## Vấn đề (đọc code trước)

Đường commit hiện tại (`sdb_txn_commit` → `sdb_wal_append_txn`):
- Mỗi commit mở WAL fd rồi đóng (open()+close() syscall pair). Xem cách pager
  truyền wal_path / mở fd trong commit path.
- `sdb_wal_append_txn` (src/wal.c) ghi frames → **fsync #1** → ghi commit-record
  → **fsync #2**. Hai durability barrier/commit → ~231 auto-commit/s ≈ 4.3 ms.

## Thiết kế

1. **Persistent WAL fd**: pager giữ một `sdb_file wal_file` mở suốt đời (mở khi
   open/create, đóng khi close), thay vì open/close mỗi commit. Commit ghi qua
   fd sẵn có.
2. **Gộp 2 fsync → 1**: ghi `frames || commit-record` bằng MỘT vùng pwrite liên
   tiếp tại wal_tail rồi **một fsync duy nhất**. Bỏ fsync giữa frames và
   commit-record.

## Vì sao 1 fsync AN TOÀN (durability guard — bắt buộc giữ)

- fsync **chính là** ordering barrier: sau fsync, toàn bộ (frames + commit-rec)
  durable; trước fsync, KHÔNG có gì được coi là durable.
- Recovery tính lại **running-CRC trên frames** trước khi chấp nhận commit-rec.
  Crash trước fsync → frames/commit partial hoặc absent → running-CRC hoặc
  decode fail → **torn → drop** (txn coi như chưa commit — đúng bất biến). Crash
  sau fsync → tất cả durable → committed. **commit-record KHÔNG BAO GIỜ được
  validate tách rời khỏi frames của nó** (đây là điều khiến gộp 1-fsync an toàn,
  không phải shortcut). Nền này do R1 (frame v4 self-describing + running-CRC
  trong commit-rec) đã dựng.
- Đây đúng mô hình single-flush của SQLite/Postgres, được torn-detector CRC
  bảo vệ — không nới lỏng guarantee.

## Persistent-fd lifecycle (điểm dễ sai — làm đúng hết)

- **Checkpoint**: `sdb_wal_clear`/checkpoint truncate WAL về 0. Persistent fd
  phải reset append offset về `SDB_WAL_HEADER_SIZE` trên CHÍNH fd đang sống
  (không phải fd stale), và lần append kế lại ghi header (first-append) như R1.
- **compact swap**: swap adopt/replace WAL — persistent fd phải trỏ đúng file
  sau swap (re-open/re-fsync trên inode mới, không giữ fd cũ trỏ file đã unlink).
- **Error paths**: không được leak một fd nửa-đóng khiến các fsync sau âm thầm
  rơi. Mọi nhánh lỗi đóng/khôi phục fd nhất quán.
- Giữ **parent-dir fsync một lần** khi tạo WAL lần đầu (durable directory entry).

## Bất biến TUYỆT ĐỐI

- Committed txn không bao giờ mất ở bất kỳ crash point.
- Torn tail drop sạch (all-or-nothing per txn); commit-rec validate CÙNG frames
  (running-CRC), không tách rời.
- Single-fsync KHÔNG nới lỏng: fsync vẫn là barrier, running-CRC vẫn phát hiện
  torn. Identity header (R1) + frame v4 giữ nguyên.
- KHÔNG đụng recovery logic (chỉ đường ghi + fd lifecycle).

## Test (TDD)

- **Crash matrix single-fsync**: mở rộng `tests/test_transaction_crash.c` —
  fault-inject cắt tại: giữa pwrite frames+commit (→ torn → drop), sau pwrite
  trước fsync (→ chưa durable → drop), sau fsync (→ committed). Assert
  all-or-nothing + không mất txn đã-committed-trước-đó.
- **Persistent-fd lifecycle**: commit → checkpoint (clear) → commit lại (fd
  reset offset đúng, header re-stamp); compact → commit (fd trỏ file mới);
  close→reopen liên tục không leak fd / không hỏng.
- **Fault-inject fsync fail**: single fsync fail → commit trả lỗi, txn không
  coi là committed, reopen nhất quán.
- Regression: toàn `ctest` pass; TSan (fd không race single-writer); ASan;
  enc-soak; build `-Werror`. Fuzz recover vẫn 0 crash (format không đổi).

## Không làm trong R2a

Group commit / leader-follower (R4 — R2a là nền đơn-luồng cho nó). MVCC (R6).
Không đổi on-disk format (frames v4 + commit-rec giữ nguyên; chỉ bỏ 1 fsync +
giữ fd).
