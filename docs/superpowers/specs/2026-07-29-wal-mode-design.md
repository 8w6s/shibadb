# WAL mode (deferred checkpoint) — design spec

_Sub-project 1 of the write-throughput roadmap. Ngày: 2026-07-29._

## Mục tiêu (tầng sản phẩm)

Đưa ShibaDB từ **checkpoint-mỗi-commit** (5-6 fsync/commit, ~60 ops/s) sang
**WAL mode**: commit chỉ ghi vào WAL + fsync WAL; việc dồn vào file dữ liệu
(checkpoint) tách ra chạy theo ngưỡng. Giữ **an toàn tuyệt đối** (không mất
giao dịch đã commit, không corrupt khi mất điện ở bất kỳ thời điểm nào).

Đây là nền tảng cho hai sub-project sau (coalescing writer, MVCC reads) — không
làm ở spec này.

## Định vị (trung thực)

WAL mode đưa mô hình durability của ShibaDB **ngang với SQLite WAL** (commit =
append + fsync; checkpoint tách rời). ShibaDB **không** tuyên bố ổn định hơn hay
nhanh hơn SQLite — SQLite có ~15 năm track record + 100% MC/DC coverage. Điểm
khác biệt thật của ShibaDB vẫn là **mã hóa mặc định** và (sau roadmap) **group
commit sẵn có**. Với payments/billing: coi là bản đang xây độ tin cậy.

## Bất biến không được phá (an toàn tuyệt đối)

1. Một txn được báo committed **chỉ sau khi** commit-record của nó đã fsync bền.
2. Mất điện ở **bất kỳ** điểm nào → reopen phải cho ảnh chụp nhất quán: mọi txn
   đã-báo-committed còn nguyên; txn dở dang biến mất hoàn toàn (không nửa vời).
3. Data file + superblock luôn hợp lệ độc lập; WAL là nguồn chân lý cho các txn
   `txn_id > superblock.checkpoint_lsn`.
4. On-disk **data page format không đổi**; chỉ WAL format + commit/checkpoint
   logic + read path thay đổi. Public API + hành vi quan sát được **không đổi**.

## Phạm vi concurrency (spec này)

**Giữ nguyên** mô hình hiện tại: single active transaction + mutex toàn cục
(reads cũng dưới mutex). Không thêm concurrency ở spec này. Điều đó có nghĩa
read path mới (đọc-xuyên-WAL) chỉ bị truy cập tuần tự → không cần khóa riêng
cho WAL index. Concurrency (coalescing writer, MVCC snapshot reads) là
sub-project sau, xây trên nền này.

---

## Technical design

### WAL format v3 — multi-txn append-only

Ngày nay WAL chứa đúng một txn và bị `resize(0)` mỗi commit. WAL v3 là nhật ký
append-only nhiều txn:

```
[wal header]   magic, version=3, page_size, file_id, base_checkpoint_lsn, header_crc
repeated:
  [frame]      page_id, txn_id, page_bytes(page_size, đã mã hóa nếu bật)
  ...
  [commit-rec] magic, txn_id, frame_count, next_page_id, freelist_page,
               running_crc(toàn bộ txn), commit_crc
```

- Mỗi txn = N frame + 1 commit-record. Chỉ khi commit-record hợp lệ + CRC khớp
  thì txn được coi committed (giữ nguyên tắc "commit-record là cái cuối cùng bền").
- `txn_id` đơn điệu tăng, nối tiếp `superblock.checkpoint_lsn`.
- Frame giữ nguyên cơ chế mã hóa/`page_lsn` hiện có (tái dùng `sdb_page_encode`
  + `sdb_encrypted_page_*`). Không đổi crypto.

### Commit path

```
begin(txn_id = ++current_lsn)
  stage pages (page_lsn = txn_id) trong page cache (dirty, pinned)
commit:
  append N frame vào cuối WAL (offset = wal_tail)
  fsync WAL                         # (1) data frames bền
  append commit-record
  fsync WAL                         # (2) commit điểm → TRẢ VỀ durable
  cập nhật in-memory: wal_index[page_id] = frame_offset; current_lsn = txn_id;
                      wal_tail tiến; (không đụng data file / superblock)
  nếu wal_tail ≥ checkpoint_threshold → checkpoint()
```

2 fsync/commit (thay 5-6). Data file + superblock không đụng khi commit.

### Read path — in-memory WAL index

`wal_index`: hash map `page_id → wal_frame_offset` cho các page có bản mới hơn
data file (tức thuộc txn chưa checkpoint). Cập nhật khi commit, xóa sạch khi
checkpoint. In-memory (single-process, dưới mutex → không cần khóa riêng).

`sdb_pager_read(page_id)`:
1. page cache có? → trả (cache là chân lý nóng nhất).
2. `wal_index` có `page_id`? → đọc frame từ WAL tại offset, decode/giải mã, trả.
3. còn lại → đọc từ data file như hiện tại.

Điều này thay quy tắc "cache giữ mọi dirty page" mong manh bằng nguồn chân lý
tường minh (WAL) → WAL có thể lớn hơn cache mà read vẫn đúng.

### Checkpoint (tách khỏi commit)

