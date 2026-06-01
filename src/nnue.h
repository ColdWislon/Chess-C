#pragma once
#include "board.h"
#include <stdbool.h>
#include <stdint.h>

/* NNUE evaluation — small 768→N perspective net, int-quantized.
   Format and conventions are pinned in docs/nnue-format.md. The Python trainer
   in tools/nnue/ produces .nnue files this loader consumes. */

#define NNUE_MAGIC        "CNUE"
#define NNUE_VERSION      1u
#define NNUE_N_FEATURES   768u      /* 64 sq * 6 pieces * 2 rel-colors */
#define NNUE_N_MAX        256u      /* hard cap on hidden size — must match current
                                       net's N (each NNUEAcc is sized to this, and the
                                       search keeps MAX_PLY+2 of them on the stack per
                                       worker, so oversize hurts L1/L2 cache). Raise
                                       only when training a bigger net. */
#define NNUE_QA           255       /* feature/accumulator scale */
#define NNUE_QB           64        /* output-weight scale */
#define NNUE_SCALE        400       /* logit -> centipawn scale */

/* Accumulator: one int16 vector per perspective (WHITE half, BLACK half).
   The "side-to-move" half is selected by nnue_evaluate_acc based on pos->side.
   Sized to NNUE_N_MAX so the search-context stack has a fixed shape; the
   loaded net's actual N (≤ NNUE_N_MAX) drives the loop bounds at use time.

   Layout chosen for SIMD: int16 is the widest format that holds activation
   sums safely (max ≈ ±2600 vs int16 range ±32k), and a contiguous int16 row
   is what AVX2 vpaddw/vpsubw want. */
typedef struct {
    int16_t v[2][NNUE_N_MAX];   /* v[WHITE] = white-perspective acc, v[BLACK] = black */
} NNUEAcc;

/* Load a .nnue net from disk. Returns true on success; on any failure the
   engine state is left "no net loaded" and the caller should keep using the
   hand-crafted evaluate(). Thread-unsafe: call before spawning search workers
   (i.e. from the UCI thread), never mid-search. */
bool nnue_load(const char *path);

/* True once a valid net is loaded. evaluate() routes through nnue_evaluate()
   only when this is true. */
bool nnue_is_loaded(void);

/* Free the loaded net (idempotent). */
void nnue_free(void);

/* ── Accumulator API ──
   For the search hot path: maintain an NNUEAcc per ply, refresh once at the
   root, then incrementally advance it on each pos_do_move so eval skips the
   per-piece sum. Single-thread per NNUEAcc — each search worker gets its
   own stack. */

/* Build acc from scratch by accumulating every piece's feature column.
   Used at root, and as fallback for any future move type whose incremental
   update isn't worth coding. O(pieces * N). */
void nnue_acc_refresh(NNUEAcc *acc, const Position *pos);

/* Compute the post-move accumulator from the pre-move one. `before` is the
   position state BEFORE `m` was applied (so `before->side` is the mover).
   Handles quiet moves, captures, promotions, castling, and en passant
   incrementally — O(N) per affected feature, ≤ 4 features touched per move.
   `prev` and `next` may not alias. */
void nnue_acc_advance(NNUEAcc *next, const NNUEAcc *prev,
                      const Position *before, Move m);

/* Finish the eval given a ready accumulator. side_to_move selects which
   perspective is the "us" half (concatenated first; matches nnue.c). */
int  nnue_evaluate_acc(const NNUEAcc *acc, int side_to_move);

/* Convenience: refresh + evaluate in one call. The slow path — used by
   non-search callers (uci `eval` cmd, texel tuner). For search, use the
   acc API directly to avoid re-summing every node. */
int  nnue_evaluate(const Position *pos);

/* Self-consistency test: walk a perft tree from `pos` to `depth`, verifying
   that for every legal move, `nnue_acc_advance(prev, before, m)` produces the
   same bytes as `nnue_acc_refresh(after)`. Returns the number of mismatches
   (0 = perfect). Prints a diagnostic on the first mismatch then keeps
   counting. Used by the `nnuetest` UCI command. */
long long nnue_acc_self_test(const Position *pos, int depth);
