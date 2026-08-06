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
