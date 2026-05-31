/* NNUE inference — see docs/nnue-format.md for the authoritative spec.

   Architecture: 768 -> N -> 1 perspective net, int16-quantized weights,
   clipped-ReLU accumulator, int64 output dot. We do a FULL accumulator refresh
   on every nnue_evaluate() call: simple and bug-resistant. The "efficiently
   updatable" incremental path (add/sub only the moved piece's column in
   make/unmake) is a deliberate later optimization — get correct Elo first. */
#include "nnue.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Loaded net. NULL weight pointers => not loaded. */
static struct {
    uint32_t N;                 /* hidden size */
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

/* Read exactly `count` little-endian int16 into dst. Returns false on short read.
   The build target (ARM/x86 Linux) is little-endian, so a bulk fread matches the
   on-disk layout directly. */
static bool read_i16(FILE *f, int16_t *dst, size_t count) {
    return fread(dst, sizeof(int16_t), count, f) == count;
}

bool nnue_load(const char *path) {
    nnue_free();

    /* Silent on file-absent: the startup best-effort load probes a default
       path that usually won't exist. Explicit callers (UCI EvalFile) report
       the failure themselves. A present-but-corrupt file still warns below. */
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
        || N == 0 || N > 4096) {
        fprintf(stderr, "info string NNUE: bad header in %s "
                "(magic/version/N/features mismatch)\n", path);
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

/* clipped ReLU into [0, QA] */
static inline int32_t crelu(int32_t x) {
    if (x < 0)        return 0;
    if (x > NNUE_QA)  return NNUE_QA;
    return x;
}

/* Accumulate b + sum of active feature columns into acc[N], for one perspective.
   Feature index per docs/nnue-format.md:
     rel_color = (color==persp)?0:1 ; rel_sq = persp==WHITE? sq : sq^56
     idx = rel_color*384 + piece*64 + rel_sq                                   */
static void accumulate(const Position *pos, int persp, int32_t *acc) {
    const uint32_t N = net.N;
    for (uint32_t n = 0; n < N; n++) acc[n] = net.ft_b[n];

    for (int color = 0; color < 2; color++) {
        int rel_color = (color == persp) ? 0 : 1;
        for (int piece = 0; piece < 6; piece++) {
            uint64_t bb = pos->pieces[color][piece];
            while (bb) {
                int sq = lsb64(bb); bb &= bb - 1;
                int rel_sq = (persp == WHITE) ? sq : (sq ^ 56);
                uint32_t idx = (uint32_t)(rel_color * 384 + piece * 64 + rel_sq);
                const int16_t *col = net.ft_w + (size_t)idx * N;
                for (uint32_t n = 0; n < N; n++) acc[n] += col[n];
            }
        }
    }
}

int nnue_evaluate(const Position *pos) {
    const uint32_t N = net.N;
    int32_t acc_w[4096], acc_b[4096];   /* N <= 4096 enforced at load */
    accumulate(pos, WHITE, acc_w);
    accumulate(pos, BLACK, acc_b);

    /* Side to move's accumulator goes first (the "us" half). */
    const int32_t *us   = (pos->side == WHITE) ? acc_w : acc_b;
    const int32_t *them = (pos->side == WHITE) ? acc_b : acc_w;

    int64_t out = net.out_b;
    const int16_t *ow = net.out_w;
    for (uint32_t n = 0; n < N; n++) out += (int64_t)crelu(us[n])   * ow[n];
    for (uint32_t n = 0; n < N; n++) out += (int64_t)crelu(them[n]) * ow[N + n];

    /* logit (scaled by QA*QB) -> centipawns */
    return (int)(out * NNUE_SCALE / ((int64_t)NNUE_QA * NNUE_QB));
}
