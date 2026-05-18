#include "board.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* ══════════════════════════════════════════════════════════
   Zobrist keys
   ══════════════════════════════════════════════════════════ */
uint64_t ZOBRIST_PIECE[2][6][64];
uint64_t ZOBRIST_EP[8];
uint64_t ZOBRIST_CASTLE[16];
uint64_t ZOBRIST_SIDE;

static uint64_t prng_z;
static uint64_t zobrist_rand(void) {
    prng_z ^= prng_z >> 12;
    prng_z ^= prng_z << 25;
    prng_z ^= prng_z >> 27;
    return prng_z * UINT64_C(0x2545F4914F6CDD1D);
}

static void init_zobrist(void) {
    prng_z = UINT64_C(0xDEADBEEFCAFEBABE);
    for (int c = 0; c < 2; c++)
        for (int p = 0; p < 6; p++)
            for (int s = 0; s < 64; s++)
                ZOBRIST_PIECE[c][p][s] = zobrist_rand();
    for (int f = 0; f < 8; f++)
        ZOBRIST_EP[f] = zobrist_rand();
    for (int i = 0; i < 16; i++)
        ZOBRIST_CASTLE[i] = zobrist_rand();
    ZOBRIST_SIDE = zobrist_rand();
}

/* ══════════════════════════════════════════════════════════
   Precomputed attack tables
   ══════════════════════════════════════════════════════════ */
uint64_t KNIGHT_ATTACKS[64];
uint64_t KING_ATTACKS[64];
uint64_t PAWN_ATTACKS[2][64];

static void init_leaper_tables(void) {
    for (int s = 0; s < 64; s++) {
        int f = FILE_OF(s), r = RANK_OF(s);
        uint64_t n = 0, k = 0, wp = 0, bp = 0;

        /* knight: offset (+/-17/15/10/6) = 2 rank + 1 file or 1 rank + 2 file */
        if (r<6&&f<7) n|=SQ_BB(s+17); /* 2 up,   1 right */
        if (r<6&&f>0) n|=SQ_BB(s+15); /* 2 up,   1 left  */
        if (r<7&&f<6) n|=SQ_BB(s+10); /* 1 up,   2 right */
        if (r<7&&f>1) n|=SQ_BB(s+ 6); /* 1 up,   2 left  */
        if (r>0&&f<6) n|=SQ_BB(s- 6); /* 1 down, 2 right */
        if (r>0&&f>1) n|=SQ_BB(s-10); /* 1 down, 2 left  */
        if (r>1&&f<7) n|=SQ_BB(s-15); /* 2 down, 1 right */
        if (r>1&&f>0) n|=SQ_BB(s-17); /* 2 down, 1 left  */
        KNIGHT_ATTACKS[s] = n;

        /* king */
        if (r<7) k|=SQ_BB(s+8);
        if (r>0) k|=SQ_BB(s-8);
        if (f<7) k|=SQ_BB(s+1);
        if (f>0) k|=SQ_BB(s-1);
        if (r<7&&f<7) k|=SQ_BB(s+9);
        if (r<7&&f>0) k|=SQ_BB(s+7);
        if (r>0&&f<7) k|=SQ_BB(s-7);
        if (r>0&&f>0) k|=SQ_BB(s-9);
        KING_ATTACKS[s] = k;

        /* white pawn attacks */
        if (r<7&&f>0) wp|=SQ_BB(s+7);
        if (r<7&&f<7) wp|=SQ_BB(s+9);
        PAWN_ATTACKS[WHITE][s] = wp;

        /* black pawn attacks */
        if (r>0&&f>0) bp|=SQ_BB(s-9);
        if (r>0&&f<7) bp|=SQ_BB(s-7);
        PAWN_ATTACKS[BLACK][s] = bp;
    }
}

/* ══════════════════════════════════════════════════════════
   Magic bitboards
   ══════════════════════════════════════════════════════════ */
typedef struct {
    uint64_t mask;
    uint64_t magic;
    int shift;
} MagicEntry;

static MagicEntry rook_me[64];
static MagicEntry bishop_me[64];
static uint64_t   rook_tbl[64][4096];
static uint64_t   bishop_tbl[64][512];

static uint64_t prng_m;
static uint64_t magic_rand(void) {
    prng_m ^= prng_m >> 12;
    prng_m ^= prng_m << 25;
    prng_m ^= prng_m >> 27;
    return prng_m * UINT64_C(0x2545F4914F6CDD1D);
}
static uint64_t sparse_rand(void) {
    return magic_rand() & magic_rand() & magic_rand();
}

