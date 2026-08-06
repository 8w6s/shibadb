# shibadb-c Phase 0 — Clean Slate Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Đưa `~/project/shibadb/` từ trạng thái hai-thân-song-song (`shibadb/` Python legacy + `shibadb-c/` với git riêng) về **fresh sole-authored git repo**, không comment trong code, không doc duplicate, không build dir vương vãi. Xong Phase 0 = baseline clean, sẵn sàng Phase 1 hardening.

**Architecture:** Sequential destructive operations trên filesystem. Không TDD-style vì đây là cleanup, không tạo feature. Verification = build còn compile + tests còn pass + metric gate (0 comment / 0 duplicate / 0 legacy dir).

**Tech Stack:** shell (bash), git, `sed`/`gcc -fpreprocessed -E` (strip C comment), Python `tokenize` module (strip Python comment), CMake presets.

## Global Constraints

- **Không** cắt API `sdb_*` public — chỉ dọn source form, không đụng semantic.
- **Không** thay đổi behavior của bất kỳ hàm nào — Phase 0 không phải là refactor logic.
- **Không** rebase hay giữ history từ git shibadb-c cũ — repo mới hoàn toàn fresh.
- **Giữ** `docs/*.md` (bao gồm spec, proposal) — comment ban chỉ áp cho `*.c/*.h/*.py`.
- **Giữ** `LICENSE` file ở root (MIT, one-file). Không header trong file.
- **User = sole author** trong git repo mới. Config `user.name = satoharuki`, `user.email` từ env `GITHUB_PAT`-linked email hoặc `git config --global user.email`.
- **Backup trước mọi destructive op** — tarball ra `/tmp` với timestamp.
- **Verification sau mỗi task destructive**: `cmake --preset gcc && cmake --build build/gcc && ctest --test-dir build/gcc --output-on-failure` phải pass.

---

## File Structure (sau Phase 0)

```
~/project/shibadb/                    ← git repo root (fresh init)
├── .git/                             ← sole-authored, 1 commit "initial"
├── .gitignore                        ← chuẩn C (build*, *.o, *.gcda, ...)
├── LICENSE                           ← MIT, giữ nguyên
├── README.md                         ← positioning, pre-1.0 warning
├── CMakeLists.txt                    ← từ shibadb-c cũ, không comment
├── CMakePresets.json                 ← 9 preset đã có
├── SECURITY.md, CONTRIBUTING.md, CODE_OF_CONDUCT.md, CHANGELOG.md, ROADMAP.md, RELEASE.md, CONTEXT.md
├── include/                          ← shibadb.h, shibadb_engine.h, shibadb_visibility.h (0 comment)
├── src/                              ← 69 .c files (0 comment)
├── tests/                            ← 44 test C files + 3 py bindings (0 comment)
├── scripts/                          ← check_reproducible, package_release, ...
└── docs/                             ← 21 doc file (đã gộp 3 cặp duplicate)
    ├── superpowers/
    │   ├── specs/2026-07-28-shibadb-c-hardening-design.md
    │   └── plans/2026-07-28-phase-0-clean-slate.md  ← file này
    ├── AUDIT.md                      ← merged
    ├── COVERAGE.md                   ← merged
    ├── SECURITY_REVIEW.md            ← merged
    └── … (18 doc khác)
```

**Xóa hoàn toàn**:
- `~/project/shibadb/shibadb/` (Python legacy)
- `~/project/shibadb/refs/` (rust-v0 + python-v0-docs)
- `~/project/shibadb/shibadb-c/` (thư mục container, content move lên root)
- 11 `build*/` trong shibadb-c
- `docs/hvpdb-compatibility.md`
- `docs/README_legacy.md`
- `docs/AUDIT_2026_07.md`, `docs/COVERAGE_2026_07.md`, `docs/SECURITY_REVIEW_2026_07_25.md`
- `.git/` cũ của shibadb-c

---

### Task 1: Backup safeguard

**Files:**
- Create: `/tmp/shibadb-pre-phase0-<timestamp>.tar.zst`

**Interfaces:**
- Consumes: nothing
- Produces: backup path (một file duy nhất) — nếu Task 2+ hỏng, `tar --zstd -xf` restore được.

- [ ] **Step 1: Đo dung lượng trước để chọn compress level**

Run: `du -sh ~/project/shibadb/`
Expected: một số ~500MB-1GB (bao gồm build dir + coverage + Python venv nếu có).

- [ ] **Step 2: Tạo tarball backup**

Run:
```bash
TS=$(date -u +%Y%m%dT%H%M%SZ)
tar --zstd -cf /tmp/shibadb-pre-phase0-${TS}.tar.zst -C ~/project shibadb
ls -lh /tmp/shibadb-pre-phase0-*.tar.zst
```
Expected: một file `.tar.zst`, kích thước ~100-300MB tùy compression.

- [ ] **Step 3: Verify tarball readable**

Run: `tar --zstd -tf /tmp/shibadb-pre-phase0-*.tar.zst | head -5`
Expected: hiển thị `shibadb/`, `shibadb/shibadb-c/`, `shibadb/shibadb/`, `shibadb/refs/`, `shibadb/docs/`.

- [ ] **Step 4: Ghi tarball path**

Run:
```bash
echo "Backup: $(ls -1t /tmp/shibadb-pre-phase0-*.tar.zst | head -1)" > /tmp/shibadb-phase0-backup.txt
cat /tmp/shibadb-phase0-backup.txt
```
Expected: dòng "Backup: /tmp/shibadb-pre-phase0-*.tar.zst".

---

### Task 2: Audit WIP hiện tại (không interactive — auto-decide theo rule)

**Files:**
- Modify: (none — chỉ đọc)
- Create: `/tmp/shibadb-phase0-wip-decision.md`

**Interfaces:**
- Consumes: Backup từ Task 1 (an toàn để inspect).
- Produces: `wip-decision.md` ghi rõ file nào giữ, file nào revert. Task 20 dùng để commit đúng subset.

- [ ] **Step 1: List modified + untracked trong shibadb-c**

Run:
```bash
cd ~/project/shibadb/shibadb-c
git status --short
```
Expected:
```
 M CMakeLists.txt
 M include/shibadb_engine.h
 M src/engine.c
?? docs/PROPOSAL_MULTI_READER.md
?? tests/test_enumerate.c
```

- [ ] **Step 2: Đo diff size**

Run:
```bash
cd ~/project/shibadb/shibadb-c
git diff --stat CMakeLists.txt include/shibadb_engine.h src/engine.c
```
Expected: bảng số line thay đổi mỗi file.

- [ ] **Step 3: Inspect diff nhanh — có phải comment-only không**

