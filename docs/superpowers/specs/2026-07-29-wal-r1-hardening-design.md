# R1 — WAL hardening: self-identifying header + self-describing frames

Sub-project của roadmap "DB engine hoàn chỉnh". Mục tiêu: đóng hai lỗ durability
trong recovery WAL-mode để đạt **durability tuyệt đối không ngoại lệ xác suất**,
đồng thời **mở khóa group commit** (R4) bằng biên txn tự mô tả.

Base: `main` @ `bb5c040` (WAL v3 đã merge). Branch làm việc: `feat/wal-r1-hardening`.

## Vấn đề (đã verify trên code thật)

### Bug (a) — `recover_all` không kiểm định danh WAL
`sdb_wal_recover_all` (src/wal.c:832) bắt đầu quét từ `cursor = SDB_WAL_HEADER_SIZE`
mà **không đọc/validate 64 byte header đầu**. QUAN TRỌNG (đã verify code): header
v3 do `sdb_wal_append_txn` (wal.c:343-390) ghi thực chất là **zero-hole** — hàm này
chỉ ghi frames + commit-rec tại `append_offset`, KHÔNG đụng `[0,64)`; 64 byte đầu
toàn 0. (`wal.c:208-223` là `sdb_wal_write_committed` — đường **legacy v2 single-txn**
do `sdb_wal_recover` xử lý, có `memcmp` file_id tại wal.c:593,646 — KHÔNG phải đường
v3 mà `recover_all` đọc.) Do đó **WAL v3 hiện KHÔNG có định danh `file_id` nào** để
`recover_all` kiểm — không thể phân biệt WAL của mình với foreign sidecar.

**Hệ quả:** với DB **không mã hóa**, một file `.wal` ngoại lai/sót cùng `page_size`
mà txn đầu có `txn_id == checkpoint_lsn+1` và running-CRC hợp lệ sẽ được apply, ghi
đè trang data đã commit. (DB mã hóa đã an toàn: `sdb_wal_validate_frames` wal.c:909
decrypt bằng data_key → foreign WAL key khác sẽ fail.)

**Phạm vi:** hẹp (chỉ plaintext DB) nhưng vi phạm bất biến "committed không bao giờ
mất/hỏng". Đây là item "file_id gate" đã ghi backlog.

### Bug (b) — frame không tự mô tả, biên txn tìm bằng dò byte
Frame hiện là `[page_id:8B][page data:page_size]` (`sdb_wal_frame_size` =
`SDB_WAL_RECORD_HEADER_SIZE(8) + page_size`, wal.c:992). Recovery không biết txn có
mấy frame → thử `k = 1,2,3,…` và tìm `k` mà tại `cursor + k*frame_size` có magic
`'SCMT'` rồi decode (wal.c:853-867).

**Hệ quả:** một txn đã fsync mà page bytes (mã hóa, ngẫu nhiên) tình cờ chứa `'SCMT'`
tại đúng bội `frame_size` + qua được cả 3 lớp chắn (decode self-CRC + `frame_count==k`
+ running-CRC trên k frame) sẽ bị nhận nhầm → `torn` → dừng recovery sớm → mất các
txn sau. Xác suất ~2⁻⁶⁴ (rất nhỏ nhờ 3 lớp CRC hiện có) — đây là **hardening để đạt
"không ngoại lệ xác suất"**, không phải bug dễ trigger.

## Thiết kế

Phần A phải **ghi header identity** (write-path) rồi mới validate được — không thể
"chỉ validate" vì header v3 hiện là zero-hole. Làm A trước (chỉ đụng `[0,64)`, không
đổi frame format); B đổi frame format sau.

### Phần A — WAL header identity + validate (ĐỔI write-path, KHÔNG đổi frame format)

**Ghi header** (`sdb_wal_append_txn`): khi `append_offset == SDB_WAL_HEADER_SIZE`
(txn đầu vào WAL mới / sau clear), ghi 64B header vào `[0,64)` TRƯỚC frames, trong
cùng lần ghi + fsync đầu (header durable cùng/ trước frames, trước commit-rec fsync).
Layout tái dùng khuôn `write_committed` (wal.c:208-223): magic@0 + version@4 +
header_size@6 + txn_id@8 + page_size@16 + page_count@20 + **file_id@24** (16B) +
next_page_id@40 + freelist@48 + CRC@`SDB_WAL_HEADER_CHECKSUM_OFFSET`. Append sau
(offset>64) KHÔNG đụng header. Version trong header = `SDB_WAL_VERSION_V3` (Phần A
không đổi frame; B sẽ bump khi đổi frame).