static uint64_t compute_rook_attacks(int sq, uint64_t occ) {
    uint64_t atk = 0;
    int f = FILE_OF(sq), r = RANK_OF(sq);
    for (int rr=r+1; rr<8; rr++) { int s=SQ(f,rr); atk|=SQ_BB(s); if (occ&SQ_BB(s)) break; }
    for (int rr=r-1; rr>=0;rr--) { int s=SQ(f,rr); atk|=SQ_BB(s); if (occ&SQ_BB(s)) break; }
    for (int ff=f+1; ff<8; ff++) { int s=SQ(ff,r); atk|=SQ_BB(s); if (occ&SQ_BB(s)) break; }
    for (int ff=f-1; ff>=0;ff--) { int s=SQ(ff,r); atk|=SQ_BB(s); if (occ&SQ_BB(s)) break; }
    return atk;
}

static uint64_t compute_bishop_attacks(int sq, uint64_t occ) {
    uint64_t atk = 0;
    int f = FILE_OF(sq), r = RANK_OF(sq);
    for (int ff=f+1,rr=r+1; ff<8&&rr<8; ff++,rr++) { int s=SQ(ff,rr); atk|=SQ_BB(s); if(occ&SQ_BB(s)) break; }
    for (int ff=f-1,rr=r+1; ff>=0&&rr<8; ff--,rr++) { int s=SQ(ff,rr); atk|=SQ_BB(s); if(occ&SQ_BB(s)) break; }
    for (int ff=f+1,rr=r-1; ff<8&&rr>=0; ff++,rr--) { int s=SQ(ff,rr); atk|=SQ_BB(s); if(occ&SQ_BB(s)) break; }
    for (int ff=f-1,rr=r-1; ff>=0&&rr>=0; ff--,rr--) { int s=SQ(ff,rr); atk|=SQ_BB(s); if(occ&SQ_BB(s)) break; }
    return atk;
}

static uint64_t rook_mask(int sq) {
    int f = FILE_OF(sq), r = RANK_OF(sq);
    /* rank ray excluding edge files, file ray excluding edge ranks */
    uint64_t m = (RANK_BB(r) & ~FILE_BB(0) & ~FILE_BB(7))
               | (FILE_BB(f) & ~RANK_BB(0) & ~RANK_BB(7));
    return m & ~SQ_BB(sq);
}

static uint64_t bishop_mask(int sq) {
    static const uint64_t EDGES = UINT64_C(0xFF818181818181FF);
    uint64_t m = compute_bishop_attacks(sq, 0);
    return m & ~EDGES;
}

static void init_magic_sq(int sq, bool is_rook) {
    MagicEntry *me  = is_rook ? &rook_me[sq]      : &bishop_me[sq];
    uint64_t   *tbl = is_rook ?  rook_tbl[sq]     :  bishop_tbl[sq];

    me->mask  = is_rook ? rook_mask(sq) : bishop_mask(sq);
    int n     = popcount64(me->mask);
    me->shift = 64 - n;
    int size  = 1 << n;

    uint64_t subsets[4096], atks[4096];
    int cnt = 0;
    uint64_t sub = 0;
    do {
        subsets[cnt] = sub;
        atks[cnt]    = is_rook ? compute_rook_attacks(sq, sub)
                               : compute_bishop_attacks(sq, sub);
        cnt++;
        sub = (sub - me->mask) & me->mask;
    } while (sub != 0);

    for (;;) {
        uint64_t magic = sparse_rand();
        if (popcount64((me->mask * magic) >> 56) < 6) continue;

        memset(tbl, 0, (size_t)size * sizeof(uint64_t));
        bool fail = false;
        for (int i = 0; i < cnt && !fail; i++) {
            int idx = (int)((subsets[i] * magic) >> me->shift);
            if      (tbl[idx] == 0)          tbl[idx] = atks[i];
            else if (tbl[idx] != atks[i])     fail = true;
        }
        if (!fail) { me->magic = magic; return; }
    }
}

static void init_magics(void) {
    prng_m = UINT64_C(0xFEEDFACEDEADBEEF);
    for (int s = 0; s < 64; s++) {
        init_magic_sq(s, true);
        init_magic_sq(s, false);
    }
}

static inline uint64_t rook_attacks(int sq, uint64_t occ) {
    uint64_t o = occ & rook_me[sq].mask;
    return rook_tbl[sq][(o * rook_me[sq].magic) >> rook_me[sq].shift];
}

static inline uint64_t bishop_attacks(int sq, uint64_t occ) {
    uint64_t o = occ & bishop_me[sq].mask;
    return bishop_tbl[sq][(o * bishop_me[sq].magic) >> bishop_me[sq].shift];
}

/* Prefetch the exact cache line we'd touch for rook_attacks(sq, occ). The
   index calculation is identical to the real lookup, but instead of returning
   the value we issue a non-temporal prefetch hint (locality=1: data will be
   re-used soon but isn't worth keeping in L1 long-term). Total cost is one
   speculative magic multiply per call site that uses it — paid in parallel
   with the *current* iteration's actual work. */
