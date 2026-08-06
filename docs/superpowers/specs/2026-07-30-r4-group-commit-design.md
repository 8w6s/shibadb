# R4 — Leader/follower group commit (sub-project 2 proper)

Đích cuối track write-throughput: nhiều luồng submit txn, một leader gom cả
batch vào MỘT fsync → phá trần `~1/fsync` commit-bền/s, mục tiêu 1000-5000
commit-bền/s cho 10-50 luồng song song. Base `main` @ `5181f6d`. Branch
`feat/r4-group-commit`.

## Nền đã có (R1+R2)
- Frame v4 self-describing + running-CRC → biên txn xác định, mỗi txn recover
  độc lập (R1). **On-disk semantics KHÔNG đổi**: batch chỉ là nhiều txn (frames
  + commit-record) nối tiếp trong WAL; recovery replay từng txn như thường.
- Persistent WAL fd + single-fsync/commit (R2a) → leader chỉ cần một fsync cho
  cả vùng batch.
- Hôm nay: engine `mutex` serialize MỌI commit (single-writer); N luồng ⇒ N
  fsync tuần tự. Đây là trần R4 phá.

## Thiết kế: PREPARE (mutex ngắn) + DURABILITY (leader gom)

Chia `commit` làm hai pha:

### PREPARE — dưới mutex, ngắn, không I/O chậm
- Encode + encrypt frames của txn (CPU, không fsync).
- Assign `txn_id = ++current_lsn` (monotone).
- Reserve vùng WAL byte không chồng lấn: `[wal_tail, wal_tail + txn_size)`;
  advance `wal_tail`.
- Đẩy txn (đã prepared: buffer frames+commit-rec, offset, txn_id) vào **commit
  queue**; ghi nhận "chưa durable". Nhả mutex.

### DURABILITY — một leader, phần còn lại là follower chờ
- Luồng đầu tiên vào pha durability trở thành **leader**; các luồng khác thành
  **follower** chờ trên condvar tới khi txn của mình được đánh dấu durable.
- Leader: gom mọi txn prepared trong queue, `pwrite` toàn bộ frames+commit-recs
  của batch vào vùng WAL đã reserve (liên tiếp), **một fsync** cho cả batch,
  cập nhật wal_index cho mọi page của batch, rồi **wake** tất cả follower có
  txn ≤ điểm durable.
- Follower thức dậy khi txn mình durable → trả `SDB_OK`.

## Bất biến TUYỆT ĐỐI (durability + concurrency)
1. Committed = commit-record của txn nằm trong vùng đã fsync. Batch chưa fsync →
   toàn bộ chưa committed; crash → recovery drop (running-CRC/biên v4). Không txn
   nào trong batch "committed một nửa".
2. `txn_id` monotone, cấp dưới mutex; vùng WAL reserve KHÔNG chồng lấn giữa các
   txn (mutex bảo vệ wal_tail advance).
3. **Single-writer-to-disk giữ nguyên**: chỉ leader ghi WAL + fsync tại một thời
   điểm. Đây KHÔNG phải multi-writer (không nhiều luồng cùng ghi disk) — nên
   không đụng bất biến "single-writer" mà roadmap để deferred; chỉ coalesce.
4. Không mất wakeup (lost-wakeup), không deadlock, không double-free txn buffer,
   không txn kẹt queue nếu leader lỗi (leader lỗi → propagate lỗi cho mọi
   follower trong batch, không ai báo OK sai).
5. `current_lsn`/`wal_tail`/`wal_index` nhất quán sau mọi batch, kể cả batch lỗi
   (rollback reserve nếu fsync fail → mọi txn batch trả lỗi, wal_tail/lsn không
   để lại vùng "committed ma").
6. Checkpoint / close / compact tương tác đúng: không group-commit khi đang
   checkpoint; batch drain trước checkpoint.

## Concurrency primitives
Cần mutex + condvar. Kiểm `src/` có `sdb_cond`/`sdb_mutex` (nếu chưa có condvar,
thêm wrapper mỏng quanh pthread_cond / Windows CONDVAR — cross-platform như
`sdb_mutex`). Leader/follower đồng bộ qua (mutex, condvar, generation counter
"durable_lsn").

## API — MÔ HÌNH ĐÃ CHỐT: chung 1 handle, đa luồng (option A)

Nhiều luồng gọi `sdb_transaction_commit` (và txn API) đồng thời trên **CÙNG một
`sdb_database` handle**; group commit gom nội bộ. Tự nhiên như SQLite WAL, không
bắt user học API mới.

Hệ quả kiến trúc (điểm khó nhất): hôm nay `SDB_ENGINE_LOCK_OR_RETURN` lock
`database->mutex` quanh TOÀN BỘ mỗi API → serialize hoàn toàn. R4 phải thu hẹp
vùng giữ mutex xuống chỉ pha PREPARE (encode/encrypt, assign txn_id, reserve WAL
range — nhanh, không fsync), rồi nhả mutex cho pha DURABILITY (leader fsync,
follower chờ trên condvar). Mọi trạng thái chia sẻ chạm ngoài mutex (commit
queue, durable_lsn, condvar) phải thread-safe. Đây là nguồn rủi ro race chính —
TSan là cổng bắt buộc. Nếu thu hẹp mutex làm lộ path chưa thread-safe không xử
lý gọn được, DỪNG bàn lại thay vì ép.

## Test (TDD) — TSan là CỔNG CHÍNH
- Concurrency: N luồng (10-50) commit đồng thời → tất cả trả OK, mọi txn durable
  sau reopen, txn_id monotone, không mất txn, không double-apply.
- **TSan**: 0 data race trên commit queue / wal_tail / current_lsn / wal_index /
  condvar. Đây là cổng bắt buộc — R4 KHÔNG merge nếu TSan có race.
- Crash-matrix batch: crash giữa leader pwrite / trước fsync / sau fsync →
  all-or-nothing cho CẢ batch (không txn nào nửa-committed); txn batch trước đã
  durable không mất.
- Fault-inject leader fsync fail → mọi follower trong batch nhận lỗi, không ai
  OK sai; wal_tail/current_lsn rollback nhất quán.
- Throughput bench: 10-50 luồng, đo commit-bền/s (mục tiêu 1000-5000) so single.
- Regression: toàn ctest + ASan + enc-soak + fuzz recover vẫn 0 crash.

## Không làm trong R4
- Multi-writer thật (nhiều luồng cùng ghi disk) — vẫn single-leader-to-disk.
- MVCC snapshot reads (R6). Cross-process. Replication.
- Không đổi on-disk WAL format (batch = txn nối tiếp, recovery không đổi).

## Rủi ro & cách kiểm soát
R4 là concurrency-critical, rủi ro race/deadlock/lost-wakeup cao nhất roadmap.
Kiểm soát: spec protocol rõ (trên) → implement từng bước → **TSan bắt buộc 0
race** → Workflow adversarial (lens: durability-batch, concurrency-race,
leader-failure, lost-wakeup/deadlock) verify từng finding → chỉ merge khi mọi
lớp sạch. Nếu implement lộ ra mô hình handle/thread mâu thuẫn, DỪNG bàn lại
thiết kế thay vì ép.
