/* Tapered evaluation (PeSTO-style).
   Material and PSTs come in MG/EG pairs; a phase factor [0..24] derived from
   non-pawn material on the board blends them, so the king's PST naturally
   transitions from "stay back" in the middlegame to "centralize" in the
   endgame without any explicit king-walk logic. */
#include "eval.h"
#include <string.h>

#define PAWN_HASH_SIZE 16384
typedef struct { uint64_t key; int mg; int eg; } PawnEntry;
static PawnEntry pawn_hash[PAWN_HASH_SIZE];

/* ── Material ──
   NOT static-const: mutable so the Texel tuner (src/texel.c) can refit them.
   The KING entry (index 5) stays 0 because both sides always have one — it
   would add a constant to every position and confuse the tuner. */
int MATERIAL_MG[6] = {  82, 337, 365, 477, 1025, 0 };
int MATERIAL_EG[6] = {  94, 281, 297, 512,  936, 0 };

/* ── Piece-square tables (PeSTO).
   Row 0 = rank 8 (top of board / black back rank).
   `pst_index` mirrors the rank for white so a single table serves both sides. */

/* PSTs: mutable so the Texel tuner can refit them — see eval.h. */
int PST_PAWN_MG[64] = {
      0,   0,   0,   0,   0,   0,   0,   0,
     98, 134,  61,  95,  68, 126,  34, -11,
     -6,   7,  26,  31,  65,  56,  25, -20,
    -14,  13,   6,  21,  23,  12,  17, -23,
    -27,  -2,  -5,  12,  17,   6,  10, -25,
    -26,  -4,  -4, -10,   3,   3,  33, -12,
    -35,  -1, -20, -23, -15,  24,  38, -22,
      0,   0,   0,   0,   0,   0,   0,   0,
};
int PST_PAWN_EG[64] = {
      0,   0,   0,   0,   0,   0,   0,   0,
    178, 173, 158, 134, 147, 132, 165, 187,
     94, 100,  85,  67,  56,  53,  82,  84,
     32,  24,  13,   5,  -2,   4,  17,  17,
     13,   9,  -3,  -7,  -7,  -8,   3,  -1,
      4,   7,  -6,   1,   0,  -5,  -1,  -8,
     13,   8,   8,  10,  13,   0,   2,  -7,
      0,   0,   0,   0,   0,   0,   0,   0,
};

int PST_KNIGHT_MG[64] = {
   -167, -89, -34, -49,  61, -97, -15,-107,
    -73, -41,  72,  36,  23,  62,   7, -17,
    -47,  60,  37,  65,  84, 129,  73,  44,
     -9,  17,  19,  53,  37,  69,  18,  22,
    -13,   4,  16,  13,  28,  19,  21,  -8,
    -23,  -9,  12,  10,  19,  17,  25, -16,
    -29, -53, -12,  -3,  -1,  18, -14, -19,
   -105, -21, -58, -33, -17, -28, -19, -23,
};
int PST_KNIGHT_EG[64] = {
    -58, -38, -13, -28, -31, -27, -63, -99,
    -25,  -8, -25,  -2,  -9, -25, -24, -52,
    -24, -20,  10,   9,  -1,  -9, -19, -41,
    -17,   3,  22,  22,  22,  11,   8, -18,
    -18,  -6,  16,  25,  16,  17,   4, -18,
    -23,  -3,  -1,  15,  10,  -3, -20, -22,
    -42, -20, -10,  -5,  -2, -20, -23, -44,
    -29, -51, -23, -15, -22, -18, -50, -64,
};

int PST_BISHOP_MG[64] = {
    -29,   4, -82, -37, -25, -42,   7,  -8,
    -26,  16, -18, -13,  30,  59,  18, -47,
    -16,  37,  43,  40,  35,  50,  37,  -2,
     -4,   5,  19,  50,  37,  37,   7,  -2,
     -6,  13,  13,  26,  34,  12,  10,   4,
      0,  15,  15,  15,  14,  27,  18,  10,
      4,  15,  16,   0,   7,  21,  33,   1,
    -33,  -3, -14, -21, -13, -12, -39, -21,
};
int PST_BISHOP_EG[64] = {
    -14, -21, -11,  -8,  -7,  -9, -17, -24,
     -8,  -4,   7, -12,  -3, -13,  -4, -14,
      2,  -8,   0,  -1,  -2,   6,   0,   4,
     -3,   9,  12,   9,  14,  10,   3,   2,
     -6,   3,  13,  19,   7,  10,  -3,  -9,
    -12,  -3,   8,  10,  13,   3,  -7, -15,
    -14, -18,  -7,  -1,   4,  -9, -15, -27,
    -23,  -9, -23,  -5,  -9, -16,  -5, -17,
};

