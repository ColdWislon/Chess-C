/* Search: iterative deepening, alpha-beta with PVS, null-move pruning, LMR,
   killers, history, aspiration windows, check extensions, repetition / 50-move.
   Optional Lazy SMP: N threads run independent ID from root, sharing the TT. */
#define _POSIX_C_SOURCE 200809L
#include "search.h"
#include "eval.h"
#include "syzygy.h"
#include "nnue.h"
#include <time.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <pthread.h>
#include <math.h>

#define MAX_PLY      64
#define HISTORY_MAX  16384
#define MATE_BOUND   (SEARCH_MATE - 1000)

/* LMR reduction lookup, indexed [depth][moves_searched]. Populated by
   search_init() with the 0.5 + log(d)*log(m)/1.75 formula — smoothly
   reduces more at higher depth and later move number than the old constant-
   plus-step formula did. Entries are clamped non-negative; the move-loop
   later clamps to <= depth-1 to keep the reduced search at depth >= 1. */
static int LMR[64][64];

typedef struct {
    uint64_t nodes;
    uint64_t qnodes;        /* nodes inside quiescence */
    uint64_t tt_probes;
    uint64_t tt_hits;
    uint64_t cutoffs;       /* total beta cutoffs */
    uint64_t cutoffs_first; /* cutoffs that happened on first move (ordering quality) */
    uint64_t nullmove_cuts; /* successful null-move prunes */
    uint64_t tb_hits;       /* successful Syzygy WDL probes inside search */
    int      seldepth;      /* deepest ply reached (including qsearch) */

    /* ── Tuning probes (PROF info-line, see emit_prof_line). Each counts how
       often a specific pruning/reduction technique fired. Ratios reveal which
       knob is poorly calibrated:
         lmr_researched / lmr_done   — high → LMR too aggressive
         nullmove_cuts / nmp_attempts — low  → NMP wasting work
         rfp_prunes / nodes           — sanity check RFP isn't dead code
         futility_prunes / nodes      — same for frontier futility
         lmp_prunes / nodes           — same for late-move pruning
         see_qprunes / qnodes         — SEE pruning effectiveness in qsearch
         asp_widens / iterations      — aspiration windows too tight if > 1 */
    uint64_t lmr_done;       /* moves searched at reduced depth */
    uint64_t lmr_researched; /* subset that beat alpha and needed full-depth research */
    uint64_t nmp_attempts;   /* null-move probes tried */
    uint64_t rfp_prunes;     /* reverse-futility / static null fires */
    uint64_t futility_prunes;/* frontier futility skips */
    uint64_t lmp_prunes;     /* late-move pruning skips */
    uint64_t see_qprunes;    /* qsearch SEE-based capture skips */
    uint64_t asp_widens;     /* aspiration window widenings this search */

    bool     stopped;

    /* Shared deadline + stop flag for this search. All workers point at the
       same SearchControl; the deadline is read atomically so the UCI layer can
       extend it mid-search on `ponderhit`, and `stop` ends all workers at the
       next check_time poll. Never NULL. */
    SearchControl *ctl;

    Move     killers[MAX_PLY][2];
    int      history[2][64][64];   /* [color][from][to] */

    /* Counter-move heuristic: counter_moves[prev_from][prev_to] is the quiet
       move that most recently produced a beta-cutoff in reply to a move with
       those endpoints. Indexed by the opponent's move (not piece-keyed —
       simpler and good enough; piece info is recoverable from the position
       if we ever want to switch). */
    Move     counter_moves[64][64];

    uint64_t rep_stack[1024];
    int      rep_top;

    Move     root_best;

    int      eval_stack[MAX_PLY + 1];

    /* Per-ply NNUE accumulator stack. Refreshed at the root, then advanced
       incrementally on each pos_do_move via search_do_move(). Active only
       when nnue_is_loaded(); when no net is loaded the stack is unused.
       Sized MAX_PLY+2 so the ply+1 write at the deepest legal ply is in
       bounds (matches the pv buffer's headroom pattern). */
    NNUEAcc  acc_stack[MAX_PLY + 2];

    /* Principal Variation: pv[ply] is the best line found from that ply
       onward, length pv_len[ply]. After a PV node selects move m, we copy
       child's pv into ours after the leading m. Sized MAX_PLY+1 so that the
       `ply+1` lookup in pv_copy is in bounds even at the deepest leaf. */
    Move     pv[MAX_PLY + 1][MAX_PLY];
    int      pv_len[MAX_PLY + 1];
} SearchCtx;

/* Wrapper that does pos_do_move and keeps the per-ply accumulator stack in
   sync. ply is the current ply BEFORE the move (so the child runs at ply+1
   and reads acc_stack[ply+1]). */
static inline void search_do_move(SearchCtx *ctx, int ply,
                                  const Position *pos, Move m, Position *child) {
    pos_do_move(pos, m, child);
    if (nnue_is_loaded() && ply + 1 < MAX_PLY + 2)
        nnue_acc_advance(&ctx->acc_stack[ply + 1],
                         &ctx->acc_stack[ply], pos, m);
}

/* Search hot-path eval. Uses the cached per-ply accumulator when NNUE is
   loaded — O(N) dot product, no per-piece sum — falls back to the hand-
   crafted eval() otherwise. */
static inline int search_eval(SearchCtx *ctx, int ply, const Position *pos) {
    if (nnue_is_loaded())
        return nnue_evaluate_acc(&ctx->acc_stack[ply], pos->side);
    return evaluate(pos);
}

