/* Syzygy adapter: translates our Position struct (bitboards per color/piece,
   ep_sq=-1 sentinel, our piece numbering PAWN..KING=0..5) to Fathom's flat
   per-piece-type bitboards + 0-as-no-ep convention, and translates Fathom's
   WDL/move encoding back into engine-native scores and Move bits.

   Castling rights mapping is fortunately 1:1 — CASTLE_WK..CASTLE_BQ already
   match TB_CASTLING_K..TB_CASTLING_q bit-for-bit. */

#include "syzygy.h"
#include "search.h"   /* SEARCH_MATE, MATE_BOUND not exported — but SEARCH_MATE is */
#include "tbprobe.h"
#include <string.h>

/* Configured probe limit (UCI: SyzygyProbeLimit). Defaults to 7 — Fathom's
   ceiling. Effective limit at probe time is min(this, TB_LARGEST). */
static int probe_limit = 7;

/* Whether to apply the 50-move rule when interpreting WDL (UCI:
   Syzygy50MoveRule). When true, blessed-loss and cursed-win collapse to
   draw — matches what the arbiter will actually claim. */
static bool use_50_move = true;

/* Minimum remaining depth at which to probe WDL inside search (UCI:
   SyzygyProbeDepth). 0 probes everywhere (including qsearch leaves, which
   would be wasteful). Default 1 — probe at every internal node, skip qsearch. */
static int probe_depth_min = 1;

/* Most recent syzygy_probe_root outcome. Cleared on every call so the chat
   layer reads exactly the last root probe, never a stale hit from an earlier
   move. */
static bool last_root_hit  = false;
static int  last_root_band = 0;

bool syzygy_init(const char *path) {
    /* Fathom owns its own state — calling tb_init again replaces it. */
    if (!path || !*path) {
        tb_free();
        return false;
    }
    if (!tb_init(path)) return false;
    return TB_LARGEST > 0;
}

void syzygy_shutdown(void) { tb_free(); }

int syzygy_largest(void) { return (int)TB_LARGEST; }

void syzygy_set_probe_limit(int max_pieces) {
    if (max_pieces < 0) max_pieces = 0;
    if (max_pieces > 7) max_pieces = 7;
    probe_limit = max_pieces;
}
int  syzygy_get_probe_limit(void)  { return probe_limit; }
void syzygy_set_50_move_rule(bool v) { use_50_move = v; }
void syzygy_set_probe_depth(int d) {
    if (d < 0)   d = 0;
    if (d > 100) d = 100;
    probe_depth_min = d;
}
int  syzygy_get_probe_depth(void)  { return probe_depth_min; }

bool syzygy_last_root_was_hit(void) { return last_root_hit; }
int  syzygy_last_root_wdl_band(void) { return last_root_band; }

/* Build Fathom's per-type bitboards from our [color][piece] layout. */
static void build_bbs(const Position *pos,
                      uint64_t *white, uint64_t *black,
                      uint64_t *kings, uint64_t *queens, uint64_t *rooks,
                      uint64_t *bishops, uint64_t *knights, uint64_t *pawns) {
    *white   = pos->pieces[WHITE][PAWN]   | pos->pieces[WHITE][KNIGHT]
             | pos->pieces[WHITE][BISHOP] | pos->pieces[WHITE][ROOK]
             | pos->pieces[WHITE][QUEEN]  | pos->pieces[WHITE][KING];
    *black   = pos->pieces[BLACK][PAWN]   | pos->pieces[BLACK][KNIGHT]
             | pos->pieces[BLACK][BISHOP] | pos->pieces[BLACK][ROOK]
             | pos->pieces[BLACK][QUEEN]  | pos->pieces[BLACK][KING];
    *kings   = pos->pieces[WHITE][KING]   | pos->pieces[BLACK][KING];
    *queens  = pos->pieces[WHITE][QUEEN]  | pos->pieces[BLACK][QUEEN];
    *rooks   = pos->pieces[WHITE][ROOK]   | pos->pieces[BLACK][ROOK];
    *bishops = pos->pieces[WHITE][BISHOP] | pos->pieces[BLACK][BISHOP];
    *knights = pos->pieces[WHITE][KNIGHT] | pos->pieces[BLACK][KNIGHT];
    *pawns   = pos->pieces[WHITE][PAWN]   | pos->pieces[BLACK][PAWN];
}

static int piece_count(const Position *pos) {
    int n = 0;
    for (int c = 0; c < 2; c++)
        for (int p = 0; p < 6; p++)
            n += __builtin_popcountll(pos->pieces[c][p]);
    return n;
}

/* Map a Fathom WDL result to a side-to-move-POV centipawn score. We want TB
   wins/losses to dominate normal eval but stay BELOW real mate scores so
   they don't conflict with mate-distance pruning. SEARCH_MATE is 900000; we
   use SEARCH_MATE/2 ± ply-ish offsets for TB wins/losses. */
static int wdl_to_score(unsigned wdl) {
    /* Fold blessed-loss / cursed-win into draw when the 50-move rule is on. */
    if (use_50_move) {
        if (wdl == TB_BLESSED_LOSS) wdl = TB_DRAW;
        if (wdl == TB_CURSED_WIN)   wdl = TB_DRAW;
    }
    switch (wdl) {
        case TB_LOSS:         return -SEARCH_MATE / 2;
        case TB_BLESSED_LOSS: return -100;  /* "losing but drawable in 50" */
        case TB_DRAW:         return 0;
        case TB_CURSED_WIN:   return  100;
        case TB_WIN:          return  SEARCH_MATE / 2;
    }
    return 0;
}

