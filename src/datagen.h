#pragma once
#include "tt.h"

/* Self-play data generation for NNUE training.

   Invoked from the UCI loop as:  datagen <out-path> [games] [depth] [seed]
   Plays fixed-depth self-play games starting from a short random opening,
   then writes one line per *quiet* position visited:

       <FEN> | <cp> | <result>

   where cp is the search score in centipawns from the side-to-move POV, and
   result is the game outcome from that same POV (1.0 win / 0.5 draw / 0.0 loss).

   CPU-heavy and single-threaded by design (reproducible given a seed). Run on
   WSL, never alongside a live Lichess game — see CLAUDE.md's idle check. */
void datagen_run_cmd(const char *args, TT *tt);
