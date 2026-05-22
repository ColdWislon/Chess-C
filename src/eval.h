#pragma once
#include "board.h"

/* Evaluate position from side-to-move's perspective (positive = good for STM). */
int evaluate(const Position *pos);

/* Tunable eval parameters, exposed for the Texel tuner (src/texel.c).
   Defined in eval.c; the rest of the engine reads them as if they were
   constants. Touching these at runtime changes how the engine evaluates. */
extern int MATERIAL_MG[6];
extern int MATERIAL_EG[6];
extern int PST_PAWN_MG[64],   PST_PAWN_EG[64];
extern int PST_KNIGHT_MG[64], PST_KNIGHT_EG[64];
extern int PST_BISHOP_MG[64], PST_BISHOP_EG[64];
extern int PST_ROOK_MG[64],   PST_ROOK_EG[64];
extern int PST_QUEEN_MG[64],  PST_QUEEN_EG[64];
extern int PST_KING_MG[64],   PST_KING_EG[64];
