#include "perft.h"

uint64_t perft(const Position *pos, int depth) {
    if (depth == 0) return 1;

    Move moves[MAX_MOVES];
    int n = pos_gen_moves(pos, moves);
    if (depth == 1) return (uint64_t)n;

    uint64_t total = 0;
    for (int i = 0; i < n; i++) {
        Position child;
        pos_do_move(pos, moves[i], &child);
        total += perft(&child, depth - 1);
    }
    return total;
}
