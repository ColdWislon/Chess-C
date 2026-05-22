#!/usr/bin/env python3
"""
Generate a Texel-style labeled corpus from a Lichess user's game history.

The classic quiet-labeled.epd (Ethereal / Zurichess) has been removed from
public hosting, so `make corpus` now builds its own from rpiBot73's actual
games — which is arguably better, since the tuner ends up fitting positions
the engine will really face.

Usage:
    python3 tools/gen-corpus.py [--user rpiBot73] [--max 2000] [--out quiet-labeled.epd]

Output format (one position per line):
    <FEN> c9 "<result>";

where <result> is "1-0", "0-1", or "1/2-1/2", matching what the C tuner's
load_epd() expects.

Filtering: positions in check are skipped (the score is dominated by the
forcing line, not the static eval). Captures are NOT filtered — quiescence
inside our eval is good enough that we just feed the raw mid-game position.
Opening (first 8 plies) and very-late mate sequences (last 4 plies) are
trimmed because their results are dominated by book / tactics.

Dependencies (install once):
    pip install python-chess requests
"""

import argparse
import sys
from io import StringIO

try:
    import requests
    import chess
    import chess.pgn
except ImportError as e:
    sys.stderr.write(
        f"Missing dependency: {e.name}\n"
        f"Install with:\n  pip install python-chess requests\n"
    )
    sys.exit(2)


def fetch_pgn(user: str, max_games: int) -> str:
    """Stream rated games from Lichess as PGN. Returns the raw text."""
    url = f"https://lichess.org/api/games/user/{user}"
    params = {
        "max": str(max_games),
        "rated": "true",
        "clocks": "false",
        "evals": "false",
        "opening": "false",
        # No annotations/variations — keep payload small.
    }
    headers = {"Accept": "application/x-chess-pgn"}
    sys.stderr.write(f"fetching up to {max_games} games for {user}…\n")
    r = requests.get(url, params=params, headers=headers, stream=True,
                     timeout=120)
    r.raise_for_status()
    # Chunks → string. Lichess returns one PGN concatenated for all games.
    chunks = []
    bytes_total = 0
    for chunk in r.iter_content(chunk_size=65536, decode_unicode=False):
        chunks.append(chunk)
        bytes_total += len(chunk)
        if bytes_total % (1024 * 1024) < 65536:
            sys.stderr.write(f"  downloaded {bytes_total // 1024} KB\r")
    sys.stderr.write(f"  downloaded {bytes_total // 1024} KB total\n")
    return b"".join(chunks).decode("utf-8", errors="replace")


def extract_positions(pgn_text: str, opening_skip: int = 8,
                      end_skip: int = 4):
    """Walk every game and yield (fen, result) tuples for the eligible plies."""
    n_games  = 0
    n_skip   = 0
    n_pos    = 0
    stream   = StringIO(pgn_text)
    while True:
        game = chess.pgn.read_game(stream)
        if game is None:
            break
        n_games += 1
        result = game.headers.get("Result", "*")
        if result not in ("1-0", "0-1", "1/2-1/2"):
            n_skip += 1
            continue

        # Walk the mainline collecting all plies first so we know the length
        # — this lets us trim the trailing mate-or-resignation sequence.
        moves = list(game.mainline_moves())
        if len(moves) < opening_skip + end_skip + 2:
            n_skip += 1
            continue

        board = game.board()
        last_emit_idx = len(moves) - end_skip
        for i, mv in enumerate(moves):
            board.push(mv)
            if i < opening_skip:           continue
            if i >= last_emit_idx:         break
            if board.is_check():           continue
            yield board.fen(), result
            n_pos += 1

    sys.stderr.write(
        f"parsed {n_games} games ({n_skip} skipped for no result / too short), "
        f"emitted {n_pos} positions\n"
    )


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--user", default="rpiBot73",
                    help="Lichess username to pull games from")
    ap.add_argument("--max",  type=int, default=2000,
                    help="Max games to download (Lichess caps at 300/min anon)")
    ap.add_argument("--out",  default="quiet-labeled.epd",
                    help="Output EPD file")
    args = ap.parse_args()

    pgn = fetch_pgn(args.user, args.max)
    with open(args.out, "w") as f:
        for fen, result in extract_positions(pgn):
            f.write(f'{fen} c9 "{result}";\n')

    sys.stderr.write(f"wrote {args.out}\n")


if __name__ == "__main__":
    main()