int PST_ROOK_MG[64] = {
     32,  42,  32,  51,  63,   9,  31,  43,
     27,  32,  58,  62,  80,  67,  26,  44,
     -5,  19,  26,  36,  17,  45,  61,  16,
    -24, -11,   7,  26,  24,  35,  -8, -20,
    -36, -26, -12,  -1,   9,  -7,   6, -23,
    -45, -25, -16, -17,   3,   0,  -5, -33,
    -44, -16, -20,  -9,  -1,  11,  -6, -71,
    -19, -13,   1,  17,  16,   7, -37, -26,
};
int PST_ROOK_EG[64] = {
     13,  10,  18,  15,  12,  12,   8,   5,
     11,  13,  13,  11,  -3,   3,   8,   3,
      7,   7,   7,   5,   4,  -3,  -5,  -3,
      4,   3,  13,   1,   2,   1,  -1,   2,
      3,   5,   8,   4,  -5,  -6,  -8, -11,
     -4,   0,  -5,  -1,  -7, -12,  -8, -16,
     -6,  -6,   0,   2,  -9,  -9, -11,  -3,
     -9,   2,   3,  -1,  -5, -13,   4, -20,
};

int PST_QUEEN_MG[64] = {
    -28,   0,  29,  12,  59,  44,  43,  45,
    -24, -39,  -5,   1, -16,  57,  28,  54,
    -13, -17,   7,   8,  29,  56,  47,  57,
    -27, -27, -16, -16,  -1,  17,  -2,   1,
     -9, -26,  -9, -10,  -2,  -4,   3,  -3,
    -14,   2, -11,  -2,  -5,   2,  14,   5,
    -35,  -8,  11,   2,   8,  15,  -3,   1,
     -1, -18,  -9,  10, -15, -25, -31, -50,
};
int PST_QUEEN_EG[64] = {
     -9,  22,  22,  27,  27,  19,  10,  20,
    -17,  20,  32,  41,  58,  25,  30,   0,
    -20,   6,   9,  49,  47,  35,  19,   9,
      3,  22,  24,  45,  57,  40,  57,  36,
    -18,  28,  19,  47,  31,  34,  39,  23,
    -16, -27,  15,   6,   9,  17,  10,   5,
    -22, -23, -30, -16, -16, -23, -36, -32,
    -33, -28, -22, -43,  -5, -32, -20, -41,
};

int PST_KING_MG[64] = {
    -65,  23,  16, -15, -56, -34,   2,  13,
     29,  -1, -20,  -7,  -8,  -4, -38, -29,
     -9,  24,   2, -16, -20,   6,  22, -22,
    -17, -20, -12, -27, -30, -25, -14, -36,
    -49,  -1, -27, -39, -46, -44, -33, -51,
    -14, -14, -22, -46, -44, -30, -15, -27,
      1,   7,  -8, -64, -43, -16,   9,   8,
    -15,  36,  12, -54,   8, -28,  24,  14,
};
int PST_KING_EG[64] = {
    -74, -35, -18, -18, -11,  15,   4, -17,
    -12,  17,  14,  17,  17,  38,  23,  11,
     10,  17,  23,  15,  20,  45,  44,  13,
     -8,  22,  24,  27,  26,  33,  26,   3,
    -18,  -4,  21,  24,  27,  23,   9, -11,
    -19,  -3,  11,  21,  23,  16,   7,  -9,
    -27, -11,   4,  13,  14,   4,  -5, -17,
    -53, -34, -21, -11, -28, -14, -24, -43,
};

static const int *PST_MG[6] = {
    PST_PAWN_MG, PST_KNIGHT_MG, PST_BISHOP_MG, PST_ROOK_MG, PST_QUEEN_MG, PST_KING_MG
};
static const int *PST_EG[6] = {
    PST_PAWN_EG, PST_KNIGHT_EG, PST_BISHOP_EG, PST_ROOK_EG, PST_QUEEN_EG, PST_KING_EG
};

/* ── Phase ──
   Standard PeSTO weights: N=1, B=1, R=2, Q=4. Max with starting material is 24.
   We clamp to PHASE_MAX so promotion-derived extra material can't push us
   outside the interpolation range. */
static const int PHASE_INC[6] = { 0, 1, 1, 2, 4, 0 };
#define PHASE_MAX 24

static inline int pst_index(int sq, int color) {
    int file = FILE_OF(sq), rank = RANK_OF(sq);
    if (color == WHITE) rank = 7 - rank;
    return rank * 8 + file;
}

static int compute_phase(const Position *pos) {
    int p = 0;
    for (int piece = KNIGHT; piece <= QUEEN; piece++) {
        int c = popcount64(pos->pieces[WHITE][piece])
              + popcount64(pos->pieces[BLACK][piece]);
        p += c * PHASE_INC[piece];
    }
    return (p > PHASE_MAX) ? PHASE_MAX : p;
}

static uint64_t pawn_key(const Position *pos) {
    uint64_t h = 0;
    for (int c = 0; c < 2; c++) {
        uint64_t bb = pos->pieces[c][PAWN];
        while (bb) {
            int sq = lsb64(bb); bb &= bb - 1;
            h ^= ZOBRIST_PIECE[c][PAWN][sq];
        }
    }
    return h;
}