static long ms_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static void check_time(SearchCtx *ctx) {
    if ((ctx->nodes & 4095) == 0) {
        long deadline = atomic_load_explicit(&ctx->ctl->deadline_ms,
                                             memory_order_relaxed);
        if (ms_now() >= deadline) {
            ctx->stopped = true;
            atomic_store_explicit(&ctx->ctl->stop, true, memory_order_relaxed);
        } else if (atomic_load_explicit(&ctx->ctl->stop, memory_order_relaxed)) {
            ctx->stopped = true;
        }
    }
}

/* Repetition: scan back by 2 plies (same side to move), capped by halfmove
   clock (irreversible moves break repetition chains). 2-fold within search
   is treated as a draw — standard practice. */
static bool is_repetition(const SearchCtx *ctx, uint64_t hash, int halfmove) {
    int limit = (halfmove < ctx->rep_top) ? halfmove : ctx->rep_top;
    for (int i = ctx->rep_top - 2; i >= ctx->rep_top - limit && i >= 0; i -= 2) {
        if (ctx->rep_stack[i] == hash) return true;
    }
    return false;
}

/* MVV-LVA: victim value minus attacker value */
static int mvv_lva(Move m) {
    int victim   = MOVE_CAPTURE(m);
    int attacker = MOVE_PIECE(m);
    if (victim == PIECE_NONE) return 0;
    return victim * 10 - attacker;
}

static int score_move(const SearchCtx *ctx, const Position *pos, Move m,
                      Move tt_move, Move counter_move, int ply, int color) {
    if (m == tt_move) return 1000000;
    int cap = MOVE_CAPTURE(m);
    if (cap != PIECE_NONE) {
        /* Bucket good (SEE >= 0) and bad captures separately. Good ones
           outrank killers; bad ones still beat history but trail killers. */
        int base = (pos_see(pos, m) >= 0) ? 100000 : 60000;
        return base + mvv_lva(m);
    }
    if (MOVE_PROMO(m) == QUEEN)       return 95000;
    if (m == ctx->killers[ply][0])    return 90000;
    if (m == ctx->killers[ply][1])    return 80000;
    /* Counter-move slots below killers and above history. MOVE_NONE never
       compares equal to a real generated move, so when there's no counter
       (e.g. at the root) this is a no-op. */
    if (m == counter_move)            return 70000;
    return ctx->history[color][MOVE_FROM(m)][MOVE_TO(m)];
}

/* Selection sort one move at a time: bring the highest-scored move to slot i */
static void pick_move(Move *moves, int *scores, int n, int i) {
    int best = i;
    for (int j = i + 1; j < n; j++) {
        if (scores[j] > scores[best]) best = j;
    }
    if (best != i) {
        Move tm = moves[i]; moves[i] = moves[best]; moves[best] = tm;
        int  ts = scores[i]; scores[i] = scores[best]; scores[best] = ts;
    }
}

static int quiescence(const Position *pos, int alpha, int beta,
                      SearchCtx *ctx, int ply) {
    ctx->nodes++;
    ctx->qnodes++;
    if (ply > ctx->seldepth) ctx->seldepth = ply;
    check_time(ctx);
    if (ctx->stopped) return 0;

    if (ply >= MAX_PLY) return search_eval(ctx, MAX_PLY, pos);

    bool in_check = pos_in_check(pos);
    int stand_pat = -SEARCH_INF;

    Move moves[MAX_MOVES];
    int n;
    if (in_check) {
        /* In check: no stand-pat (position isn't quiet); generate all legal
           moves so we can find evasions and detect mate. */
        n = pos_gen_moves(pos, moves);
        if (n == 0) return -SEARCH_MATE + ply;
    } else {
        stand_pat = search_eval(ctx, ply, pos);
        if (stand_pat >= beta) return beta;
        if (stand_pat > alpha) alpha = stand_pat;
        /* Pseudo-captures only — we'll SEE-prune before paying legality cost,
           saving a pos_do_move + sq_attacked_by per discarded capture. */
        n = pos_gen_pseudo_captures(pos, moves);
    }

    int scores[MAX_MOVES];
    for (int i = 0; i < n; i++) {
        Move m = moves[i];
        if (MOVE_CAPTURE(m) != PIECE_NONE)
            scores[i] = (MOVE_PROMO(m) == QUEEN) ? 100000 + mvv_lva(m) : mvv_lva(m);
        else if (MOVE_PROMO(m) == QUEEN)
            scores[i] = 95000;
        else
            scores[i] = 0;
    }

    for (int i = 0; i < n; i++) {
        pick_move(moves, scores, n, i);
        Move m = moves[i];

        /* Delta pruning: if even winning the captured piece cleanly can't
           raise alpha, skip without the cost of SEE or pos_do_move. */
        static const int DELTA_VAL[7] = { 100, 320, 330, 500, 900, 0, 0 };
        if (!in_check && MOVE_CAPTURE(m) != PIECE_NONE) {
            if (stand_pat + DELTA_VAL[MOVE_CAPTURE(m)] + 200 < alpha)
                continue;
        }

        /* SEE pruning: skip clearly losing captures when we're not under
           check. We still allow queen promotions through since their
           positional gain can outweigh the SEE loss, and non-captures are
           never seen here outside the in-check branch. */
        if (!in_check && MOVE_CAPTURE(m) != PIECE_NONE && MOVE_PROMO(m) == PIECE_NONE) {
            if (pos_see(pos, m) < 0) { ctx->see_qprunes++; continue; }
        }

        Position child;
        search_do_move(ctx, ply, pos, m, &child);

        /* Inline legality filter — only for the non-in-check path, since
           the in-check branch generated already-legal moves. Doing this
           per-surviving capture (after SEE) saves the pos_do_move +
           sq_attacked_by that filter_legal would have done for every
           SEE-discarded capture too. */
        if (!in_check) {
            int our_king = lsb64(child.pieces[pos->side][KING]);
            if (sq_attacked_by(&child, our_king, child.side)) continue;
        }

        int score = -quiescence(&child, -beta, -alpha, ctx, ply + 1);
        if (ctx->stopped) return 0;
        if (score >= beta) return beta;
        if (score > alpha) alpha = score;
    }
    return alpha;
}

