"""Feature extraction + shared constants — the Python side of docs/nnue-format.md.

Pure-Python, zero dependencies on purpose: this module is imported by both the
PyTorch trainer (WSL, has torch) and the parity gate (Pi, may have nothing but
python3). The feature indexing here MUST match src/nnue.c exactly.
"""

# Quantization / scaling constants (mirror src/nnue.h).
N_DEFAULT = 256
N_FEATURES = 768
QA = 255
QB = 64
SCALE = 400

WHITE, BLACK = 0, 1

# FEN piece char -> (color, piece_type), piece types per board.h: P0 N1 B2 R3 Q4 K5
_PIECE_FROM_CHAR = {
    'P': (WHITE, 0), 'N': (WHITE, 1), 'B': (WHITE, 2),
    'R': (WHITE, 3), 'Q': (WHITE, 4), 'K': (WHITE, 5),
    'p': (BLACK, 0), 'n': (BLACK, 1), 'b': (BLACK, 2),
    'r': (BLACK, 3), 'q': (BLACK, 4), 'k': (BLACK, 5),
}


def parse_placement(fen):
    """Parse the placement field of a FEN into [(color, ptype, sq), ...] using
    board.h square numbering: a1=0 … h8=63 (rank-major, file within rank)."""
    placement = fen.split()[0]
    pieces = []
    rank = 7  # FEN lists rank 8 first
    for row in placement.split('/'):
        file = 0
        for ch in row:
            if ch.isdigit():
                file += int(ch)
            else:
                color, ptype = _PIECE_FROM_CHAR[ch]
                pieces.append((color, ptype, rank * 8 + file))
                file += 1
        rank -= 1
    return pieces


def feature_index(persp, color, ptype, sq):
    """Index in [0,768) for a piece, from `persp`'s point of view.
    Matches the formula in src/nnue.c::accumulate exactly."""
    rel_color = 0 if color == persp else 1
    rel_sq = sq if persp == WHITE else (sq ^ 56)
    return rel_color * 384 + ptype * 64 + rel_sq


def side_to_move(fen):
    return WHITE if fen.split()[1] == 'w' else BLACK


def active_features(fen):
    """Return (stm_feats, nstm_feats, stm): active feature-index lists from the
    side-to-move perspective and the other perspective, plus the stm color.
    The engine concatenates [crelu(stm_acc), crelu(nstm_acc)] in that order."""
    stm = side_to_move(fen)
    pieces = parse_placement(fen)
    stm_feats = [feature_index(stm, c, p, s) for (c, p, s) in pieces]
    nstm_feats = [feature_index(1 - stm, c, p, s) for (c, p, s) in pieces]
    return stm_feats, nstm_feats, stm
