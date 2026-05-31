#!/usr/bin/env python3
"""Cross-validation gate: prove the C engine and the Python int path compute the
SAME quantized eval, byte-for-byte. This is what guarantees a net trained in
Python will behave identically once loaded by the engine.

Deliberately torch-free (pure python + the engine binary) so it runs on the Pi.
It builds a random *quantized* net directly, writes a .nnue, then for a battery
of FENs compares:
    engine:  setoption EvalFile -> position fen -> `eval`
    python:  the integer arithmetic from src/nnue.c, replicated here
Exact integer match required.

Usage:  python3 check_parity.py [path-to-engine]   (default ../../chess-engine-c)
"""
import array
import os
import random
import struct
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from features import (parse_placement, feature_index, side_to_move,
                      QA, QB, SCALE, N_FEATURES)

N = 256
NET_PATH = '/tmp/parity.nnue'

FENS = [
    "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",          # startpos
    "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR b KQkq - 0 1",          # startpos, black
    "r1bqkbnr/pppp1ppp/2n5/4p3/4P3/5N2/PPPP1PPP/RNBQKB1R w KQkq - 2 3",  # open game
    "r1bq1rk1/pp2bppp/2n1pn2/3p4/3P1B2/2NBP3/PP3PPP/R2QK1NR w KQ - 0 9", # middlegame
    "8/8/8/4k3/8/4K3/4P3/8 w - - 0 1",                                   # KPvK
    "8/2k5/8/8/8/5K2/8/7R w - - 0 1",                                    # KRvK
    "r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1",                              # castling rights
    "4k3/8/8/3pP3/8/8/8/4K3 w - d6 0 1",                                 # en passant sq
    "8/8/8/8/8/8/6k1/4K2R w K - 0 1",                                    # few pieces
    "rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8",         # busy position
]


def trunc_div(num, den):
    """C integer division truncates toward zero; Python // floors. Match C."""
    q = abs(num) // den
    return q if num >= 0 else -q


def build_random_net(seed=1234):
    random.seed(seed)
    # Modest weight ranges: accumulators land across [0,QA] (not all saturated),
    # so the test actually exercises the clamp and the dot product.
    W = [[random.randint(-80, 80) for _ in range(N)] for _ in range(N_FEATURES)]
    b = [random.randint(-40, 40) for _ in range(N)]
    O = [random.randint(-60, 60) for _ in range(2 * N)]
    ob = random.randint(-200000, 200000)
    return W, b, O, ob


def write_nnue(path, W, b, O, ob):
    with open(path, 'wb') as f:
        f.write(b'CNUE')
        f.write(struct.pack('<III', 1, N, N_FEATURES))
        flat = array.array('h', [W[fi][n] for fi in range(N_FEATURES) for n in range(N)])
        f.write(flat.tobytes())
        f.write(array.array('h', b).tobytes())
        f.write(array.array('h', O).tobytes())
        f.write(struct.pack('<i', ob))


def python_eval(W, b, O, ob, fen):
    pieces = parse_placement(fen)

    def acc_for(persp):
        a = list(b)
        for (c, p, s) in pieces:
            col = W[feature_index(persp, c, p, s)]
            for n in range(N):
                a[n] += col[n]
        return [0 if x < 0 else QA if x > QA else x for x in a]  # clipped ReLU

    stm = side_to_move(fen)
    us = acc_for(stm)
    them = acc_for(1 - stm)
    out = ob
    for n in range(N):
        out += us[n] * O[n] + them[n] * O[N + n]
    return trunc_div(out * SCALE, QA * QB)


def engine_eval(engine, fens):
    cmds = "setoption name EvalFile value %s\n" % NET_PATH
    for fen in fens:
        cmds += "position fen %s\neval\n" % fen
    cmds += "quit\n"
    out = subprocess.run([engine], input=cmds, capture_output=True, text=True).stdout
    evals = [int(l.split()[1]) for l in out.splitlines() if l.startswith("eval ")]
    return evals


def main():
    engine = sys.argv[1] if len(sys.argv) > 1 else \
        os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'chess-engine-c')
    if not os.path.exists(engine):
        print(f"engine not found: {engine} (run `make release`)"); sys.exit(2)

    W, b, O, ob = build_random_net()
    write_nnue(NET_PATH, W, b, O, ob)
    print(f"wrote random net -> {NET_PATH}")

    py = [python_eval(W, b, O, ob, fen) for fen in FENS]
    eng = engine_eval(engine, FENS)
    if len(eng) != len(FENS):
        print(f"FAIL: engine returned {len(eng)} evals, expected {len(FENS)}")
        print("engine output may indicate a load failure; check stderr."); sys.exit(1)

    ok = True
    print(f"\n{'#':>2}  {'python':>8}  {'engine':>8}  result   fen")
    for i, (fen, p, e) in enumerate(zip(FENS, py, eng)):
        match = (p == e)
        ok &= match
        print(f"{i:>2}  {p:>8}  {e:>8}  {'OK' if match else 'MISMATCH':>8}   {fen}")

    print()
    if ok:
        print("PARITY OK — C and Python compute identical quantized evals.")
        sys.exit(0)
    print("PARITY FAILED — implementations diverged. Reconcile before training.")
    sys.exit(1)


if __name__ == '__main__':
    main()