Run:
```bash
cd ~/project/shibadb/shibadb-c
for f in CMakeLists.txt include/shibadb_engine.h src/engine.c; do
  echo "=== $f ==="
  git diff "$f" | grep -E '^[+-]' | grep -vE '^(---|\+\+\+)' | grep -vE '^[+-]\s*(//|#|/\*|\*)' | head -20
done
```
Expected: nếu output ~empty → diff toàn comment (loại). Nếu có code → giữ.

- [ ] **Step 4: Auto-decide theo rule + ghi decision**

Rule:
- Untracked `test_enumerate.c` (459 LOC) — KEEP (test file mới có giá trị)
- Untracked `PROPOSAL_MULTI_READER.md` — KEEP (Roadmap 1.1 per spec)
- Modified files: nếu diff Step 3 có code → KEEP as-is, không revert. Nếu diff toàn comment → sẽ bị strip ở Task 15-17 dù sao, KEEP.

Kết luận default: **KEEP TẤT CẢ**. Không revert gì.

Run:
```bash
cat > /tmp/shibadb-phase0-wip-decision.md <<'EOF'
# WIP decision — Phase 0

Rule: KEEP all modified + untracked files. `.git` cũ sẽ bị wipe, nhưng nội dung file preserve.

## Files kept
- `shibadb-c/CMakeLists.txt` (modified)
- `shibadb-c/include/shibadb_engine.h` (modified)
- `shibadb-c/src/engine.c` (modified)
- `shibadb-c/docs/PROPOSAL_MULTI_READER.md` (untracked)
- `shibadb-c/tests/test_enumerate.c` (untracked)

## Files reverted
(none)
EOF
cat /tmp/shibadb-phase0-wip-decision.md
```
Expected: hiển thị nội dung file decision.

---

### Task 3: Xóa toàn bộ Python legacy at root

**Bối cảnh amend 2026-07-28**: sau rescan, `~/project/shibadb/` là chính Python project root (không phải container thuần). Python artifacts rải rác 13 entry ở root cộng 3 file `docs/*.md`, phải wipe hết. Preserve `docs/superpowers/` (chứa spec + plan hiện đang execute).

**Files:**
- Delete: `~/project/shibadb/shibadb/` (Python package source)
- Delete: `~/project/shibadb/tests/` (17 test Python file)
- Delete: `~/project/shibadb/benchmarks/` (Python bench)
- Delete: `~/project/shibadb/conformance/` (Python conformance)
- Delete: `~/project/shibadb/spec/` (Python spec, sẽ được thay bởi shibadb-c docs)
- Delete: `~/project/shibadb/pyproject.toml`
- Delete: `~/project/shibadb/README.md` (Python — sẽ được thay bởi shibadb-c/README.md ở Task 19)
- Delete: `~/project/shibadb/CHANGELOG.md` (Python — sẽ được thay)
- Delete: `~/project/shibadb/SECURITY.md` (Python — sẽ được thay)
- Delete: `~/project/shibadb/.gitignore` (Python — sẽ được thay ở Task 20)
- Delete: `~/project/shibadb/.coverage`
- Delete: `~/project/shibadb/.hypothesis/`
- Delete: `~/project/shibadb/.dual-graph/`
- Delete: `~/project/shibadb/docs/architecture.md`
- Delete: `~/project/shibadb/docs/shell.md`
- Delete: `~/project/shibadb/docs/hvpdb-compatibility.md`
- **PRESERVE**: `~/project/shibadb/docs/superpowers/` (spec + Plan 1 file)