static inline void prefetch_rook(int sq, uint64_t occ) {
    uint64_t o = occ & rook_me[sq].mask;
    __builtin_prefetch(&rook_tbl[sq][(o * rook_me[sq].magic) >> rook_me[sq].shift], 0, 1);
}
static inline void prefetch_bishop(int sq, uint64_t occ) {
    uint64_t o = occ & bishop_me[sq].mask;
    __builtin_prefetch(&bishop_tbl[sq][(o * bishop_me[sq].magic) >> bishop_me[sq].shift], 0, 1);
}

/* ══════════════════════════════════════════════════════════
   Init
   ══════════════════════════════════════════════════════════ */
static void init_castle_mask(void);

void board_init(void) {
    init_zobrist();
    init_leaper_tables();
    init_magics();
    init_castle_mask();
}

/* ══════════════════════════════════════════════════════════
   Helpers
   ══════════════════════════════════════════════════════════ */
static uint64_t all_occ(const Position *pos) {
    return pos->pieces[0][0]|pos->pieces[0][1]|pos->pieces[0][2]
          |pos->pieces[0][3]|pos->pieces[0][4]|pos->pieces[0][5]
          |pos->pieces[1][0]|pos->pieces[1][1]|pos->pieces[1][2]
          |pos->pieces[1][3]|pos->pieces[1][4]|pos->pieces[1][5];
}

static uint64_t color_occ(const Position *pos, int c) {
    return pos->pieces[c][0]|pos->pieces[c][1]|pos->pieces[c][2]
          |pos->pieces[c][3]|pos->pieces[c][4]|pos->pieces[c][5];
}

static int piece_at(const Position *pos, int sq, int *color_out) {
    uint64_t bb = SQ_BB(sq);
    for (int c = 0; c < 2; c++)
        for (int p = 0; p < 6; p++)
            if (pos->pieces[c][p] & bb) {
                if (color_out) *color_out = c;
                return p;
            }
    return PIECE_NONE;
}

/* ══════════════════════════════════════════════════════════
   Attack detection
   ══════════════════════════════════════════════════════════ */
bool sq_attacked_by(const Position *pos, int sq, int by) {
    uint64_t occ = all_occ(pos);
    if (PAWN_ATTACKS[1-by][sq] & pos->pieces[by][PAWN])   return true;
    if (KNIGHT_ATTACKS[sq]     & pos->pieces[by][KNIGHT])  return true;
    if (KING_ATTACKS[sq]       & pos->pieces[by][KING])    return true;
    uint64_t bq = pos->pieces[by][BISHOP]|pos->pieces[by][QUEEN];
    if (bishop_attacks(sq,occ) & bq)                       return true;
    uint64_t rq = pos->pieces[by][ROOK]|pos->pieces[by][QUEEN];
    if (rook_attacks(sq,occ)   & rq)                       return true;
    return false;
}

bool pos_in_check(const Position *pos) {
    int king_sq = lsb64(pos->pieces[pos->side][KING]);
    return sq_attacked_by(pos, king_sq, 1 - pos->side);
}

int pos_mobility(const Position *pos, int color) {
    int enemy     = 1 - color;
    uint64_t occ  = all_occ(pos);
    uint64_t mine = color_occ(pos, color);

    /* Union of enemy pawn attacks — squares it would be silly to "move to". */
    uint64_t enemy_pawn_atk = 0;
    uint64_t epawns = pos->pieces[enemy][PAWN];
    while (epawns) {
        int s = lsb64(epawns); epawns &= epawns - 1;
        enemy_pawn_atk |= PAWN_ATTACKS[enemy][s];
    }
    /* Destinations that count as real mobility: not on our own pieces and
       not attacked by an enemy pawn. */
    uint64_t bad = mine | enemy_pawn_atk;

    int score = 0;

    uint64_t bb = pos->pieces[color][KNIGHT];
    while (bb) { int s = lsb64(bb); bb &= bb-1;
        score += 4 * popcount64(KNIGHT_ATTACKS[s] & ~bad); }

    bb = pos->pieces[color][BISHOP];
    while (bb) {
        int s = lsb64(bb); bb &= bb-1;
        /* Prefetch next bishop's lookup line while current one is computing. */
        if (bb) prefetch_bishop(lsb64(bb), occ);
        score += 5 * popcount64(bishop_attacks(s, occ) & ~bad);
    }

    bb = pos->pieces[color][ROOK];
    while (bb) {
        int s = lsb64(bb); bb &= bb-1;
        if (bb) prefetch_rook(lsb64(bb), occ);
        score += 2 * popcount64(rook_attacks(s, occ) & ~bad);
    }

    bb = pos->pieces[color][QUEEN];
    while (bb) {
        int s = lsb64(bb); bb &= bb-1;
        if (bb) { prefetch_bishop(lsb64(bb), occ); prefetch_rook(lsb64(bb), occ); }
        score += 1 * popcount64((bishop_attacks(s, occ) | rook_attacks(s, occ)) & ~bad);
    }

    return score;
}

