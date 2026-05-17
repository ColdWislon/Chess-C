/* Search: iterative deepening, alpha-beta with PVS, null-move pruning, LMR,
   killers, history, aspiration windows, check extensions, repetition / 50-move.
   Optional Lazy SMP: N threads run independent ID from root, sharing the TT. */
#define _POSIX_C_SOURCE 200809L
#include "search.h"
#include "eval.h"
#include <time.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <pthread.h>

#define MAX_PLY      64
#define HISTORY_MAX  16384
#define MATE_BOUND   (SEARCH_MATE - 1000)

typedef struct {
    uint64_t nodes;
    uint64_t qnodes;        /* nodes inside quiescence */
    uint64_t tt_probes;
    uint64_t tt_hits;
    uint64_t cutoffs;       /* total beta cutoffs */
    uint64_t cutoffs_first; /* cutoffs that happened on first move (ordering quality) */
    uint64_t nullmove_cuts; /* successful null-move prunes */
    int      seldepth;      /* deepest ply reached (including qsearch) */

    long     deadline_ms;
    bool     stopped;

    /* SMP coordination: when any thread sets this, all threads exit at the
       next check_time poll. NULL is valid (single-threaded). */
    atomic_bool *global_stop;

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

    /* Principal Variation: pv[ply] is the best line found from that ply
       onward, length pv_len[ply]. After a PV node selects move m, we copy
       child's pv into ours after the leading m. Sized MAX_PLY+1 so that the
       `ply+1` lookup in pv_copy is in bounds even at the deepest leaf. */
    Move     pv[MAX_PLY + 1][MAX_PLY];
    int      pv_len[MAX_PLY + 1];
} SearchCtx;