**Interfaces:**
- Consumes: Backup Task 1 (tarball ở /tmp còn nguyên).
- Produces: root chỉ còn `docs/superpowers/`, `refs/`, `shibadb-c/`. Task 19 sau đó move shibadb-c/* lên root sạch.

- [ ] **Step 1: Verify vị trí đúng — root có Python markers**

Run:
```bash
cd ~/project/shibadb
ls pyproject.toml tests/conftest.py 2>&1 | head -3
```
Expected: hai file hiển thị (không "No such file").

- [ ] **Step 2: Đo size trước**

Run:
```bash
cd ~/project/shibadb
du -sh shibadb/ tests/ benchmarks/ conformance/ spec/ .hypothesis/ .dual-graph/ 2>&1
```
Expected: mỗi mục vài KB-MB.

- [ ] **Step 3: Xác nhận `docs/superpowers/` tồn tại (phải preserve)**

Run:
```bash
ls -la ~/project/shibadb/docs/superpowers/specs/ ~/project/shibadb/docs/superpowers/plans/
```
Expected: hiển thị spec file `2026-07-28-shibadb-c-hardening-design.md` + plan `2026-07-28-phase-0-clean-slate.md`.

- [ ] **Step 4: Xóa các dir Python (không đụng docs/, refs/, shibadb-c/)**

Run:
```bash
cd ~/project/shibadb
rm -rf shibadb tests benchmarks conformance spec .hypothesis .dual-graph
```

- [ ] **Step 5: Xóa các file Python ở root**

Run:
```bash
cd ~/project/shibadb
rm -f pyproject.toml README.md CHANGELOG.md SECURITY.md .gitignore .coverage
```

- [ ] **Step 6: Xóa Python docs (preserve superpowers/)**

Run:
```bash
cd ~/project/shibadb
rm -f docs/architecture.md docs/shell.md docs/hvpdb-compatibility.md
```

- [ ] **Step 7: Verify final root state**

Run:
```bash
cd ~/project/shibadb
ls -la
```
Expected: chỉ còn `docs/` (chứa superpowers/), `refs/` (Task 4 sẽ xóa), `shibadb-c/`. Không còn Python file/dir.

- [ ] **Step 8: Verify docs/superpowers/ intact**

Run:
```bash
ls ~/project/shibadb/docs/superpowers/specs/ ~/project/shibadb/docs/superpowers/plans/
```
Expected: file spec + plan vẫn hiện diện.

---

### Task 4: Xóa `refs/`

**Files:**
- Delete: `~/project/shibadb/refs/`

**Interfaces:**
- Consumes: Backup Task 1.
- Produces: không còn historical reference.

- [ ] **Step 1: Verify**

Run: `ls -d ~/project/shibadb/refs/*`
Expected: hiển thị `rust-v0` và `python-v0-docs`.

- [ ] **Step 2: Xóa**

Run: `rm -rf ~/project/shibadb/refs/`

- [ ] **Step 3: Verify**

Run: `ls -la ~/project/shibadb/refs 2>&1 | head`
Expected: `No such file or directory`.

---

### Task 5: Xóa `shibadb-c/docs/README_legacy.md`

**Bối cảnh amend**: `hvpdb-compatibility.md` gốc ở root `docs/` (Python's), đã xóa ở Task 3. shibadb-c/docs KHÔNG chứa file này. Task này chỉ còn README_legacy.

**Files:**
- Delete: `~/project/shibadb/shibadb-c/docs/README_legacy.md`

**Interfaces:**
- Consumes: nothing.
- Produces: shibadb-c/docs không còn legacy orphan.

- [ ] **Step 1: Verify README_legacy.md tồn tại**

Run: `ls -la ~/project/shibadb/shibadb-c/docs/README_legacy.md`
Expected: hiển thị file.

- [ ] **Step 2: Xóa**

Run: `rm ~/project/shibadb/shibadb-c/docs/README_legacy.md`

- [ ] **Step 3: Verify**

Run: `ls ~/project/shibadb/shibadb-c/docs/ | grep -Ei '(legacy|hvpdb)'`
Expected: empty output.

---

### Task 6: Xóa 11 build dir trong shibadb-c

**Files:**
- Delete: `~/project/shibadb/shibadb-c/build*/`

**Interfaces:**
- Consumes: nothing.
- Produces: chỉ CMakePresets.json quyết định build tree.

- [ ] **Step 1: Liệt kê 11 build dir**

Run: `ls -d ~/project/shibadb/shibadb-c/build*/`
Expected: liệt kê `build/`, `build-analysis/`, `build-clang/`, `build-coverage/`, `build-fresh/`, `build-fuzz/`, `build-gcc/`, `build-release/`, `build-sanitize/`, `build-soak/`, `build-soak-test/`, `build-tsan/`.

- [ ] **Step 2: Đo tổng size**

Run: `du -sh ~/project/shibadb/shibadb-c/build*/`
Expected: mỗi dir vài chục-vài trăm MB, tổng ~1-3GB.

- [ ] **Step 3: Xóa hết**

Run: `rm -rf ~/project/shibadb/shibadb-c/build*/`

- [ ] **Step 4: Verify**

Run: `ls -d ~/project/shibadb/shibadb-c/build* 2>&1`
Expected: `No such file or directory`.

---

### Task 7: Gộp `AUDIT.md` + `AUDIT_2026_07.md` → 1 file

**Files:**
- Modify: `~/project/shibadb/shibadb-c/docs/AUDIT.md`
- Delete: `~/project/shibadb/shibadb-c/docs/AUDIT_2026_07.md`

**Interfaces:**
- Consumes: 2 file audit hiện có (~117KB mỗi file, gần identical).
- Produces: 1 file `AUDIT.md` là live doc; note ngày cuối cập nhật ở đầu file.

- [ ] **Step 1: So sánh 2 file để chọn cái nào là "canonical"**

Run:
```bash
cd ~/project/shibadb/shibadb-c/docs
diff AUDIT.md AUDIT_2026_07.md | head -50
wc -l AUDIT.md AUDIT_2026_07.md
```
Expected: diff hiển thị vài dòng khác (chủ yếu timestamp/heading); wc show ~vài nghìn dòng mỗi file.

- [ ] **Step 2: Chọn AUDIT_2026_07.md làm base (mới hơn), copy đè AUDIT.md**

Run:
```bash
cd ~/project/shibadb/shibadb-c/docs
cp AUDIT_2026_07.md AUDIT.md
```

- [ ] **Step 3: Sửa heading H1 cho stable (không có ngày)**

Run:
```bash
cd ~/project/shibadb/shibadb-c/docs
sed -i '1s/.*/# Repository audit/' AUDIT.md
head -1 AUDIT.md
```
Expected: `# Repository audit`.

- [ ] **Step 4: Thêm note "Last updated" thay vì ngày trong tiêu đề**

Run:
```bash
cd ~/project/shibadb/shibadb-c/docs
python3 -c "
import pathlib
p = pathlib.Path('AUDIT.md')
lines = p.read_text().splitlines()
new = [lines[0], '', '_Last updated: 2026-07-28 (Phase 0 cleanup)_', ''] + lines[1:]
p.write_text('\n'.join(new) + '\n')
"
head -5 AUDIT.md
```
Expected: heading + "_Last updated: 2026-07-28 (Phase 0 cleanup)_" + rest.

- [ ] **Step 5: Xóa AUDIT_2026_07.md**

Run: `rm ~/project/shibadb/shibadb-c/docs/AUDIT_2026_07.md`

- [ ] **Step 6: Verify không còn `*_YYYY_MM_*.md` trong docs**

Run: `ls ~/project/shibadb/shibadb-c/docs/ | grep -E '_20[0-9]{2}_[0-9]{2}' || echo "clean"`
Expected: `COVERAGE_2026_07.md` và `SECURITY_REVIEW_2026_07_25.md` vẫn còn (Task 8, 9 sẽ xử). Chưa `clean`.

---

### Task 8: Gộp `COVERAGE.md` + `COVERAGE_2026_07.md`

**Files:**
- Modify: `~/project/shibadb/shibadb-c/docs/COVERAGE.md`
- Delete: `~/project/shibadb/shibadb-c/docs/COVERAGE_2026_07.md`

**Interfaces:**
- Consumes: 2 file coverage.
- Produces: 1 file live COVERAGE.md.

- [ ] **Step 1: Copy _2026_07 đè lên COVERAGE.md**

Run:
```bash
cd ~/project/shibadb/shibadb-c/docs
cp COVERAGE_2026_07.md COVERAGE.md
```

- [ ] **Step 2: Sửa heading H1 stable**

Run:
```bash
cd ~/project/shibadb/shibadb-c/docs
sed -i '1s/.*/# Coverage snapshot/' COVERAGE.md
head -1 COVERAGE.md
```
Expected: `# Coverage snapshot`.

- [ ] **Step 3: Thêm "Last updated" note**

Run:
```bash
cd ~/project/shibadb/shibadb-c/docs
python3 -c "
import pathlib
p = pathlib.Path('COVERAGE.md')
lines = p.read_text().splitlines()
new = [lines[0], '', '_Last updated: 2026-07-28 (Phase 0 cleanup)_', ''] + lines[1:]
p.write_text('\n'.join(new) + '\n')
"
```

- [ ] **Step 4: Xóa `COVERAGE_2026_07.md`**

Run: `rm ~/project/shibadb/shibadb-c/docs/COVERAGE_2026_07.md`

---

### Task 9: Gộp `SECURITY_REVIEW.md` + `SECURITY_REVIEW_2026_07_25.md`

**Files:**
- Modify: `~/project/shibadb/shibadb-c/docs/SECURITY_REVIEW.md`
- Delete: `~/project/shibadb/shibadb-c/docs/SECURITY_REVIEW_2026_07_25.md`

**Interfaces:**
- Consumes: 2 file.
- Produces: 1 file live SECURITY_REVIEW.md.

- [ ] **Step 1: Copy _2026_07_25 đè**