**Validate** (`sdb_wal_recover_all`): sau khi đọc buffer, trước vòng lặp:
- Nếu 64B header **all-zero** → WAL v3 **pre-R1** (crash trước khi có header ghi):
  BACKWARD-COMPAT — recover theo đường hiện tại (không cổng file_id). Không mất data cũ.
- Nếu header **non-zero** → validate đầy đủ: magic == `sdb_wal_magic`, version hợp lệ,
  `header_size@6 == 64`, `page_size@16 == superblock->page_size`,
  **`file_id@24 == superblock->file_id`** (memcmp 16B — cổng chính), CRC hợp lệ.
  Bất kỳ mismatch → `SDB_E_CORRUPT`, KHÔNG apply gì (free + close). Tái dùng khuôn
  check của `sdb_wal_recover` (wal.c:571-649).

**Durability:** header ghi + fsync trong lần fsync frames đầu (trước commit-rec fsync)
→ nếu crash trước commit-rec, txn chưa commit (recover bỏ, đúng bất biến); nếu header
ghi lỗi → cả txn đầu chưa durable. Không có cửa sổ mất committed txn. Backward-compat
zero-hole không nới lỏng gì so với hiện tại (chỉ WAL pre-R1, sau checkpoint sẽ clear
+ WAL kế có header).

### Phần B — self-describing frames (đổi format → WAL v4)
Mở rộng frame header `8B → 24B`:

```
offset  size  field
0       8     page_id           (u64 LE)   — như cũ
8       8     txn_id            (u64 LE)   — MỚI: frame thuộc txn nào
16      4     frame_index       (u32 LE)   — MỚI: thứ tự frame trong txn (0-based)
20      4     frame_count       (u32 LE)   — MỚI: tổng số frame của txn
                                             (khớp commit_rec.frame_count)
24      …     page data (page_size)
```

`sdb_wal_frame_size` → `24 + page_size`. Recovery đọc `frame_count` từ frame đầu của
txn → biết chính xác commit-record ở đâu (`cursor + frame_count*frame_size`), **không
dò byte**. Validate: mọi frame trong txn có cùng `txn_id`, `frame_index` liên tục
`0..frame_count-1`, `frame_count` khớp commit-rec. Biên txn trở nên **xác định** →
không thể nhầm page-data thành commit-record.

Bump `SDB_WAL_VERSION_V3(3) → SDB_WAL_VERSION_V4(4)` cho header + commit-rec + frame.

### Backward-compat & migration
- **Đọc:** open kiểm version trong header. v4 → đường recovery mới (biên xác định).
  v3 → giữ nguyên đường hiện tại (dò-magic + 3 lớp CRC — vẫn an toàn như hôm nay).
  Không có WAL nào trộn hai version (WAL bị clear sạch mỗi checkpoint/close).
- **Ghi:** luôn ghi v4.
- **Migration tự nhiên:** crash với WAL v3 cũ + mở bằng binary mới → recover đường v3
  → checkpoint → clear → WAL kế tiếp là v4. Clean shutdown → WAL rỗng → v4 từ đầu.
  Không cần bước migrate thủ công.

## Bất biến phải giữ
1. Committed txn (commit-record đã fsync) không bao giờ mất ở bất kỳ điểm crash nào.
2. Torn tail bị bỏ toàn bộ (all-or-nothing per txn).
3. running-CRC vẫn là torn-detector chính; frame-header mới là lớp *bổ sung* xác định
   biên, không thay running-CRC.
4. Không apply frame nào từ WAL có file_id/page_size khác superblock.

## Chiến lược test
- **Unit:** (a) recover_all từ chối foreign file_id / sai page_size / magic hỏng;
  (b) frame v4 encode/decode round-trip; biên txn đọc đúng khi page-data chứa `'SCMT'`
  cố ý; v3 đọc được bằng binary v4 (backward-compat).
- **Crash matrix:** mở rộng `tests/test_transaction_crash.c` — recover hỗn hợp: WAL v3
  cũ (migration) và v4, cắt tại mọi boundary, assert all-or-nothing.
- **Fuzz:** cập nhật `fuzz/fuzz_wal_recover.c` cho frame v4 (biên xác định) + giữ path
  đọc v3. Chạy lại campaign SAU khi R1 xong (fuzz pre-R1 không còn giá trị vì format đổi).
- Toàn bộ: `ctest` xanh, strict `-Werror`, TSan + ASan-soak + enc-soak sạch.

## Không làm trong R1 (để sub-project sau)
- Persistent WAL fd + gộp 2 fsync→1 (R2a — sau khi biên xác định đã có).
- Group commit (R4).
- Bất kỳ thay đổi read/cache nào.
