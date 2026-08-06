#!/usr/bin/env python3
import io
import sys
import tokenize
import ast
from pathlib import Path

def strip_comments(src: str) -> str:
    tokens = []
    for tok in tokenize.generate_tokens(io.StringIO(src).readline):
        tok_type = tok[0]
        tok_str = tok[1]
        if tok_type == tokenize.COMMENT:
            if tok_str.startswith('#!') and tok[2][0] == 1:
                tokens.append(tok)
            continue
        tokens.append(tok)
    return tokenize.untokenize(tokens)

def strip_docstrings(src: str) -> str:
    try:
        tree = ast.parse(src)
    except SyntaxError:
        return src
    lines_to_remove = set()
    def visit(node):
        if isinstance(node, (ast.Module, ast.FunctionDef, ast.AsyncFunctionDef, ast.ClassDef)):
            body = getattr(node, 'body', None)
            if body and isinstance(body[0], ast.Expr) and isinstance(body[0].value, ast.Constant) and isinstance(body[0].value.value, str):
                for ln in range(body[0].lineno, body[0].end_lineno + 1):
                    lines_to_remove.add(ln)
        for child in ast.iter_child_nodes(node):
            visit(child)
    visit(tree)
    if not lines_to_remove:
        return src
    lines = src.splitlines()
    kept = [ln for i, ln in enumerate(lines, 1) if i not in lines_to_remove]
    return '\n'.join(kept).rstrip() + '\n'

def collapse_blank_lines(src: str) -> str:
    out = []
    prev_blank = False
    for line in src.splitlines():
        rstrip = line.rstrip()
        is_blank = not rstrip.strip()
        if is_blank and prev_blank:
            continue
        out.append(rstrip)
        prev_blank = is_blank
    return '\n'.join(out).rstrip() + '\n'

def main():
    if len(sys.argv) < 2:
        print("usage: strip_py_comments.py FILE [FILE ...]", file=sys.stderr)
        sys.exit(2)
    for arg in sys.argv[1:]:
        p = Path(arg)
        src = p.read_text(encoding='utf-8')
        src = strip_docstrings(src)
        src = strip_comments(src)
        src = collapse_blank_lines(src)
        try:
            ast.parse(src)
        except SyntaxError as e:
            print(f"PARSE FAIL after strip: {arg}: {e}", file=sys.stderr)
            sys.exit(3)
        p.write_text(src, encoding='utf-8')

if __name__ == '__main__':
    main()