Run:
```bash
cd ~/project/shibadb/shibadb-c/docs
cp SECURITY_REVIEW_2026_07_25.md SECURITY_REVIEW.md
```

- [ ] **Step 2: Sửa heading stable**

Run:
```bash
cd ~/project/shibadb/shibadb-c/docs
sed -i '1s/.*/# Security review/' SECURITY_REVIEW.md
```

- [ ] **Step 3: Thêm "Last updated"**

Run:
```bash
cd ~/project/shibadb/shibadb-c/docs
python3 -c "
import pathlib
p = pathlib.Path('SECURITY_REVIEW.md')
lines = p.read_text().splitlines()
new = [lines[0], '', '_Last updated: 2026-07-28 (Phase 0 cleanup)_', ''] + lines[1:]
p.write_text('\n'.join(new) + '\n')
"
```

- [ ] **Step 4: Xóa `SECURITY_REVIEW_2026_07_25.md`**

Run: `rm ~/project/shibadb/shibadb-c/docs/SECURITY_REVIEW_2026_07_25.md`

- [ ] **Step 5: Verify tất cả duplicate đã dọn**

Run: `ls ~/project/shibadb/shibadb-c/docs/ | grep -E '_20[0-9]{2}_[0-9]{2}' || echo "clean"`
Expected: `clean`.

---

### Task 10: Viết script `strip_c_comments.py`

**Files:**
- Create: `~/project/shibadb/scripts/strip_c_comments.py`

**Interfaces:**
- Consumes: `.c` và `.h` file path.
- Produces: file với 0 comment, giữ nguyên tất cả preprocessor directive và code. Task 14 gọi script này.

- [ ] **Step 1: Viết script**

Content:
```python
#!/usr/bin/env python3
import sys
import re
from pathlib import Path

STRING_OR_COMMENT = re.compile(
    r'''
    ( "(?:[^"\\]|\\.)*"          # double-quoted string
    | '(?:[^'\\]|\\.)*'          # single-quoted char
    | //[^\n]*                    # line comment
    | /\*[\s\S]*?\*/              # block comment
    )
    ''',
    re.VERBOSE,
)

def strip(src: str) -> str:
    def repl(m: re.Match) -> str:
        tok = m.group(1)
        if tok.startswith('//') or tok.startswith('/*'):
            return ' ' if tok.startswith('//') else ' '
        return tok
    out = STRING_OR_COMMENT.sub(repl, src)
    lines = [line.rstrip() for line in out.splitlines()]
    result_lines = []
    prev_blank = False
    for line in lines:
        is_blank = not line.strip()
        if is_blank and prev_blank:
            continue
        result_lines.append(line)
        prev_blank = is_blank
    return '\n'.join(result_lines).rstrip() + '\n'

def main():
    if len(sys.argv) < 2:
        print("usage: strip_c_comments.py FILE [FILE ...]", file=sys.stderr)
        sys.exit(2)
    for arg in sys.argv[1:]:
        p = Path(arg)
        original = p.read_text(encoding='utf-8')
        stripped = strip(original)
        p.write_text(stripped, encoding='utf-8')

if __name__ == '__main__':
    main()
```

Write:
```bash
mkdir -p ~/project/shibadb/scripts
cat > ~/project/shibadb/scripts/strip_c_comments.py <<'PYEOF'
#!/usr/bin/env python3
import sys
import re
from pathlib import Path

STRING_OR_COMMENT = re.compile(
    r'''
    ( "(?:[^"\\]|\\.)*"
    | '(?:[^'\\]|\\.)*'
    | //[^\n]*
    | /\*[\s\S]*?\*/
    )
    ''',
    re.VERBOSE,
)

def strip(src: str) -> str:
    def repl(m):
        tok = m.group(1)
        if tok.startswith('//') or tok.startswith('/*'):
            return ' '
        return tok
    out = STRING_OR_COMMENT.sub(repl, src)
    lines = [line.rstrip() for line in out.splitlines()]
    result = []
    prev_blank = False
    for line in lines:
        is_blank = not line.strip()
        if is_blank and prev_blank:
            continue
        result.append(line)
        prev_blank = is_blank
    return '\n'.join(result).rstrip() + '\n'

def main():
    if len(sys.argv) < 2:
        print("usage: strip_c_comments.py FILE [FILE ...]", file=sys.stderr)
        sys.exit(2)
    for arg in sys.argv[1:]:
        p = Path(arg)
        p.write_text(strip(p.read_text(encoding='utf-8')), encoding='utf-8')

if __name__ == '__main__':
    main()
PYEOF
chmod +x ~/project/shibadb/scripts/strip_c_comments.py
```

- [ ] **Step 2: Test script trên 1 file mẫu**

Run:
```bash
cat > /tmp/test_strip.c <<'EOF'
// This is a line comment
#include <stdio.h>
/* Block comment
   multi-line */
int main(void) {
    const char *s = "// not a comment";
    const char *t = "/* also not */";
    return 0; // trailing
}
EOF
python3 ~/project/shibadb/scripts/strip_c_comments.py /tmp/test_strip.c
cat /tmp/test_strip.c
```
Expected: output không có `//` hay `/* */` bên ngoài string literal; string `"// not a comment"` và `"/* also not */"` PRESERVED; `#include` giữ nguyên.

- [ ] **Step 3: Cleanup test file**

Run: `rm /tmp/test_strip.c`

---

### Task 11: Viết script `strip_py_comments.py`

**Files:**
- Create: `~/project/shibadb/scripts/strip_py_comments.py`

**Interfaces:**
- Consumes: `.py` file path.
- Produces: file không có `#` comment (giữ shebang) và không có top-level docstring. Task 15 gọi.

- [ ] **Step 1: Viết script dùng `tokenize` để xử lý an toàn**