static bool has_non_pawn_material(const Position *pos, int color) {
    return (pos->pieces[color][KNIGHT] | pos->pieces[color][BISHOP]
          | pos->pieces[color][ROOK]   | pos->pieces[color][QUEEN]) != 0;
}

static inline void pv_copy(SearchCtx *ctx, int ply, Move m) {
    /* Set our PV to [m, ...child's PV]. Called only when a new best move is
       selected at a PV node (full-window). Bounded by MAX_PLY so we never
       overrun pv[ply] storage. */
    ctx->pv[ply][0] = m;
    int child_len = ctx->pv_len[ply + 1];
    int max_tail  = MAX_PLY - 1 - ply;
    if (child_len > max_tail) child_len = max_tail;
    for (int i = 0; i < child_len; i++)
        ctx->pv[ply][i + 1] = ctx->pv[ply + 1][i];
    ctx->pv_len[ply] = 1 + child_len;
}

static int alpha_beta(const Position *pos, int depth, int alpha, int beta,
                      SearchCtx *ctx, TT *tt, int ply, bool can_null,
                      Move prev_move) {
    ctx->nodes++;
    if (ply > ctx->seldepth) ctx->seldepth = ply;
    check_time(ctx);
    if (ctx->stopped) return 0;

    /* Hard ply cap: with check extensions a pathological perpetual-check tree
       could push ply past MAX_PLY, OOB-ing per-ply arrays (killers, pv).
       Bail out with static eval — quiescence does the same. */
    if (ply >= MAX_PLY) return search_eval(ctx, MAX_PLY, pos);

    bool is_pv    = (beta - alpha) > 1;
    bool in_check = pos_in_check(pos);

    /* Reset PV length at this ply — refilled when a best move is chosen. */
    ctx->pv_len[ply] = 0;

    if (ply > 0) {
        if (pos->halfmove >= 100) return 0;
        if (is_repetition(ctx, pos->hash, pos->halfmove)) return 0;

        /* Mate-distance pruning */
        int mate_alpha = -SEARCH_MATE + ply;
        int mate_beta  =  SEARCH_MATE - ply;
        if (alpha < mate_alpha) alpha = mate_alpha;
        if (beta  > mate_beta)  beta  = mate_beta;
        if (alpha >= beta) return alpha;
    }

    /* Check extension */
    if (in_check) depth++;

    /* TT lookup */
    Move tt_move = MOVE_NONE;
    ctx->tt_probes++;
    TTEntry *entry = tt_probe(tt, pos->hash);
    if (entry) {
        ctx->tt_hits++;
        if (!is_pv && ply > 0 && entry->depth >= depth) {
            int score = entry->score;
            if      (score >  MATE_BOUND) score -= ply;
            else if (score < -MATE_BOUND) score += ply;
            switch ((Bound)entry->bound) {
                case BOUND_EXACT: return score;
                case BOUND_LOWER: if (score >= beta)  return score; break;
                case BOUND_UPPER: if (score <= alpha) return score; break;
            }
        }
        tt_move = entry->best_move;
    }

    if (depth <= 0)
        return quiescence(pos, alpha, beta, ctx, ply);

    /* Internal Iterative Deepening: at PV nodes with no TT move, do a
       shallow search to populate the TT, then re-probe for a move hint. */
    if (is_pv && tt_move == MOVE_NONE && depth >= 4) {
        alpha_beta(pos, depth - 2, alpha, beta, ctx, tt, ply, false, prev_move);
        entry = tt_probe(tt, pos->hash);
        if (entry) tt_move = entry->best_move;
    }

    /* Syzygy WDL probe. Skips the rest of the subtree when we have a
       definitive result. Constraints:
       - non-root (ply > 0): at the root we use DTZ at the entry point
       - depth >= configured probe-depth floor (skip qsearch-adjacent nodes)
       - syzygy_probe_wdl handles piece-count / castling / halfmove cutoffs
       Score is mapped to ±SEARCH_MATE/2 so TB wins/losses dominate eval but
       stay BELOW real mate scores (mate-distance pruning continues to work). */
    if (ply > 0 && depth >= syzygy_get_probe_depth()) {
        int tb_score;
        if (syzygy_probe_wdl(pos, &tb_score)) {
            ctx->tb_hits++;
            /* TB results are perfect, so behave like a fail-high/fail-low. */
            return tb_score;
        }
    }

    /* Reuse the static eval cached in TT when we have one — saves a full
       evaluate() pass on revisits. The TT XOR-key check upstream guarantees
       the entry matches this position and isn't torn. */
    int static_eval = entry ? entry->static_eval : search_eval(ctx, ply, pos);
    ctx->eval_stack[ply] = static_eval;
    bool improving = (ply >= 2 && static_eval > ctx->eval_stack[ply - 2]);

    /* Razoring: at low depth far below alpha, drop into qsearch. */
    if (!is_pv && !in_check && depth <= 2 && static_eval + 300 <= alpha) {
        int q = quiescence(pos, alpha, beta, ctx, ply);
        if (q <= alpha) return q;
    }

    /* Reverse-futility / static null move (depth ≤ 6, non-pv, not in check) */
    if (!is_pv && !in_check && depth <= 6) {
        int margin = improving ? 90 * depth : 70 * depth;
        if (static_eval - margin >= beta) {
            ctx->rfp_prunes++;
            return static_eval - margin;
        }
    }

    /* Null-move pruning */
    if (can_null && !is_pv && !in_check && depth >= 3
        && static_eval >= beta
        && has_non_pawn_material(pos, pos->side)) {
        ctx->nmp_attempts++;
        Position np = *pos;
        if (np.ep_sq >= 0) {
            np.hash ^= ZOBRIST_EP[FILE_OF(np.ep_sq)];
            np.ep_sq = -1;
        }
        np.side  ^= 1;
        np.hash  ^= ZOBRIST_SIDE;
        np.halfmove++;

        int R = 3 + depth / 6;
        ctx->rep_stack[ctx->rep_top++] = np.hash;
        /* Null move doesn't move any piece — every perspective accumulator
           is unchanged, only side-to-move flips (which nnue_evaluate_acc picks
           up via np.side). Propagate the accumulator unchanged to ply+1. */
        if (nnue_is_loaded() && ply + 1 < MAX_PLY + 2)
            ctx->acc_stack[ply + 1] = ctx->acc_stack[ply];
        /* prev_move = MOVE_NONE through null search — we didn't actually
           play a move, so it would be wrong to attribute its child cutoffs
           to a "counter-move" of anything. */
        int score = -alpha_beta(&np, depth - 1 - R, -beta, -beta + 1,
                                ctx, tt, ply + 1, false, MOVE_NONE);
        ctx->rep_top--;
        if (ctx->stopped) return 0;
        if (score >= beta) { ctx->nullmove_cuts++; return beta; }
    }

    Move moves[MAX_MOVES];
    int n = pos_gen_moves(pos, moves);
    if (n == 0)
        return in_check ? (-SEARCH_MATE + ply) : 0;

    /* Look up the counter-move once for the whole loop — keyed on the
       opponent's last move's (from, to). MOVE_NONE when at the root. */
    Move counter = (prev_move != MOVE_NONE)
        ? ctx->counter_moves[MOVE_FROM(prev_move)][MOVE_TO(prev_move)]
        : MOVE_NONE;
    int scores[MAX_MOVES];
    for (int i = 0; i < n; i++)
        scores[i] = score_move(ctx, pos, moves[i], tt_move, counter,
                               ply, pos->side);

    /* Singular extension: if the TT move is much better than all alternatives,
       extend its search by 1 ply. Determined by a reduced search excluding
       the TT move — if nothing else reaches s_beta, the TT move is singular. */
    bool singular_ext = false;
    if (!in_check && depth >= 8 && tt_move != MOVE_NONE
        && entry && entry->depth >= depth - 3
        && (Bound)entry->bound != BOUND_UPPER) {
        int s_beta = entry->score - 2 * depth;
        /* Shallow exclusion search: same position, half depth, null-window
           at s_beta, with tt_move excluded via a sentinel trick — we set
           tt_move score to -INF below in score_move if singular search is
           active. Instead, just do the search with a reduced depth and if
           the result is below s_beta, the TT move is singular. */
        int excl_score = alpha_beta(pos, depth / 2, s_beta - 1, s_beta,
                                    ctx, tt, ply, false, prev_move);
        if (!ctx->stopped && excl_score < s_beta)
            singular_ext = true;
    }

    int  orig_alpha = alpha;
    Move best_move  = moves[0];
    int  best       = -SEARCH_INF;
    int  searched   = 0;

    /* Track quiets that actually got searched (i.e., survived LMP/futility),
       so on a cutoff we can apply symmetric history malus to the earlier
       ones. Cap at 64 — beyond that the malus signal is saturated anyway. */
    Move quiets_searched[64];
    int  n_quiets_searched = 0;

    /* Extended futility: at low depths, if static eval plus a depth-scaled
       margin can't reach alpha, quiet moves are unlikely to help. */
    bool futile_frontier = (!is_pv && !in_check && depth <= 3
                            && static_eval + 150 * depth <= alpha);

    ctx->rep_stack[ctx->rep_top++] = pos->hash;

    for (int i = 0; i < n; i++) {
        pick_move(moves, scores, n, i);
        Move m = moves[i];

        bool is_capture = MOVE_CAPTURE(m) != PIECE_NONE;
        bool is_promo   = MOVE_PROMO(m)   != PIECE_NONE;
        bool quiet      = !is_capture && !is_promo;

        /* SEE pruning: skip losing captures at low depth (non-PV). */
        if (!is_pv && !in_check && is_capture && depth <= 4 && searched > 0) {
            if (pos_see(pos, m) < -50 * depth) continue;
        }

        /* Late Move Pruning: at low depth, the late quiet moves in a
           well-ordered list are very unlikely to be best. Skip them entirely.
           Tighter threshold when not improving. */
        int lmp_threshold = improving ? 3 + depth * depth : 2 + depth * depth;
        if (!is_pv && !in_check && quiet && depth <= 4
            && searched >= lmp_threshold) {
            ctx->lmp_prunes++;
            continue;
        }

        /* Extended futility: skip quiet moves that can't plausibly raise
           alpha at the leaf. Don't skip the very first move (we always need
           at least one searched to populate best_move). */
        if (futile_frontier && quiet && searched > 0) {
            ctx->futility_prunes++;
            continue;
        }

        /* Record this quiet for malus on later cutoff (last entry will be
           the cutoff move itself; we'll skip it below). */
        if (quiet && n_quiets_searched < 64)
            quiets_searched[n_quiets_searched++] = m;

        Position child;
        search_do_move(ctx, ply, pos, m, &child);

        /* Singular extension: give the TT move 1 extra ply if it was
           determined to be singular (much better than alternatives). */
        int ext = (singular_ext && m == tt_move) ? 1 : 0;

        int score;
        if (searched == 0) {
            /* PV move — full window */
            score = -alpha_beta(&child, depth - 1 + ext, -beta, -alpha,
                                ctx, tt, ply + 1, true, m);
        } else {
            /* LMR for late quiet moves and bad captures at sufficient depth.
               Table is log(d)*log(m)/1.75 + 0.5; scaled down for PV nodes
               and for moves with strong ordering signals (killers / counter).
               Bad captures (SEE < 0) also get reduced — they're late moves
               by nature, ordered after good captures and killers. */
            int reduction = 0;
            bool bad_cap = (is_capture && !is_promo && pos_see(pos, m) < 0);
            if (depth >= 3 && (quiet || bad_cap) && !in_check && searched >= 2) {
                int dd = depth    < 64 ? depth    : 63;
                int mm = searched < 64 ? searched : 63;
                reduction = LMR[dd][mm];
                if (is_pv) reduction--;
                if (quiet && (m == ctx->killers[ply][0] ||
                    m == ctx->killers[ply][1] ||
                    m == counter))
                    reduction--;
                if (quiet) {
                    int h = ctx->history[pos->side][MOVE_FROM(m)][MOVE_TO(m)];
                    if (h < -1000) reduction++;
                    else if (h > 2000) reduction--;
                }
                if (bad_cap) reduction++;
                /* Static eval falling (not improving) → late quiets are even
                   less likely to help; reduce one extra ply. `improving` is
                   already computed for RFP/LMP at this node. Targets the very
                   low lmr_research rate (LMR was too conservative). */
                if (!improving) reduction++;
                if (reduction < 0)            reduction = 0;
                if (reduction >= depth - 1)   reduction = depth - 2;
                if (reduction < 0)            reduction = 0;
            }

            /* Zero-window search */
            if (reduction > 0) ctx->lmr_done++;
            score = -alpha_beta(&child, depth - 1 - reduction, -alpha - 1, -alpha,
                                ctx, tt, ply + 1, true, m);

            /* If reduced search beat alpha, redo at full depth, still zero window */
            if (!ctx->stopped && score > alpha && reduction > 0) {
                ctx->lmr_researched++;
                score = -alpha_beta(&child, depth - 1, -alpha - 1, -alpha,
                                    ctx, tt, ply + 1, true, m);
            }

            /* If still inside the window, re-search full window for PV */
            if (!ctx->stopped && score > alpha && score < beta)
                score = -alpha_beta(&child, depth - 1, -beta, -alpha,
                                    ctx, tt, ply + 1, true, m);
        }

        if (ctx->stopped) { ctx->rep_top--; return 0; }
        searched++;

        if (score > best) {
            best = score; best_move = m;
            /* Refresh PV at this ply: this is the new best continuation. */
            pv_copy(ctx, ply, m);
        }
        if (score > alpha) alpha = score;
        if (alpha >= beta) {
            ctx->cutoffs++;
            if (searched == 1) ctx->cutoffs_first++;
            /* Beta cutoff — record killers + history + counter for quiet moves */
            if (quiet) {
                if (ctx->killers[ply][0] != m) {
                    ctx->killers[ply][1] = ctx->killers[ply][0];
                    ctx->killers[ply][0] = m;
                }
                int *h = &ctx->history[pos->side][MOVE_FROM(m)][MOVE_TO(m)];
                /* History gravity: each update is pulled toward HISTORY_MAX
                   proportionally to its current size. Self-limiting — values
                   asymptote to HISTORY_MAX without ever crossing it, so no
                   global /2 cliff is needed on the hot cutoff path. The
                   bonus magnitude is the usual depth*depth. */
                int bonus = depth * depth;
                *h += bonus - (*h) * bonus / HISTORY_MAX;
                /* Malus to earlier quiets that got searched but didn't cut.
                   Last quiets_searched entry is m itself — skip it. Same
                   gravity formula with negative bonus: values asymptote
                   toward -HISTORY_MAX. Improves move ordering on revisit
                   by suppressing quiets that previously failed to cut. */
                for (int q = 0; q < n_quiets_searched - 1; q++) {
                    Move qm = quiets_searched[q];
                    int *qh = &ctx->history[pos->side][MOVE_FROM(qm)][MOVE_TO(qm)];
                    *qh += -bonus - (*qh) * bonus / HISTORY_MAX;
                }
                /* Counter-move: "after the opponent played prev_move, this
                   quiet move m caused a cutoff." Skip at the root (no prev). */
                if (prev_move != MOVE_NONE) {
                    ctx->counter_moves[MOVE_FROM(prev_move)][MOVE_TO(prev_move)] = m;
                }
            }
            break;
        }
    }

    ctx->rep_top--;

    /* Store TT with ply-independent mate score */
    int tt_score = best;
    if      (tt_score >  MATE_BOUND) tt_score += ply;
    else if (tt_score < -MATE_BOUND) tt_score -= ply;
    Bound bound = (best <= orig_alpha) ? BOUND_UPPER
                : (best >= beta)       ? BOUND_LOWER
                                       : BOUND_EXACT;
    tt_store(tt, pos->hash, depth, tt_score, bound, best_move, static_eval);

    if (ply == 0) ctx->root_best = best_move;

    return best;
}

