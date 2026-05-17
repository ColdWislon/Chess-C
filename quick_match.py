#!/usr/bin/env python3
"""Quick test game: ChessEngine-C vs Stockfish (throttled)."""
import sys, chess, chess.engine

OUR_CMD = "/home/bertrand/chess-c/chess-engine-c"
SF_CMD  = "/usr/games/stockfish"
MOVETIME = 2.0
SF_SKILL = 5      # 0..20; ~1500 elo at 5
GAMES    = 4

def play(white_eng, black_eng, white_name, black_name):
    board = chess.Board()
    moves = []
    while not board.is_game_over(claim_draw=True):
        engine = white_eng if board.turn == chess.WHITE else black_eng
        result = engine.play(board, chess.engine.Limit(time=MOVETIME))
        moves.append(board.san(result.move))
        board.push(result.move)
        if len(moves) > 200:  # safety
            break
    res = board.result(claim_draw=True)
    print(f"  Result: {res}  ({len(moves)} plies)")
    print(f"  Moves: {' '.join(moves)}")
    return res

def main():
    ours = chess.engine.SimpleEngine.popen_uci(OUR_CMD)
    sf   = chess.engine.SimpleEngine.popen_uci(SF_CMD)
    ours.configure({"Threads": 4})
    sf.configure({"Skill Level": SF_SKILL})

    score = {"ours": 0.0, "sf": 0.0}
    try:
        for g in range(GAMES):
            our_white = (g % 2 == 0)
            print(f"\n== Game {g+1}: {'ChessEngine-C (W) vs Stockfish (B)' if our_white else 'Stockfish (W) vs ChessEngine-C (B)'} ==")
            if our_white:
                res = play(ours, sf, "ChessEngine-C", "Stockfish")
            else:
                res = play(sf, ours, "Stockfish", "ChessEngine-C")

            if res == "1-0":
                if our_white: score["ours"] += 1
                else:         score["sf"]   += 1
            elif res == "0-1":
                if our_white: score["sf"]   += 1
                else:         score["ours"] += 1
            else:
                score["ours"] += 0.5
                score["sf"]   += 0.5
    finally:
        ours.quit()
        sf.quit()

    print(f"\n== Final: ChessEngine-C {score['ours']}  vs  Stockfish(skill={SF_SKILL}) {score['sf']} ==")

if __name__ == "__main__":
    main()
