#!/usr/bin/env python3
"""One Stockfish worker: read FEN|cp|result lines from --in, drive a single
persistent Stockfish process at fixed depth, write FEN|sf_cp|result to --out.

Why a UCI conversation instead of `stockfish bench` or a fen-batch tool: lets
us hold Stockfish's hash + TT warm across positions in the chunk, so each
label costs roughly one search instead of one search + a cold start.

Score is from side-to-move POV — matches what the C engine's datagen wrote
and what the dataset/training loop expects.

Mate scores get clamped to ±32000; the trainer's sigmoid(cp/SCALE) blend
saturates near 1.0/0.0 for those anyway, so the exact mate distance is noise.
"""
import argparse
import re
import subprocess
import sys

INFO_LINE_RE = re.compile(r"score (cp|mate) (-?\d+)")
MATE_CLAMP = 32000


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--in", dest="inp", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--depth", type=int, default=10)
    ap.add_argument("--sf", required=True, help="path to stockfish binary")
    args = ap.parse_args()

    sf = subprocess.Popen([args.sf], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                          text=True, bufsize=1)

    def send(cmd):
        sf.stdin.write(cmd + "\n")
        sf.stdin.flush()

    def read_until(prefix):
        last = None
        for line in sf.stdout:
            if line.startswith(prefix):
                return line.rstrip()
            if line.startswith("info "):
                last = line.rstrip()
        return last

    send("uci")
    read_until("uciok")
    send("setoption name Hash value 16")    # tiny — chunks are small + cold
    send("setoption name Threads value 1")  # parallelism is across workers
    send("isready")
    read_until("readyok")

    skipped = 0
    written = 0
    with open(args.inp) as fin, open(args.out, "w") as fout:
        for line in fin:
            line = line.strip()
            if not line:
                continue
            try:
                fen_part, _cp_part, res_part = line.split("|")
            except ValueError:
                skipped += 1
                continue
            fen = fen_part.strip()
            res = res_part.strip()

            send(f"position fen {fen}")
            send(f"go depth {args.depth}")

            # We want the LAST score line before bestmove (the deepest one),
            # so read all info lines, then bestmove.
            score = None
            for ol in sf.stdout:
                if ol.startswith("info "):
                    m = INFO_LINE_RE.search(ol)
                    if m:
                        kind, val = m.group(1), int(m.group(2))
                        if kind == "cp":
                            score = val
                        else:
                            # mate N (or -N) — clamp; sign already from-stm-POV
                            score = MATE_CLAMP if val > 0 else -MATE_CLAMP
                elif ol.startswith("bestmove"):
                    break

            if score is None:
                skipped += 1
                continue
            fout.write(f"{fen} | {score} | {res}\n")
            written += 1
            if written % 5000 == 0:
                fout.flush()
                print(f"[{args.inp.rsplit('/',1)[-1]}] {written} lines", file=sys.stderr)

    send("quit")
    sf.wait(timeout=5)
    print(f"[{args.inp.rsplit('/',1)[-1]}] DONE: {written} written, {skipped} skipped",
          file=sys.stderr)


if __name__ == "__main__":
    main()