/* ── Iterative deepening loop, run by each thread (single or SMP).
   `log_to_stderr` is true only for the main thread so multiple workers don't
   interleave their info lines. */
typedef struct {
    Move best_move;
    int  best_score;
    int  depth_reached;   /* highest fully-completed iteration */
    bool any_completed;
    Move ponder_move;     /* pv[0][1] — predicted reply, for `bestmove … ponder …` */
} IdResult;

static IdResult run_id(SearchCtx *ctx, const Position *pos, int max_depth,
                       TT *tt, bool log_to_stderr) {
    IdResult r = { MOVE_NONE, 0, 0, false, MOVE_NONE };

    /* Seed the per-ply accumulator stack at ply 0. Subsequent plies are
       advanced incrementally by search_do_move(). When no NNUE is loaded,
       acc_stack stays untouched (search_eval routes to the HCE path). */
    if (nnue_is_loaded())
        nnue_acc_refresh(&ctx->acc_stack[0], pos);

    /* Pre-seed root_best with the first legal move so we never return
       MOVE_NONE even if the search is interrupted before depth 1 completes. */
    Move first[MAX_MOVES];
    int  nf = pos_gen_moves(pos, first);
    if (nf == 0) return r;
    ctx->root_best = first[0];
    r.best_move    = first[0];

    /* Forced-move shortcut: if there's exactly one legal move, no search is
       needed — save the entire clock budget for the next move. We still go
       one ply deep to populate a score for chat / dashboard. */
    if (nf == 1) {
        Position child;
        pos_do_move(pos, first[0], &child);
        r.best_score    = -evaluate(&child);
        r.depth_reached = 1;
        r.any_completed = true;
        if (log_to_stderr) {
            fprintf(stderr,
                    "info depth 1 score cp %d nodes 1 nps 1 time 1 "
                    "seldepth 1 hashfull %d hashmb %d "
                    "tthits 0 ttprobes 0 qnodes 0 "
                    "cutoffs 0 cutoffs1 0 nullcuts 0 tbhits 0\n",
                    r.best_score, tt_hashfull(tt), tt->mb);
            fflush(stderr);
        }
        return r;
    }

    int  prev_score = 0;
    int  prev_widens = 0;

    for (int depth = 1; depth <= max_depth; depth++) {
        long iter_start = ms_now();
        int alpha  = -SEARCH_INF;
        int beta   =  SEARCH_INF;
        int window = (prev_widens >= 2) ? 50 : 25;

        if (depth >= 4) {
            alpha = prev_score - window;
            beta  = prev_score + window;
        }
        int iter_widens = 0;

        int score = 0;
        while (1) {
            /* Root call: prev_move = MOVE_NONE (no opponent move to attribute
               cutoffs to at this level). */
            score = alpha_beta(pos, depth, alpha, beta, ctx, tt, 0, true, MOVE_NONE);
            if (ctx->stopped) break;
            if (score <= alpha) {
                ctx->asp_widens++;
                iter_widens++;
                alpha -= window;
                window *= 2;
                if (window > 1000) alpha = -SEARCH_INF;
            } else if (score >= beta) {
                ctx->asp_widens++;
                iter_widens++;
                beta += window;
                window *= 2;
                if (window > 1000) beta = SEARCH_INF;
            } else {
                break;
            }
        }

        if (ctx->stopped) break;

        if (ctx->root_best != MOVE_NONE) r.best_move = ctx->root_best;
        prev_widens     = iter_widens;
        prev_score      = score;
        r.best_score    = score;
        r.depth_reached = depth;
        r.any_completed = true;
        /* Capture the ponder move (predicted reply = pv[0][1]) from THIS
           completed iteration — a later iteration may start and get aborted,
           leaving pv[0] inconsistent, so we can't read it at function exit. */
        r.ponder_move = (ctx->pv_len[0] >= 2 && ctx->pv[0][0] == r.best_move)
                        ? ctx->pv[0][1] : MOVE_NONE;

        if (log_to_stderr) {
            long elapsed = ms_now() - iter_start;
            /* `elapsed` here is per-iteration, used only for the info-line
               nps. Total elapsed since search start isn't separately tracked
               anymore — info-line nps is now per-iteration nps, which is
               actually more informative (steady-state vs. ramp). */
            if (elapsed < 1) elapsed = 1;
            long nps = (long)(ctx->nodes * 1000ULL / (uint64_t)elapsed);
            int hashfull = tt_hashfull(tt);
            /* PV line: space-separated UCI moves from the root. Cap at 16
               plies to keep info-line bounded; the engine's chosen depth can
               be much higher (we just don't dump 100 plies of move text). */
            char pv_buf[128]; pv_buf[0] = '\0';
            int pv_show = ctx->pv_len[0] < 16 ? ctx->pv_len[0] : 16;
            int pv_off  = 0;
            for (int i = 0; i < pv_show; i++) {
                char mb[6];
                move_to_uci(ctx->pv[0][i], mb);
                int wrote = snprintf(pv_buf + pv_off, sizeof(pv_buf) - pv_off,
                                     "%s%s", i ? " " : "", mb);
                if (wrote < 0 || pv_off + wrote >= (int)sizeof(pv_buf) - 1) break;
                pv_off += wrote;
            }
            fprintf(stderr,
                    "info depth %d score cp %d nodes %llu nps %ld time %ld "
                    "seldepth %d hashfull %d hashmb %d "
                    "tthits %llu ttprobes %llu qnodes %llu "
                    "cutoffs %llu cutoffs1 %llu nullcuts %llu "
                    "tbhits %llu pv %s\n",
                    depth, score, (unsigned long long)ctx->nodes, nps, elapsed,
                    ctx->seldepth, hashfull, tt->mb,
                    (unsigned long long)ctx->tt_hits,
                    (unsigned long long)ctx->tt_probes,
                    (unsigned long long)ctx->qnodes,
                    (unsigned long long)ctx->cutoffs,
                    (unsigned long long)ctx->cutoffs_first,
                    (unsigned long long)ctx->nullmove_cuts,
                    (unsigned long long)ctx->tb_hits,
                    pv_buf);
            /* PROF: derived rates that surface tuning targets. Separate
               `info string` line so dashboard parsers that already key off
               the depth-line schema don't need to change. */
            double ord    = ctx->cutoffs       ? (100.0 * ctx->cutoffs_first) / ctx->cutoffs       : 0.0;
            double lmr_r  = ctx->lmr_done      ? (100.0 * ctx->lmr_researched) / ctx->lmr_done     : 0.0;
            double nmp_y  = ctx->nmp_attempts  ? (100.0 * ctx->nullmove_cuts) / ctx->nmp_attempts  : 0.0;
            double see_r  = ctx->qnodes        ? (100.0 * ctx->see_qprunes) / ctx->qnodes          : 0.0;
            fprintf(stderr,
                    "info string PROF ordering=%.1f%% lmr_research=%.1f%% "
                    "nmp_yield=%.1f%% see_qprune=%.1f%% "
                    "rfp=%llu fut=%llu lmp=%llu asp_widens=%llu\n",
                    ord, lmr_r, nmp_y, see_r,
                    (unsigned long long)ctx->rfp_prunes,
                    (unsigned long long)ctx->futility_prunes,
                    (unsigned long long)ctx->lmp_prunes,
                    (unsigned long long)ctx->asp_widens);
            fflush(stderr);
        }

        /* Stop early if mate found and we're committed. Require a few
           plies of confirmation to avoid acting on a shallow TT-collision
           artifact that resolves at higher depth. */
        if (depth >= 3 && (score > MATE_BOUND || score < -MATE_BOUND)) break;

        /* Soft time budget: predict the next iteration as ~2× the last
           (effective branching factor 3-5 minus TT savings). If we wouldn't
           finish it inside the deadline, don't start it.
           Previous version compared total elapsed to budget — that aborted at
           ~50% of total budget regardless of where the last ply landed, so
           we systematically left ~half our thinking time on the table. */
        long iter_elapsed = ms_now() - iter_start;
        long deadline = atomic_load_explicit(&ctx->ctl->deadline_ms,
                                             memory_order_relaxed);
        if (ms_now() + 2 * iter_elapsed > deadline) break;
    }

    return r;
}