/* ══════════════════════════════════════════════════════════
   Static Exchange Evaluation (SEE)
   ══════════════════════════════════════════════════════════ */

/* SEE-only piece values. Decoupled from the tapered eval material values so
   the SEE result stays stable across game phases and doesn't depend on which
   piece values eval.c happens to use today. King set very high so a king
   capture dominates the swap chain (treated as illegal via the defender
   check below). */
static const int SEE_VAL[7] = { 100, 320, 330, 500, 900, 20000, 0 };

/* Return the smallest attacker of `to` for `color`, considering only pieces
   still present in `occ`. Slider attacks are recomputed against `occ`, which
   naturally handles x-rays that emerge as front-line pieces are removed
   during the swap chain. */
static int smallest_attacker(const Position *pos, int to, int color,
                             uint64_t occ, int *out_from) {
    uint64_t a;

    a = PAWN_ATTACKS[1 - color][to] & pos->pieces[color][PAWN] & occ;
    if (a) { *out_from = lsb64(a); return PAWN; }

    a = KNIGHT_ATTACKS[to] & pos->pieces[color][KNIGHT] & occ;
    if (a) { *out_from = lsb64(a); return KNIGHT; }

    uint64_t diag = bishop_attacks(to, occ);
    a = diag & pos->pieces[color][BISHOP] & occ;
    if (a) { *out_from = lsb64(a); return BISHOP; }

    uint64_t orth = rook_attacks(to, occ);
    a = orth & pos->pieces[color][ROOK] & occ;
    if (a) { *out_from = lsb64(a); return ROOK; }

    a = (diag | orth) & pos->pieces[color][QUEEN] & occ;
    if (a) { *out_from = lsb64(a); return QUEEN; }

    a = KING_ATTACKS[to] & pos->pieces[color][KING] & occ;
    if (a) { *out_from = lsb64(a); return KING; }

    return PIECE_NONE;
}

int pos_see(const Position *pos, Move m) {
    int from   = MOVE_FROM(m);
    int to     = MOVE_TO(m);
    int piece  = MOVE_PIECE(m);
    int cap    = MOVE_CAPTURE(m);
    int promo  = MOVE_PROMO(m);
    int flags  = MOVE_FLAGS(m);

    if (cap == PIECE_NONE && promo == PIECE_NONE && !(flags & FLAG_EP))
        return 0;

    /* Initial gain = value of the piece captured (PAWN for EP), plus any
       promotion bonus on the move itself. The attacker landing on `to` is
       valued as the promoted piece if applicable. */
    int target_val;
    if (flags & FLAG_EP)            target_val = SEE_VAL[PAWN];
    else if (cap != PIECE_NONE)     target_val = SEE_VAL[cap];
    else                            target_val = 0;

    int attacker_val;
    int gain[32];
    int d = 0;

    if (promo != PIECE_NONE) {
        gain[0]      = target_val + SEE_VAL[promo] - SEE_VAL[PAWN];
        attacker_val = SEE_VAL[promo];
    } else {
        gain[0]      = target_val;
        attacker_val = SEE_VAL[piece];
    }

    uint64_t occ = all_occ(pos) ^ SQ_BB(from);
    if (flags & FLAG_EP) {
        int ep_cap = to + ((pos->side == WHITE) ? -8 : 8);
        occ ^= SQ_BB(ep_cap);
    }

    int side = 1 - pos->side;

    while (1) {
        int from_sq;
        int atk_piece = smallest_attacker(pos, to, side, occ, &from_sq);
        if (atk_piece == PIECE_NONE) break;

        /* Refuse to capture with the king if any defender remains — the king
           would be moving into check, so that branch of the chain is illegal. */
        if (atk_piece == KING) {
            int dummy;
            uint64_t after = occ ^ SQ_BB(from_sq);
            if (smallest_attacker(pos, to, 1 - side, after, &dummy) != PIECE_NONE)
                break;
        }

        d++;
        if (d >= 32) break;  /* defensive: 32 captures on one square never happens */

        int new_attacker_val = SEE_VAL[atk_piece];
        if (atk_piece == PAWN && (RANK_OF(to) == 0 || RANK_OF(to) == 7)) {
            /* In-chain promotion: assume queen, the standard simplification. */
            gain[d] = attacker_val - gain[d-1] + SEE_VAL[QUEEN] - SEE_VAL[PAWN];
            new_attacker_val = SEE_VAL[QUEEN];
        } else {
            gain[d] = attacker_val - gain[d-1];
        }

        attacker_val = new_attacker_val;
        occ ^= SQ_BB(from_sq);
        side = 1 - side;
    }

    /* Negamax over the gain stack: each side, at their decision point,
       chooses between continuing the chain or refusing it (zero net).
       gain[d-1] = -max(-gain[d-1], gain[d]). */
    while (d > 0) {
        int a = -gain[d - 1];
        int b =  gain[d];
        int best = (a > b) ? a : b;
        gain[d - 1] = -best;
        d--;
    }
    return gain[0];
}