Run:
```bash
cat > ~/project/shibadb/scripts/strip_py_comments.py <<'PYEOF'
#!/usr/bin/env python3
import io
import sys
import tokenize
from pathlib import Path

def strip(src: str) -> str:
    result = []
    prev_end = (1, 0)
    last_line = 1
    tokens = list(tokenize.generate_tokens(io.StringIO(src).readline))
    for tok in tokens:
        tok_type, tok_str, start, end, _ = tok
        if tok_type == tokenize.COMMENT:
            if tok_str.startswith('#!') and start[0] == 1:
                pass
            else:
                continue
        if tok_type == tokenize.NL and prev_end[1] == 0 and result and result[-1].endswith('\n'):
            continue
        if start[0] > prev_end[0]:
            result.append('\n' * (start[0] - prev_end[0]))
            result.append(' ' * start[1])
        elif start[1] > prev_end[1]:
            result.append(' ' * (start[1] - prev_end[1]))
        result.append(tok_str)
        prev_end = end
    out = ''.join(result)
    lines = [line.rstrip() for line in out.splitlines()]
    cleaned = []
    prev_blank = False
    for line in lines:
        is_blank = not line.strip()
        if is_blank and prev_blank:
            continue
        cleaned.append(line)
        prev_blank = is_blank
    return '\n'.join(cleaned).rstrip() + '\n'

def strip_docstrings_ast(src: str) -> str:
    import ast
    tree = ast.parse(src)
    lines_to_remove = set()
    def visit(node):
        if isinstance(node, (ast.Module, ast.FunctionDef, ast.AsyncFunctionDef, ast.ClassDef)):
            body = node.body
            if body and isinstance(body[0], ast.Expr) and isinstance(body[0].value, ast.Constant) and isinstance(body[0].value.value, str):
                for ln in range(body[0].lineno, body[0].end_lineno + 1):
                    lines_to_remove.add(ln)
        for child in ast.iter_child_nodes(node):
            visit(child)
    visit(tree)
    if not lines_to_remove:
        return src
    src_lines = src.splitlines()
    kept = [ln for i, ln in enumerate(src_lines, 1) if i not in lines_to_remove]
    return '\n'.join(kept).rstrip() + '\n'

def main():
    if len(sys.argv) < 2:
        print("usage: strip_py_comments.py FILE [FILE ...]", file=sys.stderr)
        sys.exit(2)
    for arg in sys.argv[1:]:
        p = Path(arg)
        src = p.read_text(encoding='utf-8')
        src = strip_docstrings_ast(src)
        src = strip(src)
        p.write_text(src, encoding='utf-8')

if __name__ == '__main__':
    main()
PYEOF
chmod +x ~/project/shibadb/scripts/strip_py_comments.py
```

- [ ] **Step 2: Test trên 1 file mẫu**

Run:
```bash
cat > /tmp/test_strip.py <<'EOF'
#!/usr/bin/env python3
"""Module docstring."""
# top comment
import sys

class Foo:
    """Class docstring."""
    def bar(self):
        """Function docstring."""
        # inline
        return sys.argv  # trailing
EOF
python3 ~/project/shibadb/scripts/strip_py_comments.py /tmp/test_strip.py
cat /tmp/test_strip.py
```
Expected: shebang giữ; không docstring; không `#` comment. `import`, `class`, `def`, `return` intact.

- [ ] **Step 3: Cleanup**

Run: `rm /tmp/test_strip.py`

---

### Task 12: Dry-run strip C files — verify build tree không đổi behavior

**Files:**
- Modify: (temporary copy trong /tmp)

**Interfaces:**
- Consumes: script Task 10.
- Produces: bằng chứng build+test còn work sau strip. Task 14 sẽ apply thật.

- [ ] **Step 1: Copy shibadb-c source ra /tmp để dry-run**

Run:
```bash
rm -rf /tmp/shibadb-dryrun
cp -r ~/project/shibadb/shibadb-c /tmp/shibadb-dryrun
```

- [ ] **Step 2: Apply strip lên tất cả .c/.h trong dryrun**

Run:
```bash
cd /tmp/shibadb-dryrun
find src include tests -type f \( -name '*.c' -o -name '*.h' \) -print0 \
  | xargs -0 python3 ~/project/shibadb/scripts/strip_c_comments.py
```
Expected: chạy silent, không error.

- [ ] **Step 3: Verify 0 comment còn trong `.c`/`.h`**

Run:
```bash
cd /tmp/shibadb-dryrun
grep -rnE '^\s*(//|/\*)' --include='*.c' --include='*.h' src include tests | head -20 || echo "no comments"
```
Expected: `no comments`.

- [ ] **Step 4: Build dryrun copy với preset gcc**

Run:
```bash
cd /tmp/shibadb-dryrun
cmake --preset gcc 2>&1 | tail -20
cmake --build --preset gcc 2>&1 | tail -20
```
Expected: cmake configure OK, build OK, không error.

- [ ] **Step 5: Chạy full test suite trên dryrun**

Run:
```bash
cd /tmp/shibadb-dryrun
ctest --preset gcc --output-on-failure 2>&1 | tail -30
```
Expected: tất cả test PASS (số test tùy build).

- [ ] **Step 6: Cleanup dryrun copy**

Run: `rm -rf /tmp/shibadb-dryrun`

---

### Task 13: Dry-run strip Python files (nếu có Python nào ở shibadb-c/tests)

**Files:**
- Modify: (temporary copy trong /tmp)

**Interfaces:**
- Consumes: script Task 11.
- Produces: bằng chứng script không crash trên các file Python trong shibadb-c.

- [ ] **Step 1: List các Python file trong shibadb-c**

Run: `find ~/project/shibadb/shibadb-c -type f -name '*.py'`
Expected: 3 file — `tests/test_python_binding.py`, `tests/test_release_evidence.py`, `tests/test_security_bundle.py`, cộng bất kỳ script.

- [ ] **Step 2: Copy ra /tmp**

Run:
```bash
mkdir -p /tmp/shibadb-py-dryrun
cp ~/project/shibadb/shibadb-c/tests/*.py /tmp/shibadb-py-dryrun/
```

- [ ] **Step 3: Apply strip**

Run: `python3 ~/project/shibadb/scripts/strip_py_comments.py /tmp/shibadb-py-dryrun/*.py`
Expected: silent, không error.

- [ ] **Step 4: Verify file vẫn parse được như Python**

Run:
```bash
for f in /tmp/shibadb-py-dryrun/*.py; do
  python3 -c "import ast; ast.parse(open('$f').read())" || echo "PARSE FAIL: $f"
done
```
Expected: không output (parse OK cho tất cả).

- [ ] **Step 5: Verify 0 comment**

Run: `grep -nE '^\s*#' /tmp/shibadb-py-dryrun/*.py | grep -v '#!/' | head`
Expected: empty (chỉ shebang được giữ).

- [ ] **Step 6: Cleanup**

Run: `rm -rf /tmp/shibadb-py-dryrun`

---

### Task 14: Apply strip C thật cho toàn bộ `.c`/`.h`

**Files:**
- Modify: mọi `.c` và `.h` trong `~/project/shibadb/shibadb-c/{src,include,tests,benchmarks}` (benchmarks có thể chưa có, ignore silently).

**Interfaces:**
- Consumes: script Task 10, verified Task 12.
- Produces: 0 comment trong C source. Task 16 verify build.

- [ ] **Step 1: Apply strip**

Run:
```bash
find ~/project/shibadb/shibadb-c \
  \( -path '*/build*' -prune \) -o \
  -type f \( -name '*.c' -o -name '*.h' \) -print0 \
  | xargs -0 python3 ~/project/shibadb/scripts/strip_c_comments.py
```
Expected: silent.

- [ ] **Step 2: Verify 0 comment**

