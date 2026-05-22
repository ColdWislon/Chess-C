#!/usr/bin/env python3
"""
Apply a texel-snapshot.txt to src/eval.c by replacing matching arrays in
place. Each `int NAME[N] = { ... };` block in the snapshot replaces the
identically-named block in eval.c.

Usage:
    python3 tools/apply-texel-snapshot.py [SNAPSHOT] [EVAL_C]

Defaults: SNAPSHOT=texel-snapshot.txt, EVAL_C=src/eval.c.

Writes a sibling backup at EVAL_C + ".bak.<timestamp>" before mutating so
you can revert with `mv src/eval.c.bak.* src/eval.c`. The script is
intentionally Python (not sed) because chess PSTs are multi-line arrays and
sed substitution on multi-line patterns is fragile.
"""

import pathlib
import re
import sys
import time


def parse_snapshot(text: str) -> dict[str, str]:
    """Extract every `int NAME[N] = { ... };` block, keyed by NAME."""
    pat = re.compile(r'(int\s+(\w+)\[\d+\]\s*=\s*\{[^}]*\};)', re.DOTALL)
    return {m.group(2): m.group(1) for m in pat.finditer(text)}


def main() -> None:
    snap_path = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else
                             "texel-snapshot.txt")
    eval_path = pathlib.Path(sys.argv[2] if len(sys.argv) > 2 else
                             "src/eval.c")
    if not snap_path.exists():
        sys.exit(f"snapshot not found: {snap_path}")
    if not eval_path.exists():
        sys.exit(f"eval.c not found: {eval_path}")

    arrays = parse_snapshot(snap_path.read_text())
    if not arrays:
        sys.exit(f"no `int NAME[N] = {{ ... }};` blocks in {snap_path}")
    print(f"parsed {len(arrays)} arrays from {snap_path}: "
          f"{', '.join(sorted(arrays))}", file=sys.stderr)

    # Backup before mutating so a botched run can be rolled back without git.
    backup = eval_path.with_suffix(eval_path.suffix +
                                   f".bak.{int(time.time())}")
    backup.write_bytes(eval_path.read_bytes())
    print(f"backed up {eval_path} → {backup}", file=sys.stderr)

    eval_text = eval_path.read_text()
    replaced, missing = [], []
    for name, block in arrays.items():
        pat = re.compile(r'int\s+' + re.escape(name) +
                         r'\[\d+\]\s*=\s*\{[^}]*\};', re.DOTALL)
        new_text, n = pat.subn(block, eval_text, count=1)
        if n == 0:
            missing.append(name)
        else:
            eval_text = new_text
            replaced.append(name)

    eval_path.write_text(eval_text)
    print(f"replaced {len(replaced)} arrays in {eval_path}", file=sys.stderr)
    if missing:
        print(f"WARNING: {len(missing)} arrays NOT found "
              f"in {eval_path}: {', '.join(missing)}", file=sys.stderr)
        print(f"({eval_path} has been partially updated; "
              f"original is at {backup})", file=sys.stderr)
        sys.exit(1)
    print(f"done. revert with: mv {backup} {eval_path}", file=sys.stderr)


if __name__ == "__main__":
    main()
