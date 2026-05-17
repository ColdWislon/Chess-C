#pragma once
#include "board.h"

/* Evaluate position from side-to-move's perspective (positive = good for STM). */
int evaluate(const Position *pos);
