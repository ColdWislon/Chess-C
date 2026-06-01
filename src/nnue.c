/* NNUE inference — see docs/nnue-format.md for the authoritative spec.

   Architecture: 768 -> N -> 1 perspective net, int16-quantized weights,
   clipped-ReLU accumulator, int64 output dot.

   The hot search path uses the accumulator API (refresh once at root, then
   nnue_acc_advance per move). The convenience `nnue_evaluate(Position*)` does
   a full refresh on every call and is reserved for non-search callers
   (uci `eval` command, texel tuner) where the cost doesn't matter. */
#include "nnue.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Loaded net. NULL weight pointers => not loaded. */
static struct {
    uint32_t N;                 /* hidden size (≤ NNUE_N_MAX) */
    int16_t *ft_w;              /* [n_features * N], row-major W[f*N + n] */
    int16_t *ft_b;              /* [N] */
    int16_t *out_w;             /* [2N]  (us half | them half) */
    int32_t  out_b;             /* output bias, scaled by QA*QB */
    bool     loaded;
} net = {0};

bool nnue_is_loaded(void) { return net.loaded; }

void nnue_free(void) {
    free(net.ft_w);  free(net.ft_b);  free(net.out_w);
    memset(&net, 0, sizeof(net));
}

/* Read exactly `count` little-endian int16 into dst. Returns false on short read. */
static bool read_i16(FILE *f, int16_t *dst, size_t count) {
    return fread(dst, sizeof(int16_t), count, f) == count;
}

bool nnue_load(const char *path) {
    nnue_free();

    FILE *f = fopen(path, "rb");
    if (!f) return false;

    char magic[4];
    uint32_t version = 0, N = 0, nfeat = 0;
    bool hdr_ok = fread(magic, 1, 4, f) == 4
               && fread(&version, sizeof(version), 1, f) == 1
               && fread(&N, sizeof(N), 1, f) == 1
               && fread(&nfeat, sizeof(nfeat), 1, f) == 1;
    if (!hdr_ok || memcmp(magic, NNUE_MAGIC, 4) != 0
        || version != NNUE_VERSION || nfeat != NNUE_N_FEATURES
        || N == 0 || N > NNUE_N_MAX) {
        fprintf(stderr, "info string NNUE: bad header in %s "
                "(magic/version/N/features mismatch; N must be 1..%u)\n",
                path, NNUE_N_MAX);
        fclose(f);
        return false;
    }

    size_t ft_w_n = (size_t)nfeat * N;
    int16_t *ft_w  = malloc(ft_w_n * sizeof(int16_t));
    int16_t *ft_b  = malloc((size_t)N * sizeof(int16_t));
    int16_t *out_w = malloc((size_t)2 * N * sizeof(int16_t));
    int32_t  out_b = 0;
    bool body_ok = ft_w && ft_b && out_w
                && read_i16(f, ft_w, ft_w_n)
                && read_i16(f, ft_b, N)
                && read_i16(f, out_w, (size_t)2 * N)
                && fread(&out_b, sizeof(out_b), 1, f) == 1;
    fclose(f);

    if (!body_ok) {
        fprintf(stderr, "info string NNUE: truncated/short read in %s\n", path);
        free(ft_w); free(ft_b); free(out_w);
        return false;
    }

    net.N = N; net.ft_w = ft_w; net.ft_b = ft_b;
    net.out_w = out_w; net.out_b = out_b; net.loaded = true;
    fprintf(stderr, "info string NNUE: loaded %s (768 -> %u -> 1)\n", path, N);
    return true;
}

/* clipped ReLU into [0, QA] — int16 in, int16 out (saturating clamp). */
static inline int16_t crelu(int16_t x) {
    if (x < 0)        return 0;
    if (x > NNUE_QA)  return (int16_t)NNUE_QA;
    return x;
}

/* Feature index per docs/nnue-format.md:
     rel_color = (color == persp) ? 0 : 1
     rel_sq    = (persp == WHITE) ? sq : sq ^ 56
     idx       = rel_color * 384 + piece * 64 + rel_sq                            */
static inline uint32_t feature_index(int persp, int color, int piece, int sq) {
    int rel_color = (color == persp) ? 0 : 1;
    int rel_sq    = (persp == WHITE) ? sq : (sq ^ 56);
    return (uint32_t)(rel_color * 384 + piece * 64 + rel_sq);
}

/* acc += W[idx]   (column of N int16 weights, SIMD-friendly straight loop) */
static inline void acc_add(int16_t *acc, uint32_t idx) {
    const uint32_t N = net.N;
    const int16_t *col = net.ft_w + (size_t)idx * N;
    for (uint32_t n = 0; n < N; n++) acc[n] += col[n];
}

/* acc -= W[idx] */
static inline void acc_sub(int16_t *acc, uint32_t idx) {
    const uint32_t N = net.N;
    const int16_t *col = net.ft_w + (size_t)idx * N;
    for (uint32_t n = 0; n < N; n++) acc[n] -= col[n];
}

void nnue_acc_refresh(NNUEAcc *acc, const Position *pos) {
    const uint32_t N = net.N;
    for (int persp = 0; persp < 2; persp++) {
        int16_t *a = acc->v[persp];
        for (uint32_t n = 0; n < N; n++) a[n] = net.ft_b[n];
        for (int color = 0; color < 2; color++) {
            for (int piece = 0; piece < 6; piece++) {
                uint64_t bb = pos->pieces[color][piece];
                while (bb) {
                    int sq = lsb64(bb); bb &= bb - 1;
                    acc_add(a, feature_index(persp, color, piece, sq));
                }
            }
        }
    }
}