Kích hoạt khi `wal_tail ≥ threshold` (mặc định ~1000 page hoặc ~4 MB, cấu hình
qua option), chạy trên chính luồng writer khi ngưỡng đạt (spec này single-writer
nên chưa cần luồng checkpoint riêng):

```
checkpoint:
  với mỗi page trong wal_index (theo txn_id tăng dần):
      ghi page mới nhất vào data file tại offset của nó
  fsync data file                              # data bền TRƯỚC superblock
  superblock.checkpoint_lsn = current_lsn
  superblock.next_page_id / freelist_page = giá trị mới nhất
  ghi superblock (mirrored) + fsync superblock # commit điểm của checkpoint
  truncate WAL về 0 (KHÔNG cần fsync — xem branch perf/reduce-fsync: checkpoint_lsn
      dominance làm WAL cũ idempotent-vô hại)
  clear wal_index; wal_tail = 0
```

Idempotent: nếu crash trước khi superblock cập nhật, WAL còn nguyên, replay lại
cho cùng kết quả. Nếu crash sau superblock nhưng trước truncate, WAL cũ có
`txn_id ≤ checkpoint_lsn` → recovery bỏ qua.

### Recovery (reopen)

```
đọc superblock (checkpoint_lsn = C)
mở WAL; nếu file_id lệch → xóa WAL (như hiện tại)
scan WAL từ đầu:
  với mỗi txn có commit-record hợp lệ + CRC khớp + txn_id > C (theo thứ tự):
      áp frame của txn vào data file
      C = txn_id
  txn không có commit-record hợp lệ (dở dang, đuôi WAL) → DỪNG (bỏ phần đuôi)
nếu có áp: fsync data; superblock.checkpoint_lsn = C (+ next_page_id/freelist);
           fsync superblock; truncate WAL
rebuild xong về trạng thái nhất quán; wal_index rỗng (đã checkpoint hết)
```

Mở rộng recovery hiện tại từ "1 txn" thành "quét nhiều txn, dừng ở txn dở dang
đầu tiên". Nguyên tắc all-or-nothing per txn giữ nguyên.

### Tương tác mã hóa

Không đổi. Frame chứa page đã mã hóa (XChaCha20-Poly1305) như WAL hiện tại;
read path giải mã khi đọc frame từ WAL (đã có `sdb_encrypted_page_decrypt`).
`sdb_wal_validate_records` mở rộng để xác thực từng txn khi recovery.

### Public API / hành vi (không đổi)

`sdb_kv_put/get/delete`, `sdb_transaction_*`, `verify`, `backup`, `compact`,
`migrate` giữ nguyên chữ ký + ngữ nghĩa. `verify` mở rộng để chấp nhận trạng
thái "có WAL chưa checkpoint" là hợp lệ. `backup`/`compact` checkpoint trước
rồi thao tác trên ảnh đã dồn (đơn giản hóa, tránh phải copy WAL).

## Testing plan (điều kiện merge)

1. **Crash matrix mở rộng** — inject lỗi/kill tại mọi biên: mỗi fsync WAL, giữa
   append-frame và commit-record, mọi bước checkpoint (trước/giữa/sau data-sync,
   trước/sau superblock-sync, trước/sau truncate). Mỗi ảnh reopen phải verify +
   trả đúng ảnh cũ-hoặc-mới hoàn chỉnh. Gồm WAL đa-txn (2-3 txn tồn tại rồi crash).
2. **Recovery đa-txn** — WAL với K txn committed + 1 txn đuôi dở dang → replay
   K, bỏ đuôi.
3. **Read-through-WAL** — put chưa checkpoint rồi get phải thấy giá trị mới;
   sau checkpoint vẫn thấy; qua reopen vẫn thấy.
4. **Property/soak** — soak hiện có phải pass; thêm biến thể WAL lớn (nhiều txn
   trước checkpoint) + kill-inject.
5. **Fuzz** — target `fuzz_wal_recover` mở rộng cho format v3 đa-txn.
6. **TSan** — dù single-writer, chạy để chắc.
7. **Adversarial review** (Workflow, nhiều lens durability) trước khi coi là xong.
8. Toàn bộ ctest hiện tại vẫn 38/38.

## Rủi ro & giảm thiểu

- **Rủi ro cao nhất:** recovery đa-txn hoặc checkpoint sai → mất/hỏng dữ liệu
  committed. Giảm: crash-matrix mở rộng là điều kiện merge cứng; adversarial
  review; giữ nguyên tắc all-or-nothing + checkpoint_lsn dominance đã chứng minh.
- **WAL phình** nếu checkpoint không chạy (reader dài) — spec này không có reader
  song song nên không kẹt; vẫn đặt trần WAL + buộc checkpoint khi đạt.
- **Không human review code** — bù bằng verify nhiều lớp + không dùng production
  tới khi có track record.

## Ngoài phạm vi (sub-project sau)

- Coalescing writer (group commit, closure model, read-modify-write đúng).
- MVCC snapshot reads (read song song không khóa, thi read-concurrency với SQLite).
- Chế độ `synchronous=NORMAL` (nhanh hơn, mất vài txn cuối) — chỉ thêm nếu bạn
  muốn knob đó; mặc định giữ FULL (an toàn tuyệt đối).
