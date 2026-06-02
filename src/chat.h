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

    /* Move number we're about to play (1-indexed across the whole game).
       Used by the chat layer to rotate feature-mention lines across early
       game moves without repeating itself. */
    int  move_number;

    /* True if `our_move` came from the opening book (book_probe returned it
       without invoking search). */
    bool was_book_move;

    /* True only on the FIRST move after leaving the opening book — the chat
       layer fires a one-time "out of book" message in that slot. */
    bool just_left_book;

    /* True if the search returned at depth 1 because Syzygy's root probe
       gave a definitive WDL result. wdl_band is -1/0/+1 for loss/draw/win. */
    bool was_root_tb_hit;
    int  tb_wdl_band;

    /* Last fully-completed search depth (0 for book/TB shortcuts). */
    int  depth_reached;

    /* Engine config snapshot — for feature-brag messages. */
    int  threads;        /* UCI Threads value (≥1) */
    int  hash_mb;        /* TT size in MB */
    int  tb_largest;     /* syzygy_largest(); 0 if no tables loaded */
    bool book_loaded;    /* whether main.c's book pointer was non-NULL */
    bool nnue_loaded;    /* whether an .nnue net is loaded (else HCE/PeSTO) */
} ChatContext;

/* Build a single chat line for the given move/score context.
   Returns 0 if nothing should be said. Otherwise writes a null-terminated
   line to `out` (capacity `out_size`, recommended ≥ 128) and returns 1. */
int chat_build(const ChatContext *ctx, char *out, int out_size);
