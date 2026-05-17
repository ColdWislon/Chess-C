#pragma once
#include "board.h"
#include "tt.h"
#include <stdint.h>

#define SEARCH_INF       1000000
#define SEARCH_MATE      900000

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
