#!/usr/bin/env python3
"""Generate build-info.json from `git log`.

The dashboard's "Engine build" card consumes this file. Schema (matches what
dashboard/index.html parses):

  {
    "build_date": "YYYY-MM-DD",
    "version":    "<short-sha> (last N commits shown)",
    "summary":    "subject of HEAD commit",
    "kept":       [ {name, kind, impact, ab_result?}, ... ],
    "rejected":   []          # left empty for hand-curated rejected items
  }

`kind` is mapped from the conventional commit prefix ("search:", "tt:",
"dashboard:", ...) so the dashboard's color-coded badges stay meaningful.
"""

import json
import os
import re
import subprocess
import sys
from datetime import date

N = int(os.environ.get("BUILD_INFO_COMMITS", "20"))

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(REPO)


def git(*args: str) -> str:
    return subprocess.check_output(["git", *args], text=True).strip()


def classify(area: str) -> str:
    a = area.lower()
    if any(k in a for k in ("fix", "bug")):                       return "correctness"
    if any(k in a for k in ("search", "eval")):                   return "search-efficiency"
    if any(k in a for k in ("tt", "table")):                      return "quality"
    if any(k in a for k in ("dashboard", "ui")):                  return "ergonomics"
    if any(k in a for k in ("tools", "make", "build", "ci",
                            "doc")):                              return "ergonomics"
    return "quality"


AB_PATTERNS = [
    # "-2.07% wall time", "+1.50% NPS", etc.
    re.compile(r"[-+]?\d+\.\d+\s*%\s*[A-Za-z][A-Za-z ]*"),
    # "+4 plies", "+1 ply"
    re.compile(r"\+\d+\s*pl(?:y|ies)"),
    re.compile(r"identical\s+nodes", re.IGNORECASE),
    re.compile(r"search\s+tree\s+changed", re.IGNORECASE),
]


def parse_commit(record: str) -> dict | None:
    rec = record.strip()
    if not rec:
        return None
    parts = rec.split("\t", 2)
    if len(parts) < 2:
        return None
    sha, subject = parts[0], parts[1]
    body = parts[2] if len(parts) > 2 else ""

    if ":" in subject:
        area, name = subject.split(":", 1)
        area, name = area.strip(), name.strip()
    else:
        area, name = "commit", subject.strip()

    # Impact = first non-blank body line, capped.
    impact = subject
    for line in body.splitlines():
        line = line.strip()
        if line:
            impact = line[:200]
            break

    ab: list[str] = []
    for pat in AB_PATTERNS:
        for m in pat.finditer(body):
            s = m.group(0).strip(" .")
            if s and s not in ab:
                ab.append(s)

    entry = {
        "name":   f"{name}  [{sha}]",
        "kind":   classify(area),
        "impact": impact,
    }
    if ab:
        entry["ab_result"] = "; ".join(ab[:4])
    return entry


def main() -> None:
    head_sha = git("rev-parse", "--short", "HEAD")
    latest_subject = git("log", "-1", "--pretty=%s")

    # %x1e = ASCII record separator (won't appear in commit messages).
    raw = git(
        "log", "-n", str(N),
        "--pretty=format:%h%x09%s%x09%b%x1e",
    )
    records = raw.split("\x1e")
    kept = [e for e in (parse_commit(r) for r in records) if e]

    payload = {
        "build_date": date.today().isoformat(),
        "version":    f"{head_sha} (last {N} commits)",
        "summary":    latest_subject,
        "kept":       kept,
        "rejected":   [],
    }
    with open("build-info.json", "w") as f:
        json.dump(payload, f, indent=2)
        f.write("\n")
    print(f"wrote build-info.json (HEAD={head_sha}, {len(kept)} commits)")


if __name__ == "__main__":
    main()