/* ══════════════════════════════════════════════════════════
   Castling rights table
   ══════════════════════════════════════════════════════════ */
static int CASTLE_MASK[64]; /* AND mask applied to castling rights on move */

static void init_castle_mask(void) {
    for (int s = 0; s < 64; s++) CASTLE_MASK[s] = 15;
    CASTLE_MASK[E1] &= ~(CASTLE_WK|CASTLE_WQ);
    CASTLE_MASK[A1] &= ~CASTLE_WQ;
    CASTLE_MASK[H1] &= ~CASTLE_WK;
    CASTLE_MASK[E8] &= ~(CASTLE_BK|CASTLE_BQ);
    CASTLE_MASK[A8] &= ~CASTLE_BQ;
    CASTLE_MASK[H8] &= ~CASTLE_BK;
}

/* ══════════════════════════════════════════════════════════
   do_move
   ══════════════════════════════════════════════════════════ */
void pos_do_move(const Position *src, Move m, Position *dst) {
    *dst = *src;

    int from  = MOVE_FROM(m);
    int to    = MOVE_TO(m);
    int piece = MOVE_PIECE(m);
    int cap   = MOVE_CAPTURE(m);
    int promo = MOVE_PROMO(m);
    int flags = MOVE_FLAGS(m);
    int us    = src->side;
    int them  = 1 - us;

    /* remove from 'from' */
    dst->pieces[us][piece] ^= SQ_BB(from);
    dst->hash ^= ZOBRIST_PIECE[us][piece][from];

    /* captures */
    if (flags & FLAG_EP) {
        int ep_cap = to + (us == WHITE ? -8 : 8);
        dst->pieces[them][PAWN] ^= SQ_BB(ep_cap);
        dst->hash ^= ZOBRIST_PIECE[them][PAWN][ep_cap];
    } else if (cap != PIECE_NONE) {
        dst->pieces[them][cap] ^= SQ_BB(to);
        dst->hash ^= ZOBRIST_PIECE[them][cap][to];
    }

    /* place on 'to' */
    int placed = (promo != PIECE_NONE) ? promo : piece;
    dst->pieces[us][placed] ^= SQ_BB(to);
    dst->hash ^= ZOBRIST_PIECE[us][placed][to];

    /* castling: move rook */
    if (flags & FLAG_CASTLE) {
        int rf, rt;
        if      (to == G1) { rf = H1; rt = F1; }
        else if (to == C1) { rf = A1; rt = D1; }
        else if (to == G8) { rf = H8; rt = F8; }
        else               { rf = A8; rt = D8; }
        dst->pieces[us][ROOK] ^= SQ_BB(rf)|SQ_BB(rt);
        dst->hash ^= ZOBRIST_PIECE[us][ROOK][rf] ^ ZOBRIST_PIECE[us][ROOK][rt];
    }

    /* en passant square */
    if (dst->ep_sq >= 0) {
        dst->hash ^= ZOBRIST_EP[FILE_OF(dst->ep_sq)];
        dst->ep_sq = -1;
    }
    if (flags & FLAG_DPUSH) {
        dst->ep_sq = (from + to) / 2;
        dst->hash ^= ZOBRIST_EP[FILE_OF(dst->ep_sq)];
    }

    /* castling rights */
    dst->hash ^= ZOBRIST_CASTLE[dst->castling];
    dst->castling &= CASTLE_MASK[from] & CASTLE_MASK[to];
    dst->hash ^= ZOBRIST_CASTLE[dst->castling];

    /* clocks */
    dst->halfmove = (piece == PAWN || cap != PIECE_NONE) ? 0 : dst->halfmove + 1;
    if (us == BLACK) dst->fullmove++;

    dst->side  = them;
    dst->hash ^= ZOBRIST_SIDE;
}

/* ══════════════════════════════════════════════════════════
   Pseudo-legal move generation
   ══════════════════════════════════════════════════════════ */