Run:
```bash
grep -rnE '^\s*(//|/\*)' --include='*.c' --include='*.h' \
  ~/project/shibadb/shibadb-c/src \
  ~/project/shibadb/shibadb-c/include \
  ~/project/shibadb/shibadb-c/tests \
  | head -20 || echo "no comments"
```
Expected: `no comments`.

- [ ] **Step 3: Verify không có license header sót**

Run:
```bash
grep -rniE '(copyright|license|spdx-license)' \
  --include='*.c' --include='*.h' \
  ~/project/shibadb/shibadb-c/src \
  ~/project/shibadb/shibadb-c/include \
  ~/project/shibadb/shibadb-c/tests \
  | head -20 || echo "no headers"
```
Expected: `no headers` (LICENSE ở root là separate file, không phải trong .c/.h).

---

### Task 15: Apply strip Python thật cho `tests/*.py`

**Files:**
- Modify: mọi `.py` trong `~/project/shibadb/shibadb-c/tests`.

**Interfaces:**
- Consumes: script Task 11, verified Task 13.
- Produces: 0 comment/docstring trong Python.

- [ ] **Step 1: Apply**

Run:
```bash
find ~/project/shibadb/shibadb-c -type f -name '*.py' -print0 \
  | xargs -0 python3 ~/project/shibadb/scripts/strip_py_comments.py
```

- [ ] **Step 2: Verify parse OK**

Run:
```bash
for f in $(find ~/project/shibadb/shibadb-c -type f -name '*.py'); do
  python3 -c "import ast; ast.parse(open('$f').read())" || echo "FAIL: $f"
done
```
Expected: không output.

- [ ] **Step 3: Verify 0 comment (chỉ shebang giữ)**

Run:
```bash
grep -rnE '^\s*#' --include='*.py' ~/project/shibadb/shibadb-c \
  | grep -v '#!/' | head
```
Expected: empty.

---

### Task 16: Verify build còn compile sau strip

**Files:**
- (build tree tạm trong `~/project/shibadb/shibadb-c/build/gcc`)

**Interfaces:**
- Consumes: source đã strip.
- Produces: bằng chứng build không break.

- [ ] **Step 1: Configure release preset**

Run:
```bash
cd ~/project/shibadb/shibadb-c
cmake --preset gcc 2>&1 | tail -15
```
Expected: `-- Generating done` cuối output.

- [ ] **Step 2: Build**

Run:
```bash
cd ~/project/shibadb/shibadb-c
cmake --build --preset gcc 2>&1 | tail -20
```
Expected: `[100%] Built target ...` cuối output; không warning về strip artifact.

---

### Task 17: Verify test suite còn pass

**Files:**
- (không thay đổi)

**Interfaces:**
- Consumes: build từ Task 16.
- Produces: pass count → METRICS.md ở Task 25.

- [ ] **Step 1: Chạy ctest**

Run:
```bash
cd ~/project/shibadb/shibadb-c
ctest --preset gcc --output-on-failure 2>&1 | tee /tmp/shibadb-phase0-ctest.log | tail -30
```
Expected: tất cả test pass. Đọc dòng cuối "N tests passed, 0 failed out of N".

- [ ] **Step 2: Ghi test count**

Run:
```bash
grep -oE '[0-9]+ tests? passed' /tmp/shibadb-phase0-ctest.log | tail -1
```
Expected: một dòng như "44 tests passed" (số cụ thể tùy config).

---

### Task 18: Xóa `.git` cũ của shibadb-c

**Files:**
- Delete: `~/project/shibadb/shibadb-c/.git/`

**Interfaces:**
- Consumes: nothing (Task 17 đã verify không cần history).
- Produces: shibadb-c thành plain directory, sẵn move lên root.

- [ ] **Step 1: Verify .git exists**

Run: `ls -la ~/project/shibadb/shibadb-c/.git/HEAD`
Expected: hiển thị file.

- [ ] **Step 2: Xóa**

Run: `rm -rf ~/project/shibadb/shibadb-c/.git`

- [ ] **Step 3: Verify**

Run: `ls -la ~/project/shibadb/shibadb-c/.git 2>&1`
Expected: `No such file or directory`.

---

### Task 19: Move `shibadb-c/*` lên `~/project/shibadb/` root

**Files:**
- Move: mọi entry trong `~/project/shibadb/shibadb-c/` (kể cả dotfile ngoài `.git` đã xóa) lên `~/project/shibadb/`.
- Delete: `~/project/shibadb/shibadb-c/` empty dir.

**Interfaces:**
- Consumes: Task 18.
- Produces: root layout final theo File Structure ở đầu plan.

- [ ] **Step 1: Verify không có collision (không có file trùng tên giữa root và shibadb-c/)**

Run:
```bash
cd ~/project/shibadb
comm -12 <(ls -A | sort) <(ls -A shibadb-c/ | sort)
```
Expected: chỉ hiển thị `docs` (root có `docs/superpowers/`, shibadb-c có `docs/AUDIT.md` etc.) và có thể `README.md`. Cần merge `docs/`, cẩn thận về README.

- [ ] **Step 2: Check root có `README.md` hoặc `docs/` không**

Run:
```bash
ls -la ~/project/shibadb/README.md 2>&1
ls -la ~/project/shibadb/docs/ 2>&1
```
Expected: 
- Nếu chỉ `docs/superpowers/` tồn tại ở root (từ Task viết spec/plan trước), thì merge dễ.
- README.md ở root nếu có (user tự tạo) — quyết định giữ hoặc dùng shibadb-c/README.md.

- [ ] **Step 3: Move top-level entry (loại `docs`, `scripts`) — dùng find để tránh word-splitting**

Run:
```bash
cd ~/project/shibadb/shibadb-c
find . -mindepth 1 -maxdepth 1 \
  ! -name docs ! -name scripts \
  -exec mv -t ~/project/shibadb/ {} +
```
Expected: mọi entry (include/, src/, tests/, benchmarks/ nếu có, CMakeLists.txt, CMakePresets.json, README.md, .gitignore nếu tồn tại, etc.) chuyển lên root.

- [ ] **Step 4: Merge `docs` — move mọi entry trong `shibadb-c/docs/` vào `~/project/shibadb/docs/`**

Run:
```bash
mkdir -p ~/project/shibadb/docs
find ~/project/shibadb/shibadb-c/docs -mindepth 1 -maxdepth 1 \
  -exec mv -t ~/project/shibadb/docs/ {} +
ls ~/project/shibadb/docs/
```
Expected: hiển thị `superpowers/` (đã có), cộng ~21 doc file từ shibadb-c/docs.

- [ ] **Step 5: Merge `scripts` — move mọi entry trong `shibadb-c/scripts/` vào `~/project/shibadb/scripts/`** (Task 10/11 đã tạo `strip_*.py` ở đây)

