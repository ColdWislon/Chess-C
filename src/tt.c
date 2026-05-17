#include "tt.h"
#include <stdlib.h>
#include <string.h>

/* XOR trick for race detection + zero-key sentinel safety.
   Without locks, two threads writing the same slot can interleave the four
   field writes, producing a torn entry where the key doesn't match the data.
   Storing `key ^ entry_hash(data)` and validating on probe rejects those.
   Also: empty slot has xor_key=0 and zero data ⇒ entry_hash=0 ⇒ probe of any
   non-zero key fails to match. Avoids the "key==0 looks empty" pitfall. */
static inline uint64_t entry_hash(const TTEntry *e) {
    uint64_t h = (uint64_t)(uint32_t)e->score;
    h ^= (uint64_t)(uint16_t)e->depth  << 16;
    h ^= (uint64_t)e->bound            << 32;
    h ^= (uint64_t)e->generation       << 40;
    h ^= (uint64_t)e->best_move        << 48;
    return h;
}

void tt_init(TT *tt, int mb) {
    size_t bytes = (size_t)mb * 1024 * 1024;
    tt->size       = bytes / sizeof(TTEntry);
    tt->entries    = calloc(tt->size, sizeof(TTEntry));
    tt->mb         = mb;
    tt->generation = 0;
}

void tt_free(TT *tt)  { free(tt->entries); tt->entries = NULL; tt->size = 0; }
void tt_clear(TT *tt) {
    memset(tt->entries, 0, tt->size * sizeof(TTEntry));
    tt->generation = 0;
}
void tt_new_generation(TT *tt) { tt->generation++; }

TTEntry *tt_probe(TT *tt, uint64_t key) {
    TTEntry *e = &tt->entries[key % tt->size];
    if ((e->xor_key ^ entry_hash(e)) != key) return NULL;
    return e;
}

void tt_store(TT *tt, uint64_t key, int depth, int score, Bound bound, Move best) {
    TTEntry *e = &tt->entries[key % tt->size];
    /* Same position: always refresh. Deeper: replaces shallower collision.
       Aging: any entry from a prior `go` is freely replaceable so the table
       doesn't fossilize with stale results from earlier game positions. */
    bool same_pos = (e->xor_key ^ entry_hash(e)) == key;
    bool deeper   = ((int16_t)depth >= e->depth);
    bool stale    = (e->generation != tt->generation);
    if (same_pos || deeper || stale) {
        e->score      = (int32_t)score;
        e->depth      = (int16_t)depth;
        e->bound      = (uint8_t)bound;
        e->best_move  = best;
        e->generation = tt->generation;
        e->xor_key    = key ^ entry_hash(e);
    }
}

int tt_hashfull(const TT *tt) {
    if (!tt || !tt->entries || tt->size == 0) return 0;
    size_t sample = tt->size < 1000 ? tt->size : 1000;
    /* Stride across the whole table for a more representative sample than
       just the first `sample` buckets. */
    size_t stride = tt->size / sample;
    if (stride == 0) stride = 1;
    int used = 0;
    for (size_t i = 0, idx = 0; i < sample && idx < tt->size; i++, idx += stride)
        if (tt->entries[idx].xor_key != 0) used++;
    return (int)(used * 1000 / sample);
}

size_t tt_size(const TT *tt) { return tt ? tt->size : 0; }