/* ── Worker thread (for both the main thread and SMP helpers).
   Each worker has its own SearchCtx but shares the TT and the global_stop
   atomic, plus the per-search deadline. */
typedef struct {
    const Position  *pos;
    int              max_depth;
    TT              *tt;
    SearchControl   *ctl;        /* shared deadline + stop, see check_time */
    bool             is_main;

    /* Optional game-history seed for repetition detection. NULL/0 = none. */
    const uint64_t  *history;
    int              history_len;

    /* outputs */
    Move best_move;
    int  best_score;
    int  depth_reached;
    bool any_completed;
    Move ponder_move;
} Worker;

static void *worker_run(void *arg) {
    Worker *w = arg;
    SearchCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.ctl       = w->ctl;
    ctx.root_best = MOVE_NONE;

    /* Seed rep_stack with game-history hashes so 2-fold via prior moves is
       visible to is_repetition. Capacity matches rep_stack (1024); excess
       (very long games) is truncated from the front since is_repetition is
       bounded by halfmove anyway. */
    if (w->history && w->history_len > 0) {
        int n   = w->history_len;
        int cap = (int)(sizeof(ctx.rep_stack) / sizeof(ctx.rep_stack[0]));
        const uint64_t *src = w->history;
        if (n > cap) { src += (n - cap); n = cap; }
        memcpy(ctx.rep_stack, src, (size_t)n * sizeof(uint64_t));
        ctx.rep_top = n;
    }

    IdResult r = run_id(&ctx, w->pos, w->max_depth, w->tt, w->is_main);

    w->best_move     = r.best_move;
    w->best_score    = r.best_score;
    w->depth_reached = r.depth_reached;
    w->any_completed = r.any_completed;
    w->ponder_move   = r.ponder_move;
    return NULL;
}