static long ms_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static void check_time(SearchCtx *ctx) {
    if ((ctx->nodes & 4095) == 0) {
        if (ms_now() >= ctx->deadline_ms) {
            ctx->stopped = true;
            if (ctx->global_stop)
                atomic_store_explicit(ctx->global_stop, true, memory_order_relaxed);
        } else if (ctx->global_stop &&
                   atomic_load_explicit(ctx->global_stop, memory_order_relaxed)) {
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

    if (ply >= MAX_PLY) return evaluate(pos);

    bool in_check = pos_in_check(pos);

    Move moves[MAX_MOVES];
    int n;
    if (in_check) {
        /* In check: no stand-pat (position isn't quiet); generate all legal
           moves so we can find evasions and detect mate. */
        n = pos_gen_moves(pos, moves);
        if (n == 0) return -SEARCH_MATE + ply;
    } else {
        int stand_pat = evaluate(pos);
        if (stand_pat >= beta) return beta;
        if (stand_pat > alpha) alpha = stand_pat;
        n = pos_gen_captures(pos, moves);
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

        /* SEE pruning: skip clearly losing captures when we're not under
           check. We still allow queen promotions through since their
           positional gain can outweigh the SEE loss, and non-captures are
           never seen here outside the in-check branch. */
        if (!in_check && MOVE_CAPTURE(m) != PIECE_NONE && MOVE_PROMO(m) == PIECE_NONE) {
            if (pos_see(pos, m) < 0) continue;
        }

        Position child;
        pos_do_move(pos, m, &child);
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
    if (ply >= MAX_PLY) return evaluate(pos);

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

    /* Reuse the static eval cached in TT when we have one — saves a full
       evaluate() pass on revisits. The TT XOR-key check upstream guarantees
       the entry matches this position and isn't torn. */
    int static_eval = entry ? entry->static_eval : evaluate(pos);

    /* Reverse-futility / static null move (depth ≤ 6, non-pv, not in check) */
    if (!is_pv && !in_check && depth <= 6) {
        int margin = 90 * depth;
        if (static_eval - margin >= beta)
            return static_eval - margin;
    }

    /* Null-move pruning */
    if (can_null && !is_pv && !in_check && depth >= 3
        && static_eval >= beta
        && has_non_pawn_material(pos, pos->side)) {
        Position np = *pos;
        if (np.ep_sq >= 0) {
            np.hash ^= ZOBRIST_EP[FILE_OF(np.ep_sq)];
            np.ep_sq = -1;
        }
        np.side  ^= 1;
        np.hash  ^= ZOBRIST_SIDE;
        np.halfmove++;

        int R = 2 + (depth >= 6 ? 1 : 0);
        ctx->rep_stack[ctx->rep_top++] = np.hash;
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

    int  orig_alpha = alpha;
    Move best_move  = moves[0];
    int  best       = -SEARCH_INF;
    int  searched   = 0;

    /* Frontier futility: at depth==1, if our static eval plus a margin
       still can't reach alpha, quiet moves are unlikely to help. Computed
       once outside the loop. */
    bool futile_frontier = (!is_pv && !in_check && depth == 1
                            && static_eval + 200 <= alpha);

    ctx->rep_stack[ctx->rep_top++] = pos->hash;

    for (int i = 0; i < n; i++) {
        pick_move(moves, scores, n, i);
        Move m = moves[i];

        bool is_capture = MOVE_CAPTURE(m) != PIECE_NONE;
        bool is_promo   = MOVE_PROMO(m)   != PIECE_NONE;
        bool quiet      = !is_capture && !is_promo;

        /* Late Move Pruning: at low depth, the late quiet moves in a
           well-ordered list are very unlikely to be best. Skip them entirely.
           Threshold: searched >= 3 + depth*depth (so 4 @d1, 7 @d2, 12 @d3,
           19 @d4). Only outside PV nodes / when not in check. */
        if (!is_pv && !in_check && quiet && depth <= 4
            && searched >= 3 + depth * depth) {
            continue;
        }

        /* Frontier futility: skip quiet moves that can't plausibly raise
           alpha at the leaf. Don't skip the very first move (we always need
           at least one searched to populate best_move). */
        if (futile_frontier && quiet && searched > 0) {
            continue;
        }

        Position child;
        pos_do_move(pos, m, &child);

        int score;
        if (searched == 0) {
            /* PV move — full window */
            score = -alpha_beta(&child, depth - 1, -beta, -alpha,
                                ctx, tt, ply + 1, true, m);
        } else {
            /* LMR for late quiet moves at sufficient depth */
            int reduction = 0;
            if (depth >= 3 && quiet && !in_check && searched >= 4) {
                reduction = 1;
                if (searched >= 8) reduction = 2;
            }

            /* Zero-window search */
            score = -alpha_beta(&child, depth - 1 - reduction, -alpha - 1, -alpha,
                                ctx, tt, ply + 1, true, m);

            /* If reduced search beat alpha, redo at full depth, still zero window */
            if (!ctx->stopped && score > alpha && reduction > 0)
                score = -alpha_beta(&child, depth - 1, -alpha - 1, -alpha,
                                    ctx, tt, ply + 1, true, m);

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
} IdResult;

static IdResult run_id(SearchCtx *ctx, const Position *pos, int max_depth,
                       TT *tt, bool log_to_stderr) {
    IdResult r = { MOVE_NONE, 0, 0, false };

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
                    "cutoffs 0 cutoffs1 0 nullcuts 0\n",
                    r.best_score, tt_hashfull(tt), tt->mb);
            fflush(stderr);
        }
        return r;
    }

    int  prev_score = 0;

    for (int depth = 1; depth <= max_depth; depth++) {
        long iter_start = ms_now();
        int alpha  = -SEARCH_INF;
        int beta   =  SEARCH_INF;
        int window = 50;

        if (depth >= 4) {
            alpha = prev_score - window;
            beta  = prev_score + window;
        }

        int score = 0;
        while (1) {
            /* Root call: prev_move = MOVE_NONE (no opponent move to attribute
               cutoffs to at this level). */
            score = alpha_beta(pos, depth, alpha, beta, ctx, tt, 0, true, MOVE_NONE);
            if (ctx->stopped) break;
            if (score <= alpha) {
                alpha -= window;
                window *= 2;
                if (window > 1000) alpha = -SEARCH_INF;
            } else if (score >= beta) {
                beta += window;
                window *= 2;
                if (window > 1000) beta = SEARCH_INF;
            } else {
                break;
            }
        }

        if (ctx->stopped) break;

        if (ctx->root_best != MOVE_NONE) r.best_move = ctx->root_best;
        prev_score      = score;
        r.best_score    = score;
        r.depth_reached = depth;
        r.any_completed = true;

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
                    "cutoffs %llu cutoffs1 %llu nullcuts %llu pv %s\n",
                    depth, score, (unsigned long long)ctx->nodes, nps, elapsed,
                    ctx->seldepth, hashfull, tt->mb,
                    (unsigned long long)ctx->tt_hits,
                    (unsigned long long)ctx->tt_probes,
                    (unsigned long long)ctx->qnodes,
                    (unsigned long long)ctx->cutoffs,
                    (unsigned long long)ctx->cutoffs_first,
                    (unsigned long long)ctx->nullmove_cuts,
                    pv_buf);
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
        if (ms_now() + 2 * iter_elapsed > ctx->deadline_ms) break;
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
    long             deadline_ms;
    atomic_bool     *global_stop;
    bool             is_main;

    /* outputs */
    Move best_move;
    int  best_score;
    bool any_completed;
} Worker;

static void *worker_run(void *arg) {
    Worker *w = arg;
    SearchCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.deadline_ms = w->deadline_ms;
    ctx.global_stop = w->global_stop;
    ctx.root_best   = MOVE_NONE;

    IdResult r = run_id(&ctx, w->pos, w->max_depth, w->tt, w->is_main);

    w->best_move     = r.best_move;
    w->best_score    = r.best_score;
    w->any_completed = r.any_completed;
    return NULL;
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
    ctx.deadline_ms = ms_now() + 3600L * 1000L;
    ctx.global_stop = NULL;
    ctx.root_best   = MOVE_NONE;

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
    ctx.deadline_ms = start + time_ms;
    ctx.global_stop = NULL;
    ctx.root_best   = MOVE_NONE;

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
    if (threads < 1) threads = 1;
    if (max_depth <= 0 || max_depth > MAX_PLY) max_depth = MAX_PLY;
    if (out_have_score) *out_have_score = false;

    atomic_bool global_stop;
    atomic_init(&global_stop, false);
    long deadline = ms_now() + time_ms;

    Worker *workers = calloc((size_t)threads, sizeof(Worker));
    pthread_t *handles = (threads > 1)
                         ? calloc((size_t)threads, sizeof(pthread_t)) : NULL;

    for (int i = 0; i < threads; i++) {
        workers[i].pos          = pos;
        workers[i].max_depth    = max_depth;
        workers[i].tt           = tt;
        workers[i].deadline_ms  = deadline;
        workers[i].global_stop  = &global_stop;
        workers[i].is_main      = (i == 0);
        workers[i].best_move    = MOVE_NONE;
    }

    /* Spawn helpers (workers 1..N-1) first, then run worker 0 in the
       current thread. When the main worker returns we signal helpers to
       stop and join them all. */
    for (int i = 1; i < threads; i++) {
        pthread_create(&handles[i], NULL, worker_run, &workers[i]);
    }
    worker_run(&workers[0]);
    atomic_store_explicit(&global_stop, true, memory_order_relaxed);
    for (int i = 1; i < threads; i++) {
        pthread_join(handles[i], NULL);
    }

    Move best = workers[0].best_move;
    if (workers[0].any_completed) {
        if (out_score)      *out_score      = workers[0].best_score;
        if (out_have_score) *out_have_score = true;
    }

    free(workers);
    free(handles);
    return best;
}
