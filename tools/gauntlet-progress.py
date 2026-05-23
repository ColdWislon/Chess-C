#!/usr/bin/env python3
"""Filter fast-chess / cutechess-cli output to a clean status stream.

Drops engine UCI noise (info depth/string lines, opening-book / syzygy
startup messages, per-game completion spam) and emits one compact block
per rating-interval plus the final results block:

    [120/400  elapsed 8:14  ETA 19:21]
      main vs lmr-tune-v2   32 - 67 - 21   Elo -8.7 ±23.6   LOS 76.5%  (lmr-tune-v2 ahead)

Plus the final "Results of …" block verbatim.

Use:
    fast-chess <args> 2>&1 | python3 tools/gauntlet-progress.py <TOTAL_GAMES>

The 2>&1 matters — engine stderr (info lines) bypasses any pipe that
only takes stdout, so we have to merge stderr in to filter it.
"""
import re
import sys
import time

TOTAL = int(sys.argv[1]) if len(sys.argv) > 1 else 0

_FINISHED_RE = re.compile(r"Finished game (\d+)")
_SCORE_RE    = re.compile(r"^Score of (\S+) vs (\S+):\s*(\d+)\s*-\s*(\d+)\s*-\s*(\d+)")
_ELO_RE      = re.compile(r"^Elo difference:\s*([-+\d.]+)\s*\+/-\s*([\d.]+),\s*LOS:\s*([\d.]+)")
_RESULTS_RE  = re.compile(r"^Results of ")


def fmt_dur(s: float) -> str:
    s = int(s)
    h, r = divmod(s, 3600)
    m, s = divmod(r, 60)
    return f"{h}:{m:02d}:{s:02d}" if h else f"{m}:{s:02d}"


def main() -> None:
    start: float | None = None
    done = 0
    last_score = None     # (A, B, wins, losses, draws) from latest Score line
    in_results_block = False

    for raw in sys.stdin:
        line = raw.rstrip("\n")

        # Track game completions silently — drives the counter / ETA.
        m = _FINISHED_RE.search(line)
        if m:
            n = int(m.group(1))
            if start is None:
                start = time.time()
            done = max(done, n)
            continue

        # Stash the latest Score line; emit when the matching Elo line lands.
        m = _SCORE_RE.match(line)
        if m:
            last_score = (m.group(1), m.group(2),
                          int(m.group(3)), int(m.group(4)), int(m.group(5)))
            continue

        m = _ELO_RE.match(line)
        if m and last_score is not None:
            A, B, w, l, d = last_score
            elo = float(m.group(1))
            err = float(m.group(2))
            los = float(m.group(3))
            # Header banner
            if start and done and TOTAL:
                elapsed = time.time() - start
                if done < TOTAL:
                    eta = (TOTAL - done) * (elapsed / done)
                    head = (f"[{done}/{TOTAL}  "
                            f"elapsed {fmt_dur(elapsed)}  "
                            f"ETA {fmt_dur(eta)}]")
                else:
                    head = f"[{done}/{TOTAL}  elapsed {fmt_dur(elapsed)}  done]"
            else:
                head = f"[{done}]"
            # Who's ahead annotation
            if   elo >  1: leader = f"{A} ahead"
            elif elo < -1: leader = f"{B} ahead"
            else:          leader = "even"
            print(head, flush=True)
            print(f"  {A} vs {B}   {w} - {d} - {l}   "
                  f"Elo {elo:+.1f} ±{err:.1f}   "
                  f"LOS {los:.1f}%  ({leader})",
                  flush=True)
            print(flush=True)
            last_score = None
            continue

        # Final "Results of …" block — print verbatim so the user gets the
        # full breakdown (Ptnml, WL/DD, etc.).
        if _RESULTS_RE.match(line):
            in_results_block = True
        if line.startswith("Finished match") or line.startswith("Total Time:"):
            in_results_block = False
            print(line, flush=True)
            continue
        if in_results_block:
            print(line, flush=True)
            continue

        # Drop everything else (info lines, startup, etc.).


if __name__ == "__main__":
    try:
        main()
    except (BrokenPipeError, KeyboardInterrupt):
        pass