Run:
```bash
mkdir -p ~/project/shibadb/scripts
find ~/project/shibadb/shibadb-c/scripts -mindepth 1 -maxdepth 1 \
  -exec mv -t ~/project/shibadb/scripts/ {} +
ls ~/project/shibadb/scripts/
```
Expected: hiển thị `strip_c_comments.py`, `strip_py_comments.py` (từ Task 10/11) + `check_reproducible.py`, `package_release.py`, `release_audit.py`, `release_evidence.py`, `verify_security_bundle.py` (từ shibadb-c).

- [ ] **Step 6: Xóa dir rỗng**

Run:
```bash
rmdir ~/project/shibadb/shibadb-c/docs
rmdir ~/project/shibadb/shibadb-c/scripts
rmdir ~/project/shibadb/shibadb-c
```
Expected: cả 3 dir xóa OK (chỉ xóa khi empty).

- [ ] **Step 7: Verify final layout**

Run:
```bash
cd ~/project/shibadb
ls -la
```
Expected: có `include/`, `src/`, `tests/`, `docs/`, `scripts/`, `CMakeLists.txt`, `CMakePresets.json`, `README.md`, `LICENSE`, `.gitignore` (nếu shibadb-c cũ có), etc. Không có `shibadb-c/`, `shibadb/`, `refs/`.

---

### Task 20: Tạo `.gitignore` chuẩn C

**Files:**
- Create/overwrite: `~/project/shibadb/.gitignore`

**Interfaces:**
- Consumes: nothing.
- Produces: baseline ignore rules cho C project + coverage/sanitizer artifacts.

- [ ] **Step 1: Write .gitignore**

Run:
```bash
cat > ~/project/shibadb/.gitignore <<'EOF'
build*/
*.o
*.a
*.so
*.dylib
*.dll
*.gcda
*.gcno
*.profraw
*.profdata
.cache/
compile_commands.json
default.profraw
crashes-fuzz/
corpus-fuzz/
.hypothesis/
__pycache__/
*.pyc
.pytest_cache/
.coverage
htmlcov/
EOF
cat ~/project/shibadb/.gitignore
```

---

### Task 21: Init fresh git repo, config sole author

**Files:**
- Create: `~/project/shibadb/.git/`

**Interfaces:**
- Consumes: layout Task 19, gitignore Task 20.
- Produces: git repo sẵn commit đầu.

- [ ] **Step 1: `git init`**

Run:
```bash
cd ~/project/shibadb
git init -b main
```
Expected: "Initialized empty Git repository in /home/satoharuki/project/shibadb/.git/".

- [ ] **Step 2: Config author cho repo này (không đụng global)**

Run:
```bash
cd ~/project/shibadb
git config user.name "satoharuki"
git config user.email "$(git config --global user.email || echo 'satoharuki@localhost')"
git config user.name
git config user.email
```
Expected: hiển thị tên + email.

- [ ] **Step 3: Verify không có untracked debris ngoài mong đợi**

Run:
```bash
cd ~/project/shibadb
git status --short | head -20
```
Expected: tất cả file untracked (repo mới), không có build dir (nhờ .gitignore).

---

### Task 22: Verify build vẫn compile trong repo mới

**Files:**
- (build tree ở `build/gcc/`, ignored bởi .gitignore)

**Interfaces:**
- Consumes: repo Task 21.
- Produces: sanity check final trước initial commit.

- [ ] **Step 1: Configure**

Run:
```bash
cd ~/project/shibadb
cmake --preset gcc 2>&1 | tail -10
```
Expected: configure OK.

- [ ] **Step 2: Build**

Run:
```bash
cd ~/project/shibadb
cmake --build --preset gcc 2>&1 | tail -10
```
Expected: `[100%] Built target ...`.

- [ ] **Step 3: Test**

Run:
```bash
cd ~/project/shibadb
ctest --preset gcc --output-on-failure 2>&1 | tail -15
```
Expected: tất cả test pass, count khớp với Task 17.

---

### Task 23: Ghi `docs/METRICS.md` baseline

**Files:**
- Create: `~/project/shibadb/docs/METRICS.md`

**Interfaces:**
- Consumes: kết quả build/test Task 22.
- Produces: baseline metrics cho Phase 1 tracking.

- [ ] **Step 1: Đo tất cả metric**

Run:
```bash
cd ~/project/shibadb

PROD_LOC=$(find src include -type f \( -name '*.c' -o -name '*.h' \) -exec cat {} + | wc -l)
TEST_LOC=$(find tests -type f -name '*.c' -exec cat {} + | wc -l)
RATIO=$(python3 -c "print(f'{${TEST_LOC} / ${PROD_LOC} * 100:.1f}%')")
DUP_DOC=$(ls docs/ | grep -cE '_20[0-9]{2}_[0-9]{2}' || echo 0)
COMMENT_C=$(grep -rEc '^\s*(//|/\*)' --include='*.c' --include='*.h' src include tests | awk -F: '{sum+=$2} END {print sum+0}')
COMMENT_PY=$(grep -rEc '^\s*#' --include='*.py' tests | grep -v '#!/' | awk -F: '{sum+=$2} END {print sum+0}')

echo "PROD_LOC=$PROD_LOC TEST_LOC=$TEST_LOC RATIO=$RATIO DUP_DOC=$DUP_DOC COMMENT_C=$COMMENT_C COMMENT_PY=$COMMENT_PY"
```

- [ ] **Step 2: Write METRICS.md**

Run:
```bash
cd ~/project/shibadb
cat > docs/METRICS.md <<EOF
# Metrics — Phase 0 baseline

_Snapshot: 2026-07-28 sau Phase 0 clean slate._

## Health signals

| Metric | Baseline | Target 1.0 | Hvpdb red line |
|---|---:|---:|---:|
| Production LOC (\`src/\` + \`include/\`) | $PROD_LOC | — | — |
| Test LOC (\`tests/\`) | $TEST_LOC | — | — |
| Test / Production ratio | $RATIO | ≥30% | 3% |
| Duplicate doc files (\`*_YYYY_MM_*.md\`) | $DUP_DOC | 0 | 16 |
| Comment lines in \`*.c\`/\`*.h\` | $COMMENT_C | 0 | — |
| Comment lines in \`*.py\` | $COMMENT_PY | 0 | — |

## Anti-pattern checks (Phase 0 gates)

- [x] Xóa Python legacy \`shibadb/\`
- [x] Xóa \`refs/rust-v0\`, \`refs/python-v0-docs\`
- [x] Xóa \`docs/hvpdb-compatibility.md\`
- [x] Xóa \`docs/README_legacy.md\`
- [x] Gộp AUDIT/COVERAGE/SECURITY_REVIEW duplicates
- [x] Xóa 11 build dir
- [x] Xóa toàn bộ comment trong \`.c\`/\`.h\`/\`.py\`
- [x] Fresh git repo, sole author = satoharuki
- [x] Move \`shibadb-c/*\` lên root

## Build health

- CMake preset \`release\`: OK
- ctest: PASS (see /tmp/shibadb-phase0-ctest.log for count)

## Next

Phase 1 hardening — 4 pillar parallel + CI infra. See \`docs/superpowers/plans/2026-07-28-phase-1-4pillar.md\` (to be written).
EOF
cat docs/METRICS.md
```

