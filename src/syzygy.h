#pragma once
#include "board.h"
#include <stdbool.h>

/* Syzygy tablebase wrapper around Fathom (external/tbprobe.c).
   All functions are no-ops when no tablebase files have been loaded. */

/* Initialize from a colon-separated list of directories. Idempotent —
   safe to call again with a new path; previous tables are freed.
   Returns true if at least one table was loaded. */
bool syzygy_init(const char *path);

/* Free Fathom's internal state. Safe to call when nothing was loaded. */
void syzygy_shutdown(void);

/* Largest piece count for which we have tables, or 0 if none loaded. */
int  syzygy_largest(void);

/* WDL probe — fast, thread-safe, suitable for in-search use.
   Returns true and writes `*out_score` (centipawns, side-to-move POV) if the
   probe succeeded. Returns false on miss, OOR (too many pieces / above the
   configured probe limit / castling rights / non-zero rule50). */
bool syzygy_probe_wdl(const Position *pos, int *out_score);

/* Root probe (DTZ-aware) — NOT thread-safe, single-call per search.
   Returns a recommended move (in the engine's Move encoding) plus the
   matching score, or MOVE_NONE on miss. Picks a move that preserves the
   best WDL outcome and minimises DTZ for wins (or maximises for losses).
   `out_score` is filled even on a draw/loss so the dashboard sees the
   correct evaluation. */
Move syzygy_probe_root(const Position *pos, int *out_score);

/* Configuration knobs — controlled by UCI options. */
void syzygy_set_probe_limit(int max_pieces); /* clamps WDL/root probes */
void syzygy_set_50_move_rule(bool use_50);   /* affects WIN vs CURSED_WIN */
void syzygy_set_probe_depth(int min_depth);  /* min remaining depth to probe */
int  syzygy_get_probe_limit(void);
int  syzygy_get_probe_depth(void);

/* Did the most recent syzygy_probe_root return a TB hit? Cleared on every
   syzygy_probe_root call (success or miss). Read by the chat layer to emit
   "syzygy: <result>" lines. */
bool syzygy_last_root_was_hit(void);
/* WDL band of the last root hit: -1 loss, 0 draw, +1 win. Undefined when
   the last call was a miss. */
int  syzygy_last_root_wdl_band(void);