static int gen_pseudo(const Position *pos, Move *moves, bool caps_only) {
    int cnt = 0;
    int us = pos->side, them = 1 - us;
    uint64_t occ  = all_occ(pos);
    uint64_t mine = color_occ(pos, us);
    uint64_t theirs = color_occ(pos, them);
    uint64_t targets = caps_only ? theirs : ~mine;

#define PUSH(from,to,pc,cap,promo,flags) \
    moves[cnt++] = MAKE_MOVE(from,to,pc,cap,promo,flags)

    /* Pawns */
    {
        uint64_t pawns = pos->pieces[us][PAWN];
        int fwd  = (us == WHITE) ?  8 : -8;
        int rank7 = (us == WHITE) ?  6 :  1; /* rank where pawns promote */
        int rank2 = (us == WHITE) ?  1 :  6; /* rank for double push */

        uint64_t bb = pawns;
        while (bb) {
            int from = lsb64(bb); bb &= bb-1;
            int fr   = RANK_OF(from);

            /* single push */
            int to1 = from + fwd;
            if (!(occ & SQ_BB(to1))) {
                if (fr == rank7) {
                    /* Promotions are tactical — always generate. In qsearch
                       limit to queen promo to keep the q-tree manageable. */
                    if (caps_only) {
                        PUSH(from,to1,PAWN,PIECE_NONE,QUEEN,FLAG_NONE);
                    } else {
                        for (int pp = KNIGHT; pp <= QUEEN; pp++)
                            PUSH(from,to1,PAWN,PIECE_NONE,pp,FLAG_NONE);
                    }
                } else if (!caps_only) {
                    PUSH(from,to1,PAWN,PIECE_NONE,PIECE_NONE,FLAG_NONE);
                    /* double push */
                    int to2 = from + 2*fwd;
                    if (fr == rank2 && !(occ & SQ_BB(to2)))
                        PUSH(from,to2,PAWN,PIECE_NONE,PIECE_NONE,FLAG_DPUSH);
                }
            }

            /* captures */
            uint64_t atk = PAWN_ATTACKS[us][from] & theirs;
            while (atk) {
                int to = lsb64(atk); atk &= atk-1;
                int cap = piece_at(pos, to, NULL);
                if (fr == rank7) {
                    if (caps_only) {
                        PUSH(from,to,PAWN,cap,QUEEN,FLAG_NONE);
                    } else {
                        for (int pp = KNIGHT; pp <= QUEEN; pp++)
                            PUSH(from,to,PAWN,cap,pp,FLAG_NONE);
                    }
                } else {
                    PUSH(from,to,PAWN,cap,PIECE_NONE,FLAG_NONE);
                }
            }

            /* en passant */
            if (pos->ep_sq >= 0 && (PAWN_ATTACKS[us][from] & SQ_BB(pos->ep_sq))) {
                PUSH(from,pos->ep_sq,PAWN,PAWN,PIECE_NONE,FLAG_EP);
            }
        }
    }

    /* Guarded mailbox lookup: skip the call on empty targets (the dominant
       case during non-capture move gen). Mailbox makes piece_at O(1), but
       gen_pseudo still expands many non-capture targets, so dodging the call
       is cheaper than doing it on empty squares too. */
#define CAP_AT(to_)  (((uint64_t)1 << (to_)) & theirs ? piece_at(pos, (to_), NULL) : PIECE_NONE)

    /* Knights */
    {
        uint64_t bb = pos->pieces[us][KNIGHT];
        while (bb) {
            int from = lsb64(bb); bb &= bb-1;
            uint64_t atk = KNIGHT_ATTACKS[from] & targets;
            while (atk) {
                int to = lsb64(atk); atk &= atk-1;
                PUSH(from,to,KNIGHT,CAP_AT(to),PIECE_NONE,FLAG_NONE);
            }
        }
    }

    /* Bishops */
    {
        uint64_t bb = pos->pieces[us][BISHOP];
        while (bb) {
            int from = lsb64(bb); bb &= bb-1;
            if (bb) prefetch_bishop(lsb64(bb), occ);
            uint64_t atk = bishop_attacks(from,occ) & targets;
            while (atk) {
                int to = lsb64(atk); atk &= atk-1;
                PUSH(from,to,BISHOP,CAP_AT(to),PIECE_NONE,FLAG_NONE);
            }
        }
    }

    /* Rooks */
    {
        uint64_t bb = pos->pieces[us][ROOK];
        while (bb) {
            int from = lsb64(bb); bb &= bb-1;
            if (bb) prefetch_rook(lsb64(bb), occ);
            uint64_t atk = rook_attacks(from,occ) & targets;
            while (atk) {
                int to = lsb64(atk); atk &= atk-1;
                PUSH(from,to,ROOK,CAP_AT(to),PIECE_NONE,FLAG_NONE);
            }
        }
    }

    /* Queens */
    {
        uint64_t bb = pos->pieces[us][QUEEN];
        while (bb) {
            int from = lsb64(bb); bb &= bb-1;
            if (bb) { prefetch_bishop(lsb64(bb), occ); prefetch_rook(lsb64(bb), occ); }
            uint64_t atk = (bishop_attacks(from,occ)|rook_attacks(from,occ)) & targets;
            while (atk) {
                int to = lsb64(atk); atk &= atk-1;
                PUSH(from,to,QUEEN,CAP_AT(to),PIECE_NONE,FLAG_NONE);
            }
        }
    }

    /* King */
    {
        int from = lsb64(pos->pieces[us][KING]);
        uint64_t atk = KING_ATTACKS[from] & targets;
        while (atk) {
            int to = lsb64(atk); atk &= atk-1;
            PUSH(from,to,KING,CAP_AT(to),PIECE_NONE,FLAG_NONE);
        }

        /* Castling (not in caps_only) */
        if (!caps_only) {
            if (us == WHITE) {
                if ((pos->castling & CASTLE_WK) &&
                    !(occ & (SQ_BB(F1)|SQ_BB(G1))) &&
                    !sq_attacked_by(pos,E1,BLACK) &&
                    !sq_attacked_by(pos,F1,BLACK) &&
                    !sq_attacked_by(pos,G1,BLACK))
                    PUSH(E1,G1,KING,PIECE_NONE,PIECE_NONE,FLAG_CASTLE);
                if ((pos->castling & CASTLE_WQ) &&
                    !(occ & (SQ_BB(B1)|SQ_BB(C1)|SQ_BB(D1))) &&
                    !sq_attacked_by(pos,E1,BLACK) &&
                    !sq_attacked_by(pos,D1,BLACK) &&
                    !sq_attacked_by(pos,C1,BLACK))
                    PUSH(E1,C1,KING,PIECE_NONE,PIECE_NONE,FLAG_CASTLE);
            } else {
                if ((pos->castling & CASTLE_BK) &&
                    !(occ & (SQ_BB(F8)|SQ_BB(G8))) &&
                    !sq_attacked_by(pos,E8,WHITE) &&
                    !sq_attacked_by(pos,F8,WHITE) &&
                    !sq_attacked_by(pos,G8,WHITE))
                    PUSH(E8,G8,KING,PIECE_NONE,PIECE_NONE,FLAG_CASTLE);
                if ((pos->castling & CASTLE_BQ) &&
                    !(occ & (SQ_BB(B8)|SQ_BB(C8)|SQ_BB(D8))) &&
                    !sq_attacked_by(pos,E8,WHITE) &&
                    !sq_attacked_by(pos,D8,WHITE) &&
                    !sq_attacked_by(pos,C8,WHITE))
                    PUSH(E8,C8,KING,PIECE_NONE,PIECE_NONE,FLAG_CASTLE);
            }
        }
    }
#undef PUSH
#undef CAP_AT
    return cnt;
}