void nnue_acc_advance(NNUEAcc *next, const NNUEAcc *prev,
                      const Position *before, Move m) {
    /* Start by copying the entire previous state, then mutate only the
       handful of columns that change. */
    memcpy(next->v, prev->v, sizeof(prev->v));

    int from   = MOVE_FROM(m);
    int to     = MOVE_TO(m);
    int piece  = MOVE_PIECE(m);
    int cap    = MOVE_CAPTURE(m);
    int promo  = MOVE_PROMO(m);
    int flags  = MOVE_FLAGS(m);
    int us     = before->side;
    int them   = 1 - us;

    /* Piece that ends up on TO (promoted type if promotion, else moving piece). */
    int placed = (promo != PIECE_NONE) ? promo : piece;

    for (int persp = 0; persp < 2; persp++) {
        int16_t *a = next->v[persp];

        /* Mover leaves FROM as `piece`, lands on TO as `placed`. */
        acc_sub(a, feature_index(persp, us,    piece,  from));
        acc_add(a, feature_index(persp, us,    placed, to));

        /* Captured piece (if any) vanishes from its square. EP capture is
           offset by ±8 (rank behind the destination from the mover's POV). */
        if (cap != PIECE_NONE) {
            int cap_sq = to;
            if (flags & FLAG_EP)
                cap_sq = (us == WHITE) ? (to - 8) : (to + 8);
            acc_sub(a, feature_index(persp, them, cap,    cap_sq));
        }

        /* Castling: also relocate the rook. Kingside if king moves right. */
        if (flags & FLAG_CASTLE) {
            int rook_from, rook_to;
            if (to > from) {                         /* kingside */
                rook_from = (from & 56) | 7;         /* h-file on king's rank */
                rook_to   = to - 1;                  /* f-file (one left of king) */
            } else {                                 /* queenside */
                rook_from = (from & 56);             /* a-file on king's rank */
                rook_to   = to + 1;                  /* d-file (one right of king) */
            }
            acc_sub(a, feature_index(persp, us, ROOK, rook_from));
            acc_add(a, feature_index(persp, us, ROOK, rook_to));
        }
    }
}

int nnue_evaluate_acc(const NNUEAcc *acc, int side_to_move) {
    const uint32_t N = net.N;
    const int16_t *us   = (side_to_move == WHITE) ? acc->v[WHITE] : acc->v[BLACK];
    const int16_t *them = (side_to_move == WHITE) ? acc->v[BLACK] : acc->v[WHITE];

    int64_t out = net.out_b;
    const int16_t *ow = net.out_w;
    for (uint32_t n = 0; n < N; n++) out += (int64_t)crelu(us[n])   * ow[n];
    for (uint32_t n = 0; n < N; n++) out += (int64_t)crelu(them[n]) * ow[N + n];

    /* logit (scaled by QA*QB) -> centipawns */
    return (int)(out * NNUE_SCALE / ((int64_t)NNUE_QA * NNUE_QB));
}

int nnue_evaluate(const Position *pos) {
    /* Slow-path wrapper: refresh from scratch, then evaluate. Used by non-
       search callers (uci `eval` command, texel) where eval cost is irrelevant.
       Search must use nnue_acc_advance + nnue_evaluate_acc instead. */
    static NNUEAcc tmp;            /* not threadsafe — non-search callers only */
    nnue_acc_refresh(&tmp, pos);
    return nnue_evaluate_acc(&tmp, pos->side);
}

/* Compare two accumulators for the active N hidden units only (not the whole
   NNUE_N_MAX storage). Returns 0 on bitwise match. */
static int acc_diff(const NNUEAcc *a, const NNUEAcc *b) {
    const uint32_t N = net.N;
    for (int p = 0; p < 2; p++)
        for (uint32_t n = 0; n < N; n++)
            if (a->v[p][n] != b->v[p][n]) return 1;
    return 0;
}

static long long acc_test_recurse(const Position *pos, const NNUEAcc *prev,
                                  int depth, long long *mismatches,
                                  int *reported) {
    if (depth == 0) return 1;

    Move moves[MAX_MOVES];
    int n = pos_gen_moves(pos, moves);
    long long count = 0;

    for (int i = 0; i < n; i++) {
        Position child;
        pos_do_move(pos, moves[i], &child);

        NNUEAcc inc, fresh;
        nnue_acc_advance(&inc, prev, pos, moves[i]);
        nnue_acc_refresh(&fresh, &child);

        if (acc_diff(&inc, &fresh)) {
            (*mismatches)++;
            if (!*reported) {
                char ms[6]; move_to_uci(moves[i], ms);
                fprintf(stderr,
                    "nnuetest: MISMATCH at depth %d, move %s "
                    "(from=%d to=%d piece=%d cap=%d promo=%d flags=%d)\n",
                    depth, ms,
                    MOVE_FROM(moves[i]), MOVE_TO(moves[i]),
                    MOVE_PIECE(moves[i]), MOVE_CAPTURE(moves[i]),
                    MOVE_PROMO(moves[i]), MOVE_FLAGS(moves[i]));
                *reported = 1;
            }
        }

        count += acc_test_recurse(&child, &inc, depth - 1, mismatches, reported);
    }
    return count;
}

long long nnue_acc_self_test(const Position *pos, int depth) {
    if (!net.loaded) {
        fprintf(stderr, "nnuetest: no net loaded\n");
        return -1;
    }
    NNUEAcc root;
    nnue_acc_refresh(&root, pos);
    long long mismatches = 0;
    int reported = 0;
    long long leaves = acc_test_recurse(pos, &root, depth, &mismatches, &reported);
    fprintf(stderr,
        "nnuetest: depth %d, %lld leaf positions visited, %lld accumulator mismatches\n",
        depth, leaves, mismatches);
    return mismatches;
}
