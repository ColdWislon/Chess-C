#!/usr/bin/env python3
"""Augment fast-chess / cutechess-cli stdout with game-count + elapsed + ETA.

Designed to sit at the end of a pipeline:
    fast-chess <args> | python3 tools/gauntlet-progress.py <TOTAL_GAMES>

Per-game lines ("Finished game N (…)") pass through unchanged so you can
still see individual results. Rating-interval blocks ("Score of …",
"Elo difference: …", "Results of …") get a prefix line printed first:

    [120/400  elapsed 8:14  ETA 19:21]
    Score of main vs lmr-tune-v2: 32 - 21 - 67  [0.531] 120
    Elo difference: 21.7 +/- 28.4, LOS: 93.4 %, DrawRatio: 55.8 %

ETA = (TOTAL - done) * elapsed/done. First ETA appears once the first game
finishes (need at least one data point). With 0 games or no TOTAL, no ETA.
"""
import re
import sys
import time

TOTAL = int(sys.argv[1]) if len(sys.argv) > 1 else 0

_FINISHED_RE = re.compile(r"Finished game (\d+)")


def fmt_dur(s: float) -> str:
    s = int(s)
    h, r = divmod(s, 3600)
    m, s = divmod(r, 60)
    return f"{h}:{m:02d}:{s:02d}" if h else f"{m:02d}:{s:02d}"


def main() -> None:
    start: float | None = None
    done = 0
    # Lines that get the ETA banner inserted ABOVE them. Match on prefix
    # because fast-chess and cutechess-cli format these consistently.
    BANNERED = (
        "Score of ",
        "Elo difference: ",
        "Results of ",
        "Finished match",
    )

    for raw in sys.stdin:
        line = raw.rstrip("\n")

        # Track per-game completions to drive the counter.
        m = _FINISHED_RE.search(line)
        if m:
            n = int(m.group(1))
            if start is None:
                start = time.time()
            done = max(done, n)
            print(line, flush=True)
            continue

        # Prepend an ETA banner before each "interesting" status block.
        if any(line.startswith(p) for p in BANNERED):
            if start is not None and done > 0 and TOTAL > 0 and done < TOTAL:
                elapsed = time.time() - start
                avg = elapsed / done
                eta = (TOTAL - done) * avg
                banner = (f"[{done}/{TOTAL}  "
                          f"elapsed {fmt_dur(elapsed)}  "
                          f"ETA {fmt_dur(eta)}]")
                print(banner, flush=True)
            print(line, flush=True)
            continue

        # Pass everything else through verbatim.
        print(line, flush=True)


if __name__ == "__main__":
    try:
        main()
    except (BrokenPipeError, KeyboardInterrupt):
        pass
