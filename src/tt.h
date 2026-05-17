#pragma once
#include <stddef.h>
#include "board.h"

typedef enum { BOUND_EXACT, BOUND_LOWER, BOUND_UPPER } Bound;

/* xor_key = real_key ^ entry_hash(data); see tt.c for race / sentinel rationale.
   generation is for aging — incremented per `go`, lets stale entries from older
   moves be evicted by fresh shallow ones. */
typedef struct {
    uint64_t xor_key;
    int32_t  score;
    int16_t  depth;
    uint8_t  bound;
    uint8_t  generation;
    Move     best_move;
} TTEntry;

typedef struct {
    TTEntry *entries;
    size_t   size;
    int      mb;          /* configured size in MB (for dashboard reporting) */
    uint8_t  generation;
} TT;

void tt_init(TT *tt, int mb);
void tt_free(TT *tt);
void tt_clear(TT *tt);
void tt_new_generation(TT *tt);
TTEntry *tt_probe(TT *tt, uint64_t key);
void tt_store(TT *tt, uint64_t key, int depth, int score, Bound bound, Move best);
/* UCI hashfull: permille of entries used, sampled from the first 1000. */
int  tt_hashfull(const TT *tt);
size_t tt_size(const TT *tt);