void search_init(void) {
    /* Idempotent: recompute the LMR table from scratch each call. Row/col 0
       stay zero (we never reduce with no depth or zero moves searched). */
    for (int d = 0; d < 64; d++) LMR[d][0] = 0;
    for (int m = 0; m < 64; m++) LMR[0][m] = 0;
    for (int d = 1; d < 64; d++) {
        for (int m = 1; m < 64; m++) {
            double r  = 0.5 + log((double)d) * log((double)m) / 1.75;
            int    ir = (int)r;
            LMR[d][m] = ir < 0 ? 0 : ir;
        }
    }
}

Move find_best_move(const Position *pos, int time_ms, TT *tt) {
    return find_best_move_ext(pos, time_ms, MAX_PLY, tt);
}

Move find_best_move_ext(const Position *pos, int time_ms, int max_depth, TT *tt) {
    return find_best_move_score(pos, time_ms, max_depth, tt, NULL, NULL);
}

Move find_best_move_score(const Position *pos, int time_ms, int max_depth,
                          TT *tt, int *out_score, bool *out_have_score) {
    return find_best_move_smp(pos, time_ms, max_depth, tt, 1,
                              out_score, out_have_score);
}

BenchResult search_bench(const Position *pos, int depth, TT *tt) {
    BenchResult br = { MOVE_NONE, 0, 0 };
    if (depth <= 0) depth = 1;
    if (depth > MAX_PLY) depth = MAX_PLY;

    SearchCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    /* Huge deadline so check_time / soft-budget never fire — bench must be
       deterministic across runs and machines. */
    SearchControl ctl;
    atomic_init(&ctl.deadline_ms, ms_now() + 3600L * 1000L);
    atomic_init(&ctl.stop, false);
    ctx.ctl       = &ctl;
    ctx.root_best = MOVE_NONE;

    IdResult r = run_id(&ctx, pos, depth, tt, false);

    br.best  = r.best_move;
    br.score = r.best_score;
    br.nodes = ctx.nodes;
    return br;
}