bool syzygy_probe_wdl(const Position *pos, int *out_score) {
    if (TB_LARGEST == 0) return false;

    int n = piece_count(pos);
    if (n > (int)TB_LARGEST || n > probe_limit) return false;

    /* Fathom's WDL probe rejects positions with non-zero rule50 or any
       castling rights — bail without calling it in those cases. */
    if (pos->halfmove != 0) return false;
    if (pos->castling != 0) return false;

    uint64_t white, black, kings, queens, rooks, bishops, knights, pawns;
    build_bbs(pos, &white, &black, &kings, &queens, &rooks, &bishops, &knights,
              &pawns);
    unsigned ep = (pos->ep_sq < 0) ? 0u : (unsigned)pos->ep_sq;

    unsigned r = tb_probe_wdl(white, black, kings, queens, rooks, bishops,
                              knights, pawns, 0u, 0u, ep,
                              pos->side == WHITE);
    if (r == TB_RESULT_FAILED) return false;

    if (out_score) *out_score = wdl_to_score(r);
    return true;
}

/* Convert a Fathom-encoded move (from/to/promotes) into our Move. We resolve
   the moving piece + capture by consulting the position's bitboards. */
static Move fathom_move_to_engine(const Position *pos,
                                  int from, int to, int promotes, int is_ep) {
    /* Identify the moving piece by scanning our side's bitboards at `from`. */
    int piece = PIECE_NONE;
    for (int p = 0; p < 6; p++) {
        if (pos->pieces[pos->side][p] & SQ_BB(from)) { piece = p; break; }
    }
    if (piece == PIECE_NONE) return MOVE_NONE;

    /* Identify capture (if any) at `to` for the enemy side. */
    int cap = PIECE_NONE;
    for (int p = 0; p < 6; p++) {
        if (pos->pieces[1 - pos->side][p] & SQ_BB(to)) { cap = p; break; }
    }
    if (is_ep) cap = PAWN;

    int promo_piece;
    switch (promotes) {
        case TB_PROMOTES_QUEEN:  promo_piece = QUEEN;  break;
        case TB_PROMOTES_ROOK:   promo_piece = ROOK;   break;
        case TB_PROMOTES_BISHOP: promo_piece = BISHOP; break;
        case TB_PROMOTES_KNIGHT: promo_piece = KNIGHT; break;
        default:                 promo_piece = PIECE_NONE;
    }

    int flags = FLAG_NONE;
    if (is_ep) flags |= FLAG_EP;
    if (piece == PAWN && ((to - from == 16) || (to - from == -16)))
        flags |= FLAG_DPUSH;

    return MAKE_MOVE(from, to, piece, cap, promo_piece, flags);
}

Move syzygy_probe_root(const Position *pos, int *out_score) {
    /* Reset chat-visible flag for every call — a fresh miss must NOT see a
       stale hit from a previous move. */
    last_root_hit  = false;
    last_root_band = 0;

    if (TB_LARGEST == 0) return MOVE_NONE;

    int n = piece_count(pos);
    if (n > (int)TB_LARGEST || n > probe_limit) return MOVE_NONE;
    if (pos->castling != 0) return MOVE_NONE;

    uint64_t white, black, kings, queens, rooks, bishops, knights, pawns;
    build_bbs(pos, &white, &black, &kings, &queens, &rooks, &bishops, &knights,
              &pawns);
    unsigned ep = (pos->ep_sq < 0) ? 0u : (unsigned)pos->ep_sq;

    unsigned r = tb_probe_root(white, black, kings, queens, rooks, bishops,
                               knights, pawns,
                               (unsigned)pos->halfmove, 0u, ep,
                               pos->side == WHITE, NULL);
    if (r == TB_RESULT_FAILED)    return MOVE_NONE;
    if (r == TB_RESULT_CHECKMATE) return MOVE_NONE;
    if (r == TB_RESULT_STALEMATE) return MOVE_NONE;

    int from   = (int)TB_GET_FROM(r);
    int to     = (int)TB_GET_TO(r);
    int promo  = (int)TB_GET_PROMOTES(r);
    int is_ep  = (int)TB_GET_EP(r);
    unsigned wdl = TB_GET_WDL(r);

    Move m = fathom_move_to_engine(pos, from, to, promo, is_ep);
    if (m == MOVE_NONE) return MOVE_NONE;

    last_root_hit  = true;
    /* Collapse blessed/cursed to draw when the 50-move rule is on — chat
       should say "drawn" in that case, matching the score we hand back. */
    unsigned wdl_eff = wdl;
    if (use_50_move) {
        if (wdl_eff == TB_BLESSED_LOSS) wdl_eff = TB_DRAW;
        if (wdl_eff == TB_CURSED_WIN)   wdl_eff = TB_DRAW;
    }
    if      (wdl_eff == TB_WIN  || wdl_eff == TB_CURSED_WIN)   last_root_band = +1;
    else if (wdl_eff == TB_LOSS || wdl_eff == TB_BLESSED_LOSS) last_root_band = -1;
    else                                                       last_root_band =  0;

    if (out_score) *out_score = wdl_to_score(wdl);
    return m;
}
