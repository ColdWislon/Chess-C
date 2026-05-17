#include "bench.h"
#include "board.h"
#include "search.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

/* Bench positions: a small fixed set covering openings, middlegames, tactics,
   and endgames. Hand-picked so total runtime at depth 8 stays under ~30 s on
   a Pi 4. Determinism: at fixed depth with TT cleared between positions and
   single-thread search, total_nodes is reproducible — so an unchanged
   total_nodes after a refactor proves the search tree is unchanged. */
static const char *BENCH_POSITIONS[] = {
    /* 1. Startpos */
    "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
    /* 2. Kiwipete — heavy tactics, all piece types active */
    "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1",
    /* 3. R+P endgame */
    "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1",
    /* 4. Open middlegame, both sides castled */
    "r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1",
    /* 5. Italian-ish middlegame with pinned pieces */
    "rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8",
    /* 6. Quiet positional middlegame */
    "r4rk1/1pp1qppp/p1np1n2/2b1p1B1/2B1P1b1/P1NP1N2/1PP1QPPP/R4RK1 w - - 0 10",
    /* 7. K+P endgame (passed pawn race) */
    "8/k7/3p4/p2P1p2/P2P1P2/8/8/K7 w - - 0 1",
    /* 8. QGD-type middlegame, full board */
    "r1bq1rk1/pp2bppp/2n2n2/2pp4/3P4/2P1PN2/PP1NBPPP/R1BQK2R w KQ - 0 7",
};
static const int BENCH_NUM = sizeof(BENCH_POSITIONS) / sizeof(BENCH_POSITIONS[0]);

static long ms_diff(struct timespec a, struct timespec b) {
    return (b.tv_sec - a.tv_sec) * 1000L
         + (b.tv_nsec - a.tv_nsec) / 1000000L;
}

BenchSummary run_bench(int depth, TT *tt) {
    BenchSummary s = { 0, 0 };
    if (depth <= 0) depth = 8;

    printf("=== bench depth %d, %d positions ===\n", depth, BENCH_NUM);

    for (int i = 0; i < BENCH_NUM; i++) {
        Position bp;
        if (!pos_from_fen(&bp, BENCH_POSITIONS[i])) {
            fprintf(stderr, "bench: invalid FEN %d\n", i + 1);
            continue;
        }
        tt_clear(tt);

        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        BenchResult br = search_bench(&bp, depth, tt);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        long ms = ms_diff(t0, t1);
        if (ms < 1) ms = 1;

        char buf[6];
        if (br.best != MOVE_NONE) move_to_uci(br.best, buf);
        else                      strcpy(buf, "0000");

        long nps = (long)(br.nodes * 1000ULL / (uint64_t)ms);
        printf("Position %d/%d: move %s score cp %d nodes %llu time %ld ms nps %ld\n",
               i + 1, BENCH_NUM, buf, br.score,
               (unsigned long long)br.nodes, ms, nps);
        fflush(stdout);

        s.total_ms    += ms;
        s.total_nodes += br.nodes;
    }

    long total_nps = (s.total_ms > 0)
        ? (long)(s.total_nodes * 1000ULL / (uint64_t)s.total_ms) : 0;
    printf("===============================\n");
    printf("Total time (ms) : %ld\n",  s.total_ms);
    printf("Total nodes     : %llu\n", (unsigned long long)s.total_nodes);
    printf("Nodes/second    : %ld\n",  total_nps);
    fflush(stdout);

    return s;
}

TimedBenchSummary run_bench_timed(int ms_per_position, TT *tt) {
    TimedBenchSummary s = { 0, 0, 0, 0 };
    if (ms_per_position <= 0) ms_per_position = 1000;

    printf("=== bench_timed budget %d ms/position, %d positions ===\n",
           ms_per_position, BENCH_NUM);

    for (int i = 0; i < BENCH_NUM; i++) {
        Position bp;
        if (!pos_from_fen(&bp, BENCH_POSITIONS[i])) {
            fprintf(stderr, "bench_timed: invalid FEN %d\n", i + 1);
            continue;
        }
        tt_clear(tt);

        TimedBenchResult br = search_bench_timed(&bp, ms_per_position, tt);

        char buf[6];
        if (br.best != MOVE_NONE) move_to_uci(br.best, buf);
        else                      strcpy(buf, "0000");

        int pct = (int)(br.elapsed_ms * 100 / ms_per_position);
        printf("Position %d/%d: move %s depth %d score cp %d elapsed %ld ms (%d%% of budget) nodes %llu\n",
               i + 1, BENCH_NUM, buf, br.depth_reached, br.score,
               br.elapsed_ms, pct, (unsigned long long)br.nodes);
        fflush(stdout);

        s.total_elapsed_ms += br.elapsed_ms;
        s.total_budget_ms  += ms_per_position;
        s.total_depth      += br.depth_reached;
        s.total_nodes      += br.nodes;
    }

    int avg_util = (s.total_budget_ms > 0)
        ? (int)(s.total_elapsed_ms * 100 / s.total_budget_ms) : 0;
    printf("===============================\n");
    printf("Total elapsed (ms) : %ld\n",  s.total_elapsed_ms);
    printf("Total budget (ms)  : %ld\n",  s.total_budget_ms);
    printf("Budget utilization : %d%%\n", avg_util);
    printf("Total depth        : %d\n",   s.total_depth);
    printf("Total nodes        : %llu\n", (unsigned long long)s.total_nodes);
    fflush(stdout);

    return s;
}
