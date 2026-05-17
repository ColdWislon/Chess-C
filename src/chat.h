#pragma once
#include "board.h"
#include <stdbool.h>

typedef struct {
    Move our_move;
    int  score;          /* in centipawns, side-to-move POV */
    bool have_score;
    int  last_score;
    bool have_last;
    bool is_first_move;
} ChatContext;

/* Build a single chat line for the given move/score context.
   Returns 0 if nothing should be said. Otherwise writes a null-terminated
   line to `out` (capacity `out_size`, recommended ≥ 128) and returns 1. */
int chat_build(const ChatContext *ctx, char *out, int out_size);
