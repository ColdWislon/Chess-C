#pragma once

/* Texel-style automatic eval tuning.

   Loads a labeled EPD corpus, fits the sigmoid scaling constant K, then runs
   coordinate-descent over the tunable eval parameters (material + PSTs).
   Each pass twiddles every parameter by ±step and keeps the change if it
   reduces the mean-squared error of `sigmoid(eval) vs game result`. Step
   halves whenever a full pass yields no improvement; tuner exits when step
   reaches zero.

   The EPD file format expected: one position per line, FEN followed by a
   result token. Supported result tokens:
     "1-0"      → 1.0  (white won)
     "0-1"      → 0.0  (black won)
     "1/2-1/2"  → 0.5  (draw)
   Tokens may appear anywhere on the line (typical Texel "quiet-labeled.epd"
   has `c9 "1-0";` style; we just substring-match).

   Best tuned values are snapshotted to ./texel-snapshot.txt after every
   pass, so a SIGINT mid-run doesn't lose work. */

void texel_run(const char *epd_path);
