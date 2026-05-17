#!/usr/bin/env python3
"""Ladder qualification: ChessEngine-C vs Stockfish at multiple skill levels.

20 games per skill level (alternating colors), 2s/move, prints progress and
per-level summary + crude elo estimate. Output also goes to a PGN file.
"""
import math, sys, time
import chess, chess.engine, chess.pgn

OUR_CMD       = "/home/bertrand/chess-c/chess-engine-c"
SF_CMD        = "/usr/games/stockfish"
MOVETIME      = 2.0
GAMES_PER_LVL = 20
SF_SKILLS     = [5, 8, 12, 16]
THREADS       = 4
PGN_OUT       = "/home/bertrand/chess-c/ladder_match.pgn"
LOG_OUT       = "/home/bertrand/chess-c/ladder_match.log"


def elo_from_score(score, n):
    """Approximate elo diff from score (0..n). Returns inf for sweeps."""
    if n == 0:
        return 0
    p = score / n
    if p <= 0.0:   return float("-inf")
    if p >= 1.0:   return float("inf")
    return -400.0 * math.log10(1.0 / p - 1.0)


def play_one(white_eng, black_eng, white_name, black_name, skill):
    board = chess.Board()
    game  = chess.pgn.Game()
    game.headers["Event"]     = f"Ladder vs SF skill {skill}"
    game.headers["White"]     = white_name
    game.headers["Black"]     = black_name
    game.headers["TimeControl"] = f"movetime {int(MOVETIME*1000)}ms"
    node = game

    plies = 0
    while not board.is_game_over(claim_draw=True):
        eng = white_eng if board.turn == chess.WHITE else black_eng
        try:
            result = eng.play(board, chess.engine.Limit(time=MOVETIME))
        except chess.engine.EngineTerminatedError as e:
            print(f"    ENGINE CRASH: {e}", flush=True)
            return None, plies, game
        if result.move is None:
            break
        node = node.add_variation(result.move)
        board.push(result.move)
        plies += 1
        if plies > 300:  # safety
            break

    res = board.result(claim_draw=True)
    game.headers["Result"] = res
    return res, plies, game


def run_level(skill, log):
    print(f"\n{'='*68}", flush=True)
    print(f"= SF Skill {skill}  —  {GAMES_PER_LVL} games, {MOVETIME}s/move", flush=True)
    print(f"{'='*68}", flush=True)
    log.write(f"\n== SF Skill {skill} ==\n")

    ours = chess.engine.SimpleEngine.popen_uci(OUR_CMD)
    sf   = chess.engine.SimpleEngine.popen_uci(SF_CMD)
    ours.configure({"Threads": THREADS})
    sf.configure({"Skill Level": skill})

    score = 0.0
    games = []
    try:
        for g in range(GAMES_PER_LVL):
            our_white = (g % 2 == 0)
            wn = "ChessEngine-C" if our_white else f"SF-skill-{skill}"
            bn = f"SF-skill-{skill}" if our_white else "ChessEngine-C"
            if our_white:
                res, plies, game = play_one(ours, sf, wn, bn, skill)
            else:
                res, plies, game = play_one(sf, ours, wn, bn, skill)

            if res is None:
                outcome = "ABORT"
                pts = 0.0
            elif res == "1-0":
                pts = 1.0 if our_white else 0.0
                outcome = "W" if our_white else "L"
            elif res == "0-1":
                pts = 0.0 if our_white else 1.0
                outcome = "L" if our_white else "W"
            else:
                pts = 0.5
                outcome = "D"
            score += pts

            colour = "W" if our_white else "B"
            line = (f"  Game {g+1:2}/{GAMES_PER_LVL}  ({colour})  "
                    f"{outcome}  {plies:3} plies  "
                    f"running {score:.1f}/{g+1}")
            print(line, flush=True)
            log.write(line + "\n"); log.flush()

            if game:
                games.append(game)
    finally:
        ours.quit()
        sf.quit()

    n = GAMES_PER_LVL
    elo = elo_from_score(score, n)
    summary = (f"  Final {score:.1f}/{n}  ({100*score/n:.0f}%)  "
               f"≈ {elo:+.0f} elo vs SF skill {skill}")
    print(summary, flush=True)
    log.write(summary + "\n"); log.flush()
    return score, games


def main():
    t_start = time.time()
    log = open(LOG_OUT, "w")
    log.write(f"Ladder match: {GAMES_PER_LVL} games per skill, "
              f"{MOVETIME}s/move, threads={THREADS}\n")
    log.write(f"Levels: {SF_SKILLS}\n")
    log.flush()

    all_games = []
    results = []
    for skill in SF_SKILLS:
        score, games = run_level(skill, log)
        all_games.extend(games)
        results.append((skill, score))

    elapsed = time.time() - t_start
    print(f"\n{'='*68}", flush=True)
    print("== FINAL LADDER ==", flush=True)
    log.write("\n== FINAL LADDER ==\n")
    for skill, score in results:
        elo = elo_from_score(score, GAMES_PER_LVL)
        line = (f"  SF skill {skill:2}: {score:.1f}/{GAMES_PER_LVL}  "
                f"({100*score/GAMES_PER_LVL:.0f}%)  ≈ {elo:+.0f} elo")
        print(line, flush=True)
        log.write(line + "\n")
    print(f"\n  Total wall time: {elapsed/60:.1f} min", flush=True)
    log.write(f"\nTotal wall time: {elapsed/60:.1f} min\n")
    log.close()

    with open(PGN_OUT, "w") as f:
        for g in all_games:
            print(g, file=f, end="\n\n")
    print(f"  PGN saved to {PGN_OUT}", flush=True)
    print(f"  Log saved to {LOG_OUT}", flush=True)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("\nInterrupted.")
        sys.exit(1)