/* Pawn structure: doubled / isolated penalties and passed pawn bonus.
   Passed pawn bonus is much larger in EG since there's less material to stop
   the runner. Cached via a small pawn hash table. */
static void pawn_structure(const Position *pos, int *mg_out, int *eg_out) {
    uint64_t pk = pawn_key(pos);
    PawnEntry *pe = &pawn_hash[pk % PAWN_HASH_SIZE];
    if (pe->key == pk) {
        *mg_out = pe->mg;
        *eg_out = pe->eg;
        return;
    }

    int mg = 0, eg = 0;
    for (int color = 0; color < 2; color++) {
        int sign = (color == WHITE) ? 1 : -1;
        uint64_t pawns = pos->pieces[color][PAWN];
        uint64_t opp   = pos->pieces[1 - color][PAWN];

        uint64_t bb = pawns;
        while (bb) {
            int sq = lsb64(bb); bb &= bb-1;
            int file = FILE_OF(sq);
            int rank = RANK_OF(sq);

            if (popcount64(pawns & FILE_BB(file)) > 1) {
                mg += sign * -10;
                eg += sign * -25;
            }

            bool has_neighbor = false;
            if (file > 0 && (pawns & FILE_BB(file-1))) has_neighbor = true;
            if (file < 7 && (pawns & FILE_BB(file+1))) has_neighbor = true;
            if (!has_neighbor) {
                mg += sign * -12;
                eg += sign * -18;
            }

            uint64_t ahead_mask = 0;
            if (color == WHITE) {
                for (int r = rank+1; r < 8; r++) ahead_mask |= RANK_BB(r);
            } else {
                for (int r = rank-1; r >= 0; r--) ahead_mask |= RANK_BB(r);
            }
            uint64_t adj_files = FILE_BB(file);
            if (file > 0) adj_files |= FILE_BB(file-1);
            if (file < 7) adj_files |= FILE_BB(file+1);
            if (!(opp & adj_files & ahead_mask)) {
                /* Rank-scaled passed-pawn bonus. Index is "ranks advanced
                   from own 2nd rank" so 1=just-pushed, 6=one-square-from-
                   promotion. A 7th-rank passer in EG is ~rook-value. */
                static const int PASSED_MG[8] = { 0,  5, 10, 20,  35,  60,  90, 0 };
                static const int PASSED_EG[8] = { 0, 10, 20, 35,  60, 100, 160, 0 };
                int rel = (color == WHITE) ? rank : 7 - rank;
                mg += sign * PASSED_MG[rel];
                eg += sign * PASSED_EG[rel];
            }
        }
    }
    pe->key = pk;
    pe->mg  = mg;
    pe->eg  = eg;
    *mg_out = mg;
    *eg_out = eg;
}

/* King safety: pawns adjacent to king (pawn shield). MG only — in the
   endgame the king walks to the center and "shielding" actively hurts. */
static int king_safety_mg(const Position *pos) {
    int score = 0;
    for (int color = 0; color < 2; color++) {
        int sign = (color == WHITE) ? 1 : -1;
        int king_sq = lsb64(pos->pieces[color][KING]);
        uint64_t friendly = pos->pieces[color][PAWN];
        uint64_t shield   = KING_ATTACKS[king_sq] & friendly;
        score += sign * (int)popcount64(shield) * 10;
    }
    return score;
}

static int mobility_score(const Position *pos) {
    /* pos_mobility now returns centipawn-weighted mobility per side, so the
       eval just takes the side difference — no extra multiplier. */
    return pos_mobility(pos, WHITE) - pos_mobility(pos, BLACK);
}

int evaluate(const Position *pos) {
    int mg = 0, eg = 0;

    /* Material + PST, accumulated separately for MG and EG. */
    for (int c = 0; c < 2; c++) {
        int sign = (c == WHITE) ? 1 : -1;
        for (int p = 0; p < 6; p++) {
            uint64_t bb = pos->pieces[c][p];
            while (bb) {
                int sq = lsb64(bb); bb &= bb-1;
                int idx = pst_index(sq, c);
                mg += sign * (MATERIAL_MG[p] + PST_MG[p][idx]);
                eg += sign * (MATERIAL_EG[p] + PST_EG[p][idx]);
            }
        }
    }

    int pmg, peg;
    pawn_structure(pos, &pmg, &peg);
    mg += pmg;
    eg += peg;

    /* Bishop pair — standard PeSTO addendum, larger in EG where the two
       diagonals dominate open positions. */
    if (popcount64(pos->pieces[WHITE][BISHOP]) >= 2) { mg += 30; eg += 50; }
    if (popcount64(pos->pieces[BLACK][BISHOP]) >= 2) { mg -= 30; eg -= 50; }

    mg += king_safety_mg(pos);  /* MG-only term */

    int mob = mobility_score(pos);
    mg += mob;
    eg += mob;

    int phase = compute_phase(pos);
    int score = (mg * phase + eg * (PHASE_MAX - phase)) / PHASE_MAX;

    return (pos->side == WHITE) ? score : -score;
}
