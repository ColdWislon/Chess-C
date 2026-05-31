#pragma once
#include "board.h"
#include <stdbool.h>

/* NNUE evaluation — small 768→N perspective net, int-quantized.
   Format and conventions are pinned in docs/nnue-format.md. The Python trainer
   in tools/nnue/ produces .nnue files this loader consumes. */

#define NNUE_MAGIC        "CNUE"
#define NNUE_VERSION      1u
#define NNUE_N_FEATURES   768u      /* 64 sq * 6 pieces * 2 rel-colors */
#define NNUE_QA           255       /* feature/accumulator scale */
#define NNUE_QB           64        /* output-weight scale */
#define NNUE_SCALE        400       /* logit -> centipawn scale */

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

/* Evaluate `pos` with the loaded net, side-to-move POV, in centipawns.
   Full accumulator refresh per call (correctness-first; incremental update is
   a future optimization). Undefined if no net is loaded — guard with
   nnue_is_loaded(). */
int  nnue_evaluate(const Position *pos);