/* Filter pseudo-legal moves for legality */
static int filter_legal(const Position *pos, Move *pseudo, int n, Move *out) {
    int cnt = 0;
    for (int i = 0; i < n; i++) {
        Position after;
        pos_do_move(pos, pseudo[i], &after);
        int king_sq = lsb64(after.pieces[pos->side][KING]);
        if (!sq_attacked_by(&after, king_sq, after.side))
            out[cnt++] = pseudo[i];
    }
    return cnt;
}

int pos_gen_moves(const Position *pos, Move *moves) {
    Move pseudo[MAX_MOVES];
    int n = gen_pseudo(pos, pseudo, false);
    return filter_legal(pos, pseudo, n, moves);
}

int pos_gen_captures(const Position *pos, Move *moves) {
    Move pseudo[MAX_MOVES];
    int n = gen_pseudo(pos, pseudo, true);
    return filter_legal(pos, pseudo, n, moves);
}

int pos_gen_pseudo_captures(const Position *pos, Move *moves) {
    return gen_pseudo(pos, moves, true);
}

/* ══════════════════════════════════════════════════════════
   Position setup
   ══════════════════════════════════════════════════════════ */
static uint64_t compute_hash(const Position *pos) {
    uint64_t h = 0;
    for (int c = 0; c < 2; c++)
        for (int p = 0; p < 6; p++) {
            uint64_t bb = pos->pieces[c][p];
            while (bb) { int s = lsb64(bb); bb &= bb-1; h ^= ZOBRIST_PIECE[c][p][s]; }
        }
    if (pos->ep_sq >= 0) h ^= ZOBRIST_EP[FILE_OF(pos->ep_sq)];
    h ^= ZOBRIST_CASTLE[pos->castling];
    if (pos->side == BLACK) h ^= ZOBRIST_SIDE;
    return h;
}

void pos_startpos(Position *pos) {
    memset(pos, 0, sizeof(*pos));
    /* White pieces */
    pos->pieces[WHITE][PAWN]   = RANK_BB(1);
    pos->pieces[WHITE][KNIGHT] = SQ_BB(B1)|SQ_BB(G1);
    pos->pieces[WHITE][BISHOP] = SQ_BB(C1)|SQ_BB(F1);
    pos->pieces[WHITE][ROOK]   = SQ_BB(A1)|SQ_BB(H1);
    pos->pieces[WHITE][QUEEN]  = SQ_BB(D1);
    pos->pieces[WHITE][KING]   = SQ_BB(E1);
    /* Black pieces */
    pos->pieces[BLACK][PAWN]   = RANK_BB(6);
    pos->pieces[BLACK][KNIGHT] = SQ_BB(B8)|SQ_BB(G8);
    pos->pieces[BLACK][BISHOP] = SQ_BB(C8)|SQ_BB(F8);
    pos->pieces[BLACK][ROOK]   = SQ_BB(A8)|SQ_BB(H8);
    pos->pieces[BLACK][QUEEN]  = SQ_BB(D8);
    pos->pieces[BLACK][KING]   = SQ_BB(E8);

    pos->side     = WHITE;
    pos->ep_sq    = -1;
    pos->castling = CASTLE_WK|CASTLE_WQ|CASTLE_BK|CASTLE_BQ;
    pos->halfmove = 0;
    pos->fullmove = 1;
    pos->hash     = compute_hash(pos);
}

