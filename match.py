#!/usr/bin/env python3
"""
Engine match manager — runs N games between two UCI engines,
alternating colors, using python-chess's engine protocol.
"""
import math, argparse, chess, chess.pgn, chess.engine

ENGINE_A = {"name": "ChessEngine-C",    "cmd": "/home/bertrand/chess-c/chess-engine-c"}
ENGINE_B = {"name": "ChessEngine-Rust", "cmd": "/home/bertrand/chess/target/release/chess-engine"}

GAMES   = 100
PGN_OUT = "/home/bertrand/chess-c/match.pgn"

OPENING_FENS = [
    "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
    "rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR b KQkq - 0 1",
    "rnbqkbnr/pppppppp/8/8/3P4/8/PPP1PPPP/RNBQKBNR b KQkq - 0 1",
    "rnbqkbnr/pppppppp/8/8/2P5/8/PP1PPPPP/RNBQKBNR b KQkq - 0 1",
    "rnbqkbnr/pppppppp/8/8/5P2/8/PPPPP1PP/RNBQKBNR b KQkq - 0 1",
    "rnbqkbnr/pppp1ppp/8/4p3/4P3/8/PPPP1PPP/RNBQKBNR w KQkq - 0 2",
    "rnbqkbnr/pp1ppppp/8/2p5/4P3/8/PPPP1PPP/RNBQKBNR w KQkq - 0 2",
    "rnbqkbnr/ppp1pppp/8/3p4/2PP4/8/PP2PPPP/RNBQKBNR b KQkq - 0 2",
    "rnbqkb1r/pppp1ppp/5n2/4p3/4P3/5N2/PPPP1PPP/RNBQKB1R w KQkq - 2 3",
    "r1bqkbnr/pppp1ppp/2n5/4p3/4P3/5N2/PPPP1PPP/RNBQKB1R w KQkq - 2 3",
]


def elo_diff(score, n):
    if score <= 0 or score >= n:
        return float("inf") if score >= n else float("-inf")
    p = score / n
    return -400 * math.log10(1 / p - 1)


def elo_err(score, n):
    if score <= 0 or score >= n:
        return float("inf")
    p = score / n
    dp = 1.96 * math.sqrt(p * (1 - p) / n)
    lo = -400 * math.log10(1 / max(p - dp, 1e-9) - 1)
    hi = -400 * math.log10(1 / min(p + dp, 1 - 1e-9) - 1)
    return (hi - lo) / 2


def play_game(eng_white, eng_black, white_name, black_name, opening_fen, tc, game_no):
    board = chess.Board(opening_fen)
    game  = chess.pgn.Game()
    game.setup(board)
    game.headers["White"]  = white_name
    game.headers["Black"]  = black_name
    game.headers["Round"]  = str(game_no)
    node  = game

    limit = chess.engine.Limit(white_clock=tc, black_clock=tc)

    while not board.is_game_over(claim_draw=True):
        eng = eng_white if board.turn == chess.WHITE else eng_black
        clk = chess.engine.Limit(white_clock=tc, black_clock=tc)
        try:
            result = eng.play(board, clk)
        except chess.engine.EngineError:
            break
        if result.move is None or result.move not in board.legal_moves:
            break
        node = node.add_variation(result.move)
        board.push(result.move)
        if board.fullmove_number > 200:
            break

    outcome = board.outcome(claim_draw=True)
    if outcome is None:
        game.headers["Result"] = "1/2-1/2"
        winner = "draw"
    elif outcome.winner == chess.WHITE:
        game.headers["Result"] = "1-0"
        winner = "white"
    elif outcome.winner == chess.BLACK:
        game.headers["Result"] = "0-1"
        winner = "black"
    else:
        game.headers["Result"] = "1/2-1/2"
        winner = "draw"

    return winner, game


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("-n", "--games", type=int, default=GAMES)
    parser.add_argument("-t", "--time",  type=float, default=5.0,
                        help="seconds per side per game (default 5)")
    args = parser.parse_args()

    n_games = args.games
    tc      = args.time

    print(f"Match: {ENGINE_A['name']} vs {ENGINE_B['name']}")
    print(f"Time control: {tc}s/game  |  {n_games} games  |  alternating colors")
    print(f"PGN: {PGN_OUT}\n")

    eng_a = chess.engine.SimpleEngine.popen_uci(ENGINE_A["cmd"])
    eng_b = chess.engine.SimpleEngine.popen_uci(ENGINE_B["cmd"])

    scores = {"A": 0.0, "B": 0.0}
    log    = []

    with open(PGN_OUT, "w") as pgn_file:
        for g in range(n_games):
            if g % 2 == 0:
                white_eng, black_eng = eng_a, eng_b
                white_name = ENGINE_A["name"]
                black_name = ENGINE_B["name"]
                a_is_white = True
            else:
                white_eng, black_eng = eng_b, eng_a
                white_name = ENGINE_B["name"]
                black_name = ENGINE_A["name"]
                a_is_white = False

            opening_fen = OPENING_FENS[g % len(OPENING_FENS)]

            winner, pgn_game = play_game(
                white_eng, black_eng, white_name, black_name,
                opening_fen, tc, g + 1
            )

            print(pgn_game, file=pgn_file, end="\n\n")
            pgn_file.flush()

            # Score from A's perspective
            if winner == "white":
                w = "A" if a_is_white else "B"
            elif winner == "black":
                w = "B" if a_is_white else "A"
            else:
                w = "draw"

            if w == "A":        scores["A"] += 1.0
            elif w == "B":      scores["B"] += 1.0
            else:               scores["A"] += 0.5; scores["B"] += 0.5
            log.append(w)

            played   = g + 1
            a_score  = scores["A"]
            elo      = elo_diff(a_score, played)
            err      = elo_err(a_score, played)
            sym      = {"A": "C", "B": "R", "draw": "="}[w]
            elo_str  = f"{elo:+.0f}±{err:.0f}" if not math.isinf(elo) else "±∞"
            print(f"  {played:3}/{n_games}  {sym}  "
                  f"C={a_score:.1f} R={scores['B']:.1f}  Elo {elo_str}",
                  flush=True)

    eng_a.quit()
    eng_b.quit()

    a_score = scores["A"]
    wins_a  = sum(1 for r in log if r == "A")
    wins_b  = sum(1 for r in log if r == "B")
    draws   = sum(1 for r in log if r == "draw")
    elo     = elo_diff(a_score, n_games)
    err     = elo_err(a_score, n_games)
    elo_str = f"{elo:+.0f} ± {err:.0f}" if not math.isinf(elo) else "no result"

    print()
    print("═" * 55)
    print(f"  {ENGINE_A['name']:<22}  {wins_a:3d} wins")
    print(f"  {ENGINE_B['name']:<22}  {wins_b:3d} wins")
    print(f"  Draws                    {draws:3d}")
    print(f"  Score: {a_score:.1f} / {n_games}")
    print(f"  Elo (C vs Rust): {elo_str}")
    print("═" * 55)
    print(f"  PGN → {PGN_OUT}")


if __name__ == "__main__":
    main()