TimedBenchResult search_bench_timed(const Position *pos, int time_ms, TT *tt) {
    TimedBenchResult br = { MOVE_NONE, 0, 0, 0, 0 };
    if (time_ms < 1) time_ms = 1;

    SearchCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    long start = ms_now();
    SearchControl ctl;
    atomic_init(&ctl.deadline_ms, start + time_ms);
    atomic_init(&ctl.stop, false);
    ctx.ctl       = &ctl;
    ctx.root_best = MOVE_NONE;

    IdResult r = run_id(&ctx, pos, MAX_PLY, tt, false);

    br.best          = r.best_move;
    br.score         = r.best_score;
    br.depth_reached = r.depth_reached;
    br.elapsed_ms    = ms_now() - start;
    br.nodes         = ctx.nodes;
    return br;
}

Move find_best_move_smp(const Position *pos, int time_ms, int max_depth, TT *tt,
                        int threads, int *out_score, bool *out_have_score) {
    return find_best_move_smp_hist(pos, time_ms, max_depth, tt, threads,
                                   NULL, 0, out_score, out_have_score);
}

Move find_best_move_smp_hist(const Position *pos, int time_ms, int max_depth,
                             TT *tt, int threads,
                             const uint64_t *history, int history_len,
                             int *out_score, bool *out_have_score) {
    return find_best_move_smp_hist_depth(pos, time_ms, max_depth, tt, threads,
                                         history, history_len,
                                         out_score, out_have_score, NULL);
}

