#pragma once
#include <stdint.h>
#include "tt.h"

/* Bench summary: aggregate timings and node count across the bench position
   set, used by both UCI `bench` and the make targets that A/B-compare two
   builds. */
typedef struct {
    long     total_ms;
    uint64_t total_nodes;
} BenchSummary;

/* Run the bench position set at the given depth. Prints per-position results
   and a final summary to stdout. Clears the TT before each position so node
   counts are independent and reproducible. */
BenchSummary run_bench(int depth, TT *tt);

/* Timed bench: same position set, but each position is given a fixed time
   budget (ms) instead of a fixed depth. Useful for changes that affect time
   management or stopping behavior (where fixed-depth bench can't see the
   difference). Reports per-position depth reached and budget utilization;
   summary includes total depth and total elapsed for A/B comparison. */
typedef struct {
    long     total_elapsed_ms;
    long     total_budget_ms;
    int      total_depth;
    uint64_t total_nodes;
} TimedBenchSummary;

TimedBenchSummary run_bench_timed(int ms_per_position, TT *tt);