bool pos_from_fen(Position *pos, const char *fen) {
    memset(pos, 0, sizeof(*pos));
    pos->ep_sq = -1;

    /* piece placement */
    int sq = A8; /* FEN starts at a8 */
    while (*fen && *fen != ' ') {
        char c = *fen++;
        if (c == '/') { sq -= 16; continue; } /* next rank down */
        if (c >= '1' && c <= '8') { sq += c-'0'; continue; }
        static const char PIECES[] = "PNBRQKpnbrqk";
        const char *p = strchr(PIECES, c);
        if (!p) return false;
        int idx = (int)(p - PIECES);
        int color = idx >= 6 ? BLACK : WHITE;
        int piece = idx % 6;
        pos->pieces[color][piece] |= SQ_BB(sq);
        sq++;
    }
    if (*fen) fen++;

    /* side to move */
    pos->side = (*fen == 'b') ? BLACK : WHITE;
    while (*fen && *fen != ' ') fen++;
    if (*fen) fen++;

    /* castling */
    while (*fen && *fen != ' ') {
        switch (*fen++) {
            case 'K': pos->castling |= CASTLE_WK; break;
            case 'Q': pos->castling |= CASTLE_WQ; break;
            case 'k': pos->castling |= CASTLE_BK; break;
            case 'q': pos->castling |= CASTLE_BQ; break;
        }
    }
    if (*fen) fen++;

    /* en passant */
    if (*fen && *fen != '-') {
        int f = fen[0]-'a', r = fen[1]-'1';
        if (f < 0 || f > 7 || (r != 2 && r != 5)) return false;
        pos->ep_sq = SQ(f, r);
        while (*fen && *fen != ' ') fen++;
    } else { while (*fen && *fen != ' ') fen++; }
    if (*fen) fen++;

    /* halfmove */
    pos->halfmove = atoi(fen);
    while (*fen && *fen != ' ') fen++;
    if (*fen) fen++;

    /* fullmove */
    pos->fullmove = atoi(fen);
    if (pos->fullmove < 1) pos->fullmove = 1;

    /* Sanity: exactly one king per side. Without this, lsb64(0) is UB
       and later calls (pos_in_check, eval) crash on malformed input. */
    if (popcount64(pos->pieces[WHITE][KING]) != 1 ||
        popcount64(pos->pieces[BLACK][KING]) != 1)
        return false;

    pos->hash = compute_hash(pos);
    return true;
}

/* ══════════════════════════════════════════════════════════
   UCI move encoding
   ══════════════════════════════════════════════════════════ */
void move_to_uci(Move m, char buf[6]) {
    int from = MOVE_FROM(m), to = MOVE_TO(m);
    buf[0] = 'a' + FILE_OF(from);
    buf[1] = '1' + RANK_OF(from);
    buf[2] = 'a' + FILE_OF(to);
    buf[3] = '1' + RANK_OF(to);
    int promo = MOVE_PROMO(m);
    if (promo != PIECE_NONE) {
        static const char PROMO_CH[] = "pnbrqk";
        buf[4] = PROMO_CH[promo];
        buf[5] = '\0';
    } else {
        buf[4] = '\0';
    }
}

Move move_from_uci(const Position *pos, const char *s) {
    if (!s || strlen(s) < 4) return MOVE_NONE;
    int from = SQ(s[0]-'a', s[1]-'1');
    int to   = SQ(s[2]-'a', s[3]-'1');
    int promo_req = PIECE_NONE;
    if (s[4]) {
        static const char *PC = "pnbrqk";
        char c = s[4];
        if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
        const char *p = strchr(PC, c);
        if (p) promo_req = (int)(p - PC);
    }

    /* Generate legal moves and find the one matching from/to/promo */
    Move legal[MAX_MOVES];
    int n = pos_gen_moves(pos, legal);
    for (int i = 0; i < n; i++) {
        if (MOVE_FROM(legal[i]) == from && MOVE_TO(legal[i]) == to) {
            int mp = MOVE_PROMO(legal[i]);
            if (promo_req == PIECE_NONE || mp == promo_req)
                return legal[i];
        }
    }
    return MOVE_NONE;
}
