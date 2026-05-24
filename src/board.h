#pragma once
#include <stdint.h>
#include <stdbool.h>

/* ── Piece types ── */
#define PAWN   0
#define KNIGHT 1
#define BISHOP 2
#define ROOK   3
#define QUEEN  4
#define KING   5
#define PIECE_NONE 6

/* ── Colors ── */
#define WHITE 0
#define BLACK 1

/* ── Castling rights bitmask ── */
#define CASTLE_WK 1
#define CASTLE_WQ 2
#define CASTLE_BK 4
#define CASTLE_BQ 8

/* ── Square numbering: a1=0 … h8=63 ── */
#define SQ(f,r)   ((r)*8+(f))
#define FILE_OF(s) ((s)&7)
#define RANK_OF(s) ((s)>>3)
#define SQ_BB(s)   (UINT64_C(1)<<(s))

#define FILE_BB(f) (UINT64_C(0x0101010101010101)<<(f))
#define RANK_BB(r) (UINT64_C(0xFF)<<((r)*8))

/* Named squares */
enum {
    A1,B1,C1,D1,E1,F1,G1,H1,
    A2,B2,C2,D2,E2,F2,G2,H2,
    A3,B3,C3,D3,E3,F3,G3,H3,
    A4,B4,C4,D4,E4,F4,G4,H4,
    A5,B5,C5,D5,E5,F5,G5,H5,
    A6,B6,C6,D6,E6,F6,G6,H6,
    A7,B7,C7,D7,E7,F7,G7,H7,
    A8,B8,C8,D8,E8,F8,G8,H8,
};

/* ── Move encoding (32-bit packed) ──
   bits  0- 5  from square
   bits  6-11  to square
   bits 12-14  piece type (PAWN..KING)
   bits 15-17  captured piece (PAWN..KING, or PIECE_NONE=6)
   bits 18-20  promotion piece (PAWN..KING, or PIECE_NONE=6)
   bits 21-23  flags
*/
typedef uint32_t Move;

#define FLAG_NONE   0
#define FLAG_EP     1
#define FLAG_CASTLE 2
#define FLAG_DPUSH  4

#define MOVE_FROM(m)    ((int)((m)&0x3F))
#define MOVE_TO(m)      ((int)(((m)>>6)&0x3F))
#define MOVE_PIECE(m)   ((int)(((m)>>12)&0x7))
#define MOVE_CAPTURE(m) ((int)(((m)>>15)&0x7))
#define MOVE_PROMO(m)   ((int)(((m)>>18)&0x7))
#define MOVE_FLAGS(m)   ((int)(((m)>>21)&0x7))

#define MAKE_MOVE(from,to,pc,cap,promo,flags) \
    ((Move)((from)|((to)<<6)|((pc)<<12)|((cap)<<15)|((promo)<<18)|((flags)<<21)))

#define MOVE_NONE ((Move)0)
#define MAX_MOVES 256

/* ── Position ── */
typedef struct {
    uint64_t pieces[2][6]; /* [color][piece] */
    int      side;         /* WHITE or BLACK */
    int      ep_sq;        /* -1 = none */
    int      castling;     /* CASTLE_* bitmask */
    int      halfmove;
    int      fullmove;
    uint64_t hash;         /* Zobrist hash */
} Position;

/* ── Init (call once at startup) ── */
void board_init(void);

/* ── Position setup ── */
void pos_startpos(Position *pos);
bool pos_from_fen(Position *pos, const char *fen);

/* ── Move application ── */
void pos_do_move(const Position *src, Move m, Position *dst);

/* ── Move generation ── */
int pos_gen_moves(const Position *pos, Move *moves);    /* legal */
int pos_gen_captures(const Position *pos, Move *moves); /* legal captures */
/* Pseudo-legal captures (no legality filter). Used by qsearch where
   SEE-pruning can drop ~half of captures before paying the legality cost;
   the caller must verify legality (e.g., via sq_attacked_by on the king
   after pos_do_move) for the survivors. */
int pos_gen_pseudo_captures(const Position *pos, Move *moves);

/* ── Queries ── */
bool pos_in_check(const Position *pos);                 /* is side-to-move in check? */
bool sq_attacked_by(const Position *pos, int sq, int by_color);
/* Mobility evaluation for `color`, returned in centipawn-scaled units (the
   caller subtracts the two colors and adds directly to the score — no extra
   weighting). Per-piece weights (N=4, B=5, R=2, Q=1) reflect that 1 square of
   queen mobility is worth less than 1 square of knight mobility (queens
   already have huge raw reach). Squares attacked by enemy pawns are excluded
   from the destination mask — pieces don't gain real mobility from squares
   they'd just lose tempo on. King and pawns are excluded. */
int  pos_mobility(const Position *pos, int color);

/* Static Exchange Evaluation for a capture move on `pos`. Returns the net
   material balance (centipawns, side-to-move POV) of the optimal sequence
   of captures on the target square, assuming both sides play the cheapest
   available attacker. Handles x-rays, en passant, in-chain promotions, and
   refuses king captures that would walk into a defender.
   For non-capture / non-promotion moves, returns 0. */
int  pos_see(const Position *pos, Move m);

/* ── UCI move encoding ── */
Move move_from_uci(const Position *pos, const char *s);
void move_to_uci(Move m, char buf[6]);

/* ── Bit utilities ── */
static inline int popcount64(uint64_t b) { return __builtin_popcountll(b); }
static inline int lsb64(uint64_t b)      { return __builtin_ctzll(b); }
static inline uint64_t pop_lsb(uint64_t *b) {
    uint64_t bit = *b & -(*b);
    *b &= *b - 1;
    return bit;
}

/* ── Zobrist keys (for TT) ── */
extern uint64_t ZOBRIST_PIECE[2][6][64];
extern uint64_t ZOBRIST_EP[8];
extern uint64_t ZOBRIST_CASTLE[16];
extern uint64_t ZOBRIST_SIDE;

/* ── Precomputed attack tables ── */
extern uint64_t KNIGHT_ATTACKS[64];
extern uint64_t KING_ATTACKS[64];
extern uint64_t PAWN_ATTACKS[2][64];

/* ── Slider attack lookups (magic bitboards) ── */
uint64_t board_bishop_attacks(int sq, uint64_t occ);
uint64_t board_rook_attacks(int sq, uint64_t occ);
