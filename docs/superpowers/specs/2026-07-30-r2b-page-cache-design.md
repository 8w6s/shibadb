# R2b — Hash-indexed, byte-configurable page cache

Sub-project đầu của track scale (roadmap rank 2, "transformational"). Thay page
cache linear-scan/fixed-64 bằng hash-index O(1) + capacity cấu hình theo byte,
để đọc vẫn nhanh khi dataset vượt RAM. Base `main` @ `d47dc95`. Branch
`feat/r2b-page-cache`.

## Vấn đề (đã đọc code)

`src/page_cache.c`: `get`/`put`/`remove` đều **linear-scan O(capacity)** toàn
mảng `entries`; `put` còn quét tìm victim min-stamp (LRU) cũng O(capacity).
Capacity cố định `SDB_PAGER_CACHE_CAPACITY` (64 pages ≈ 256 KiB). Hệ quả:
- 256 KiB không giữ nổi phần interior của B+Tree lớn → mỗi point-read re-fetch
  index page từ disk.
- Không nâng capacity được: cache 64k-page sẽ khiến mỗi `get` O(64k).

## Thiết kế

- **Hash-index open-addressing** (`page_id → entry slot`), tái dùng đúng pattern
  `src/wal_index.c` (linear-probe, power-of-2 capacity, nhân golden-ratio hash).
  → O(1) trung bình cho get/put/remove/evict. Đây là thay đổi cốt lõi.
- **Capacity byte-config**: pager open tính `capacity = cache_bytes ?
  clamp(cache_bytes / page_size, MIN, MAX) : SDB_PAGER_CACHE_CAPACITY`. Mặc định
  (cache_bytes==0) giữ y hành vi hiện tại.
- **Eviction CLOCK (second-chance)** O(1) amortized: ref-bit/entry + clock hand
  quét, thay linear min-stamp. (Nếu CLOCK phức tạp hóa quá thì segmented-LRU
  cũng chấp nhận, miễn evict không còn O(capacity).)
- **Struct**: hash slots (page_id→index, hoặc probe trực tiếp trên entries) +
  entries (page_id, ref-bit, valid, bytes) + storage contiguous + clock_hand.

## Bất biến TUYỆT ĐỐI (giữ nguyên)

1. **Write-through**: `put` chỉ chạy SAU durability barrier (data fsync ở
   journal-mode / frame durable ở WAL-mode). KHÔNG BAO GIỜ write-back — không
   dirty page nào chỉ sống trong cache, nếu không cửa sổ mất-commit tái xuất.
2. Cache chỉ giữ **clean plaintext COPIES** → durability-neutral.
3. `sdb_secure_zero(storage)` khi destroy (giữ policy chống data-remanence).
4. `get` trả **copy** (memcpy ra ngoài), không lộ buffer nội bộ — để sau này
   lock-free reads publish page bằng swap-con-trỏ, không mutate-tại-chỗ.
5. Không double-membership (1 page_id ≤ 1 entry), không stale sau remove/evict.

## API

Giữ signature public: `sdb_page_cache_init/get/put/remove/destroy`. Thân struct
`sdb_page_cache` đổi (thêm hash + clock). `init` vẫn nhận `capacity` (pager tính
từ cache_bytes). Callers (pager.c read-through + commit cache path) KHÔNG đổi
ngoài giá trị capacity truyền vào.

## Wire vào pager/engine

- `sdb_pager_open`: `capacity = cache_bytes ? clamp(...) : SDB_PAGER_CACHE_CAPACITY`.
- `sdb_database_options` thêm field `cache_bytes` (0 = default). **ABI cẩn thận**:
  append cuối struct + tôn trọng `struct_size` versioning trong
  `sdb_database_options_init` và validate; option cũ (không set) → default.
- Engine truyền cache_bytes xuống pager.

## Test (TDD)

- Hash correctness: get/put/remove đúng qua nhiều page_id, collision (nhiều id
  cùng slot ban đầu), cache đầy.
- Eviction: cache đầy → evict theo CLOCK/second-chance, không double-membership,
  không mất entry còn ref gần đây bừa bãi.
- Byte-config: cache_bytes → capacity đúng + clamp MIN/MAX; 0 → default.
- Write-through preserved: integration (commit → cache chỉ sau durable) không
  regress; crash-matrix hiện có vẫn all-or-nothing.
- Regression: `test_page_cache` hiện có pass; toàn `ctest` pass; ASan (memory);
  build `-Werror`.
- (tùy) micro-bench: get throughput khi working-set > cache, so linear cũ.

## Không làm trong R2b

MVCC/lock-free reads (R6); persistent-fd + group-commit (R2a/R4); mmap; nén.
Chỉ đổi cache indexing + capacity config; không đụng durability path.
