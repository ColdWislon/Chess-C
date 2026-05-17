#!/usr/bin/env python3
"""
Score comparison: feed the same positions to both engines at fixed depth,
compare best move and centipawn score.
"""
import subprocess, sys, re, time

RUST_BIN = "/home/bertrand/chess/target/release/chess-engine"
C_BIN    = "/home/bertrand/chess-c/chess-engine-c"
DEPTH    = 8

POSITIONS = [
    ("startpos",             "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1"),
    ("e4 e5",                "rnbqkbnr/pppp1ppp/8/4p3/4P3/8/PPPP1PPP/RNBQKBNR w KQkq - 0 2"),
    ("Sicilian after 1.e4c5","rnbqkbnr/pp1ppppp/8/2p5/4P3/8/PPPP1PPP/RNBQKBNR w KQkq - 0 2"),
    ("Italian game",         "r1bqkbnr/pppp1ppp/2n5/4p3/2B1P3/5N2/PPPP1PPP/RNBQK2R b KQkq - 3 3"),
    ("Ruy Lopez",            "r1bqkb1r/pppp1ppp/2n2n2/1B2p3/4P3/5N2/PPPP1PPP/RNBQK2R w KQkq - 4 4"),
    ("Queen's Gambit",       "rnbqkbnr/ppp1pppp/8/3p4/2PP4/8/PP2PPPP/RNBQKBNR b KQkq - 0 2"),
    ("Middlegame tactics",   "r1bq1rk1/pp2bppp/2n1pn2/3p4/3P1B2/2NBP3/PP3PPP/R2QK1NR w KQ - 0 9"),
    ("King safety",          "2rq1rk1/pp3ppp/2n1p3/3pP3/3P4/2PB1N2/PP3PPP/R2QR1K1 w - - 0 14"),
    ("Endgame R+P",          "8/5pk1/8/3R4/4K3/8/8/8 w - - 0 1"),
    ("Pawn structure",       "r1bqr1k1/pp2bppp/2n1pn2/2pp4/3P1B2/2NBP3/PP1N1PPP/R2QR1K1 w - - 0 10"),
    ("Pin position",         "rnb1kbnr/pppp1ppp/8/4p3/4PP1q/8/PPPP2PP/RNBQKBNR w KQkq - 1 3"),
    ("Open file",            "r4rk1/pp1n1ppp/2p1p3/3pP3/3P4/2PB1N2/PP3PPP/R4RK1 w - - 0 13"),
]


def query_engine(binary, fen, depth):
    cmd = [binary]
    uci_input = (
        "uci\n"
        "isready\n"
        f"position fen {fen}\n"
        f"go depth {depth}\n"
        "quit\n"
    )
    try:
        proc = subprocess.run(
            cmd, input=uci_input, capture_output=True, text=True, timeout=60
        )
        out = proc.stdout + proc.stderr
    except subprocess.TimeoutExpired:
        return None, None, None

    # Find last "info depth N score cp X nodes Y"
    score = None
    nodes = None
    for m in re.finditer(r"info depth \d+ score cp (-?\d+) nodes (\d+)", out):
        score = int(m.group(1))
        nodes = int(m.group(2))

    # Best move
    bm = None
    m = re.search(r"bestmove (\S+)", out)
    if m:
        bm = m.group(1)

    return score, bm, nodes


def fmt_score(s):
    if s is None: return "   N/A "
    return f"{s:+7d}"

def main():
    print(f"Comparing engines at depth {DEPTH}")
    print(f"  Rust: {RUST_BIN}")
    print(f"  C:    {C_BIN}")
    print()
    print(f"{'Position':<26} {'Score Rust':>10} {'Score C':>10} {'Δ':>7}  {'Move Rust':<10} {'Move C':<10} {'Match'}")
    print("-" * 95)

    matches = 0
    score_diffs = []
    total = 0

    for name, fen in POSITIONS:
        t0 = time.time()
        r_score, r_move, r_nodes = query_engine(RUST_BIN, fen, DEPTH)
        c_score, c_move, c_nodes = query_engine(C_BIN,    fen, DEPTH)
        elapsed = time.time() - t0

        if r_score is None or c_score is None:
            print(f"  {name:<24}  TIMEOUT or error")
            continue

        diff = c_score - r_score
        score_diffs.append(abs(diff))
        same_move = (r_move == c_move)
        if same_move: matches += 1
        total += 1

        match_str = "✓" if same_move else "✗"
        print(f"  {name:<24} {fmt_score(r_score)} {fmt_score(c_score)} {diff:>+7}  {r_move or '?':<10} {c_move or '?':<10} {match_str}  ({elapsed:.1f}s)")

    print("-" * 95)
    if total:
        avg_diff = sum(score_diffs) / total
        print(f"\nResults: {matches}/{total} same best move  |  avg |score diff| = {avg_diff:.1f} cp")
        if avg_diff <= 5 and matches == total:
            print("PASS — engines agree within noise.")
        elif avg_diff <= 30:
            print("CLOSE — small eval differences expected (Zobrist keys differ, mobility bug replicated).")
        else:
            print("DIVERGE — significant differences, investigate.")


if __name__ == "__main__":
    main()