Move find_best_move_smp_hist_depth(const Position *pos, int time_ms, int max_depth,
                                   TT *tt, int threads,
                                   const uint64_t *history, int history_len,
                                   int *out_score, bool *out_have_score,
                                   int *out_depth) {
    /* Self-owned control: a fixed deadline `time_ms` out, no external stop.
       The pondering UCI path uses find_best_move_smp_ctl directly so it can
       extend the deadline / request a stop while the search runs. */
    SearchControl ctl;
    atomic_init(&ctl.deadline_ms, ms_now() + time_ms);
    atomic_init(&ctl.stop, false);
    return find_best_move_smp_ctl(pos, &ctl, max_depth, tt, threads,
                                  history, history_len,
                                  out_score, out_have_score, out_depth, NULL);
}

Move find_best_move_smp_ctl(const Position *pos, SearchControl *ctl,
                            int max_depth, TT *tt, int threads,
                            const uint64_t *history, int history_len,
                            int *out_score, bool *out_have_score, int *out_depth,
                            Move *out_ponder) {
    if (threads < 1) threads = 1;
    if (max_depth <= 0 || max_depth > MAX_PLY) max_depth = MAX_PLY;
    if (out_have_score) *out_have_score = false;
    if (out_depth)      *out_depth      = 0;
    if (out_ponder)     *out_ponder     = MOVE_NONE;

    /* Syzygy root probe — DTZ-aware best move, returned before any worker is
       spawned (tb_probe_root is documented not thread-safe). On a hit we emit
       a synthetic info line so the dashboard sees the tbhit + the chosen score,
       and skip the search entirely. We still respect the user's Hash setup. */
    {
        int tb_score = 0;
        Move tb_move = syzygy_probe_root(pos, &tb_score);
        if (tb_move != MOVE_NONE) {
            char mb[6]; move_to_uci(tb_move, mb);
            fprintf(stderr,
                    "info depth 1 score cp %d nodes 0 nps 0 time 0 "
                    "seldepth 0 hashfull %d hashmb %d "
                    "tthits 0 ttprobes 0 qnodes 0 "
                    "cutoffs 0 cutoffs1 0 nullcuts 0 tbhits 1 pv %s\n",
                    tb_score, tt_hashfull(tt), tt->mb, mb);
            fflush(stderr);
            if (out_score)      *out_score      = tb_score;
            if (out_have_score) *out_have_score = true;
            (void)max_depth; (void)threads;
            (void)history; (void)history_len;
            return tb_move;
        }
    }

    Worker *workers = calloc((size_t)threads, sizeof(Worker));
    pthread_t *handles = (threads > 1)
                         ? calloc((size_t)threads, sizeof(pthread_t)) : NULL;

    for (int i = 0; i < threads; i++) {
        workers[i].pos          = pos;
        workers[i].max_depth    = max_depth;
        workers[i].tt           = tt;
        workers[i].ctl          = ctl;
        workers[i].is_main      = (i == 0);
        workers[i].history      = history;
        workers[i].history_len  = history_len;
        workers[i].best_move    = MOVE_NONE;
    }

    /* Spawn helpers (workers 1..N-1) first, then run worker 0 in the
       current thread. When the main worker returns we signal helpers to
       stop and join them all. */
    for (int i = 1; i < threads; i++) {
        pthread_create(&handles[i], NULL, worker_run, &workers[i]);
    }
    worker_run(&workers[0]);
    atomic_store_explicit(&ctl->stop, true, memory_order_relaxed);
    for (int i = 1; i < threads; i++) {
        pthread_join(handles[i], NULL);
    }

    Move best = workers[0].best_move;
    if (workers[0].any_completed) {
        if (out_score)      *out_score      = workers[0].best_score;
        if (out_have_score) *out_have_score = true;
        if (out_depth)      *out_depth      = workers[0].depth_reached;
        if (out_ponder)     *out_ponder     = workers[0].ponder_move;
    }

    free(workers);
    free(handles);
    return best;
}
