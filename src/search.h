#pragma once
#include "board.h"
#include "tt.h"
#include <stdint.h>
#include <stdatomic.h>
#include <limits.h>

#define SEARCH_INF       1000000
#define SEARCH_MATE      900000

/* Shared, mutable search control. All SMP workers in one search point at the
   same SearchControl. The deadline can be updated mid-search from another
   thread — used by pondering: a ponder search starts with
   deadline = PONDER_DEADLINE_INF (effectively never times out) and, on
   `ponderhit`, the UCI front-end stores the real budget. `stop` requests an
   immediate abort (workers return their best line so far at the next poll). */
typedef struct {
    _Atomic long deadline_ms;   /* CLOCK_MONOTONIC ms at which to stop */
    atomic_bool  stop;          /* hard stop: finish ASAP, return best so far */
} SearchControl;

/* A deadline so far out that check_time / the soft-budget abort never fire.
   LONG_MAX/4 keeps `ms_now() + 2*iter_elapsed` from overflowing. */
#define PONDER_DEADLINE_INF  (LONG_MAX / 4)

/* One-shot startup init for search-internal tables (LMR reduction lookup, …).
   Safe to call multiple times — idempotent. Call after board_init(). */
void search_init(void);

/* Returns MOVE_NONE if no move available.
   If `out_score` is non-NULL, it receives the score of the last completed
   iteration (side-to-move POV, centipawns). Untouched if no iteration completed. */
Move find_best_move(const Position *pos, int time_ms, TT *tt);
Move find_best_move_ext(const Position *pos, int time_ms, int max_depth, TT *tt);
Move find_best_move_score(const Position *pos, int time_ms, int max_depth,
                          TT *tt, int *out_score, bool *out_have_score);

/* Lazy SMP entry: `threads` workers run independent iterative deepening from
   root, sharing only the TT. Worker 0 (the main thread) is run in the caller's
   thread; helpers are pthreads spawned for the duration of the search.
   Returns the main worker's best move. `threads <= 1` is equivalent to
   find_best_move_score. */
Move find_best_move_smp(const Position *pos, int time_ms, int max_depth, TT *tt,
                        int threads, int *out_score, bool *out_have_score);

/* Variant of find_best_move_smp that seeds each worker's repetition stack
   with `history_len` prior game-position hashes (in chronological order,
   excluding the current root). Used by the UCI front-end to feed the
   `position … moves …` walk into 2-fold detection; without this, the search
   only sees positions reached inside the tree and can blunder into a draw
   the arbiter will claim from game history. Pass history=NULL/len=0 for the
   plain entry-point semantics. */
Move find_best_move_smp_hist(const Position *pos, int time_ms, int max_depth,
                             TT *tt, int threads,
                             const uint64_t *history, int history_len,
                             int *out_score, bool *out_have_score);

/* Same as above, plus an out-pointer for the highest fully-completed search
   depth (0 for book/TB shortcuts). Used by the chat layer to mention search
   depth on quiet moves. */
Move find_best_move_smp_hist_depth(const Position *pos, int time_ms, int max_depth,
                                   TT *tt, int threads,
                                   const uint64_t *history, int history_len,
                                   int *out_score, bool *out_have_score,
                                   int *out_depth);

/* Control-owning SMP entry point. Identical to find_best_move_smp_hist_depth
   except the caller supplies (and may mutate, from another thread) the
   SearchControl that holds the deadline + stop flag. The caller is responsible
   for initializing `ctl` (atomic_init both fields) before calling. Used by the
   UCI front-end to implement pondering. */
Move find_best_move_smp_ctl(const Position *pos, SearchControl *ctl,
                            int max_depth, TT *tt, int threads,
                            const uint64_t *history, int history_len,
                            int *out_score, bool *out_have_score, int *out_depth);

/* Deterministic fixed-depth search used by the bench infrastructure.
   Single-threaded, effectively no time limit — runs full iterative deepening
   to exactly `depth`. Returns the chosen move, the last-iteration score, and
   the total node count searched. Suitable for A/B regression testing: if the
   underlying change is purely performance, total nodes should be byte-identical
   before vs. after. */
typedef struct {
    Move     best;
    int      score;
    uint64_t nodes;
} BenchResult;

BenchResult search_bench(const Position *pos, int depth, TT *tt);

/* Timed bench: same single-threaded iterative deepening, but bounded by
   `time_ms` (just like a real game). Reports depth reached, actual elapsed
   time, and node count. Use for changes that affect time management or
   stopping behavior (where deterministic fixed-depth bench can't detect
   the difference). */
typedef struct {
    Move     best;
    int      score;
    int      depth_reached;
    long     elapsed_ms;
    uint64_t nodes;
} TimedBenchResult;

TimedBenchResult search_bench_timed(const Position *pos, int time_ms, TT *tt);