---

### Task 24: Initial commit

**Files:**
- Commit: mọi file tracked (theo .gitignore).

**Interfaces:**
- Consumes: mọi task trên.
- Produces: commit `main:HEAD` là baseline `shibadb 1.0.0 phase-0-baseline`.

- [ ] **Step 1: Stage tất cả**

Run:
```bash
cd ~/project/shibadb
git add -A
git status --short | head -20
```
Expected: tất cả file đều "A " (added).

- [ ] **Step 2: Commit**

Run:
```bash
cd ~/project/shibadb
git commit -m "shibadb 1.0.0 baseline

Fresh sole-authored repo sau Phase 0 cleanup:
- Xoá Python legacy, refs/, 11 build dir, 2 doc scope-creep
- Gộp AUDIT/COVERAGE/SECURITY_REVIEW duplicate
- Strip toàn bộ comment/license header trong .c/.h/.py
- Move shibadb-c/* lên root
- Init fresh git repo

Baseline cho Phase 1 hardening — 4 pillar (durability, concurrency,
format, perf) + CI infra."
```
Expected: commit hash + summary "N files changed".

- [ ] **Step 3: Verify log**

Run:
```bash
cd ~/project/shibadb
git log --oneline
```
Expected: 1 dòng, hash + "shibadb 1.0.0 phase-0-baseline".

---

### Task 25: Final verification gate

**Files:**
- (không thay đổi — chỉ check)

**Interfaces:**
- Consumes: repo Task 24.
- Produces: bằng chứng Phase 0 hoàn tất, ready cho Phase 1.

- [ ] **Step 1: Verify 0 comment C**

Run:
```bash
cd ~/project/shibadb
COUNT=$(grep -rEc '^\s*(//|/\*)' --include='*.c' --include='*.h' src include tests | awk -F: '{sum+=$2} END {print sum+0}')
echo "C comment count: $COUNT (target: 0)"
[ "$COUNT" -eq 0 ] || echo "FAIL"
```
Expected: `C comment count: 0 (target: 0)` and không có `FAIL`.

- [ ] **Step 2: Verify 0 comment Python (ngoài shebang)**

Run:
```bash
cd ~/project/shibadb
COUNT=$(grep -rnE '^\s*#' --include='*.py' tests | grep -v '#!/' | wc -l)
echo "Python comment count: $COUNT (target: 0)"
[ "$COUNT" -eq 0 ] || echo "FAIL"
```
Expected: `Python comment count: 0 (target: 0)`.

- [ ] **Step 3: Verify 0 duplicate doc**

Run:
```bash
cd ~/project/shibadb
COUNT=$(ls docs/ | grep -cE '_20[0-9]{2}_[0-9]{2}' || echo 0)
echo "Duplicate doc count: $COUNT (target: 0)"
[ "$COUNT" -eq 0 ] || echo "FAIL"
```
Expected: `Duplicate doc count: 0`.

- [ ] **Step 4: Verify 0 legacy path**

Run:
```bash
cd ~/project/shibadb
for p in shibadb shibadb-c refs; do
  if [ -e "$p" ]; then echo "FAIL: $p còn tồn tại"; fi
done
echo "legacy paths check done"
```
Expected: chỉ `legacy paths check done`.

- [ ] **Step 5: Verify 0 build dir tracked**

Run:
```bash
cd ~/project/shibadb
git ls-files | grep -E '^build' | head || echo "no build dir tracked"
```
Expected: `no build dir tracked`.

- [ ] **Step 6: Verify test suite còn pass trong final state**

Run:
```bash
cd ~/project/shibadb
rm -rf build/
cmake --preset gcc 2>&1 | tail -5
cmake --build --preset gcc 2>&1 | tail -5
ctest --preset gcc --output-on-failure 2>&1 | tail -10
```
Expected: build OK, ctest pass.

- [ ] **Step 7: Verify sole author**

Run:
```bash
cd ~/project/shibadb
git log --format='%an <%ae>' | sort -u
```
Expected: 1 dòng — `satoharuki <email>`.

---

### Task 26: Cleanup temp files + note completion

**Files:**
- Delete: `/tmp/shibadb-phase0-*` (tarball backup + logs)

**Interfaces:**
- Consumes: Task 25 verify OK.
- Produces: cleanup, sẵn sàng ghi memory + move sang Phase 1 plan.

- [ ] **Step 1: Verify backup còn tồn tại ít nhất 24h nữa (hoặc archive nếu user muốn)**

Rule: giữ tarball backup trong `/tmp` — sẽ auto-clean bởi OS. Nếu user muốn archive lâu dài, move ra `~/backups/`. Default: để `/tmp` xóa tự động.

Run: `ls -lh /tmp/shibadb-phase0-backup.txt /tmp/shibadb-pre-phase0-*.tar.zst`
Expected: cả 2 file tồn tại. Không xóa manual.

- [ ] **Step 2: Note completion**

Run:
```bash
echo "Phase 0 complete: $(date -u +%Y-%m-%dT%H:%M:%SZ)" >> ~/project/shibadb/docs/METRICS.md
tail -3 ~/project/shibadb/docs/METRICS.md
```

- [ ] **Step 3: Amend commit initial để include METRICS update**

Run:
```bash
cd ~/project/shibadb
git add docs/METRICS.md
git commit --amend --no-edit
git log --oneline
```
Expected: vẫn 1 commit, hash mới (do amend).

---

## Post-Plan Checklist (agent-facing)

Sau khi tất cả 26 task done, verify manual:

- [ ] `~/project/shibadb/` là git repo, 1 commit
- [ ] Không có `shibadb/`, `shibadb-c/`, `refs/`
- [ ] `docs/` không có file `*_YYYY_MM_*.md`
- [ ] `src/`, `include/`, `tests/` không có comment trong `.c`/`.h`/`.py`
- [ ] `docs/METRICS.md` đầy đủ
- [ ] `LICENSE` file ở root vẫn tồn tại
- [ ] `cmake --preset gcc && cmake --build --preset gcc && ctest --preset gcc` pass

**Sau khi Phase 0 xong → chuyển sang writing-plans cho Phase 1.**
