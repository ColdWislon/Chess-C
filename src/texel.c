/* Texel tuner. See texel.h for the high-level flow and EPD format. */
#define _POSIX_C_SOURCE 200809L
#include "texel.h"
#include "board.h"
#include "eval.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

/* Hard cap so we can stack-allocate the parameter list. Material+PSTs +
   small slack is ~800; 1024 gives room for the user to register more later. */
#define MAX_PARAMS 1024

typedef struct {
    int *ptr;
    int  lo, hi;        /* inclusive bounds; clamp at apply time */
    int  default_val;   /* value at registration time — used by the L2
                           regularizer to pull noisy params back toward sane
                           starting values when the data signal is weak */
    char name[24];      /* short name for the snapshot dump */
} TuneParam;

static TuneParam params[MAX_PARAMS];
static int       n_params = 0;

/* Corpus held in two parallel arrays. results[] is the white-POV outcome
   (0/0.5/1) so we don't have to remember side-to-move at eval time. */
static Position *positions = NULL;
static double   *results   = NULL;
static int       n_pos     = 0;

/* Sigmoid scaling constant. Refit once at start so the chosen value matches
   the actual cp scale of THIS eval (not Stockfish's). */
static double K = 1.0;

/* L2 regularization strength. Penalizes (param - default)² so noisy
   coordinate descent on a small/noisy corpus can't push individual params
   to wild values. Tuned so a 100cp deviation on one param adds ~1.3e-6 to
   MSE — meaningful when the data signal is weak but invisible when it's
   strong. Set to 0 to disable. */
#define TEXEL_LAMBDA 1.0e-7

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static double sigmoid(int score_cp) {
    /* Classic Texel: 1 / (1 + 10^(-K·score/400)). Equivalent to a logistic
       with base 10 instead of e. */
    return 1.0 / (1.0 + pow(10.0, -K * (double)score_cp / 400.0));
}

/* L2 regularization term — sum of squared deviations from each param's
   default, scaled by TEXEL_LAMBDA. Pulls noisy directions back toward the
   sane starting values without dominating real signal. Cheap (O(n_params)
   per call vs O(n_pos) for the data term, which dominates by ~100x). */
static double reg_term(void) {
    if (TEXEL_LAMBDA == 0.0) return 0.0;
    double r = 0.0;
    for (int i = 0; i < n_params; i++) {
        double delta = (double)(*params[i].ptr - params[i].default_val);
        r += delta * delta;
    }
    return TEXEL_LAMBDA * r;
}

/* Mean squared error of sigmoid(eval) against the labeled outcome, over the
   whole corpus, PLUS the L2 regularization term. evaluate() returns
   side-to-move POV, so we flip when black is to move to get a uniform
   white-POV score. */
static double mse(void) {
    double E = 0.0;
    for (int i = 0; i < n_pos; i++) {
        int s = evaluate(&positions[i]);
        if (positions[i].side == BLACK) s = -s;
        double d = results[i] - sigmoid(s);
        E += d * d;
    }
    return E / (double)n_pos + reg_term();
}

/* Coarse grid search then refine — K's optimum is broad and unimodal in
   practice, so this is plenty. */
static void fit_K(void) {
    double best_K = K, best_E = mse();
    for (double k = 0.2; k <= 2.5; k += 0.05) {
        K = k;
        double E = mse();
        if (E < best_E) { best_E = E; best_K = k; }
    }
    K = best_K;
    fprintf(stderr, "[texel] fitted K=%.3f, baseline MSE=%.7f\n", K, best_E);
}

static void register_param(int *p, int lo, int hi, const char *name) {
    if (n_params >= MAX_PARAMS) {
        fprintf(stderr, "[texel] param overflow (>%d)\n", MAX_PARAMS);
        return;
    }
    params[n_params].ptr         = p;
    params[n_params].lo          = lo;
    params[n_params].hi          = hi;
    params[n_params].default_val = *p;          /* snapshot of the starting value */
    snprintf(params[n_params].name, sizeof(params[n_params].name), "%s", name);
    n_params++;
}

/* Material params only — 10 of them (5 pieces × MG/EG, skipping KING because
   both sides always have one so its value is a constant offset). The
   smaller param set is much harder to overfit on a noisy bot-vs-bot corpus.
   Bounds 0-3000 give enough headroom that even a queen-EG can find its
   true optimum without saturating (the previous 0-2000 was binding on the
   first run). */
static void register_material(void) {
    static const char *PIECE = "PNBRQ";
    char buf[24];
    for (int p = 0; p < 5; p++) {
        snprintf(buf, sizeof(buf), "MAT_MG_%c", PIECE[p]);
        register_param(&MATERIAL_MG[p], 0, 3000, buf);
        snprintf(buf, sizeof(buf), "MAT_EG_%c", PIECE[p]);
        register_param(&MATERIAL_EG[p], 0, 3000, buf);
    }
}

/* PSTs: all 6 pieces × MG/EG × 64 squares = 768 params. KING is included
   because king-square placement genuinely varies (back in MG, central in EG)
   — that's what makes the eval "tapered". Bounds ±500 instead of the
   previous ±200 — Stockfish-class engines routinely have PST squares in
   the ±300..±400 range, and the tight ±200 was saturating on the first run. */
static void register_psts(void) {
    static const char *PIECE6 = "PNBRQK";
    int *pst_mg[6] = { PST_PAWN_MG, PST_KNIGHT_MG, PST_BISHOP_MG,
                       PST_ROOK_MG, PST_QUEEN_MG,  PST_KING_MG };
    int *pst_eg[6] = { PST_PAWN_EG, PST_KNIGHT_EG, PST_BISHOP_EG,
                       PST_ROOK_EG, PST_QUEEN_EG,  PST_KING_EG };
    char buf[24];
    for (int p = 0; p < 6; p++) {
        for (int sq = 0; sq < 64; sq++) {
            snprintf(buf, sizeof(buf), "PST_%c_MG[%d]", PIECE6[p], sq);
            register_param(&pst_mg[p][sq], -500, 500, buf);
            snprintf(buf, sizeof(buf), "PST_%c_EG[%d]", PIECE6[p], sq);
            register_param(&pst_eg[p][sq], -500, 500, buf);
        }
    }
}

/* EPD loader. Each line: "<FEN fields...> <stuff containing the result>".
   We split the line at the first occurrence of a result token, parse the
   prefix as FEN, then read the token. */
static bool load_epd(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) { perror(path); return false; }

    /* Two-pass: count lines so we can size the arrays exactly. (Skipping
       this would mean realloc-heavy growth on the Pi where memory is finite.) */
    int cap = 0;
    char probe[2048];
    while (fgets(probe, sizeof(probe), f)) cap++;
    rewind(f);

    positions = malloc((size_t)cap * sizeof(Position));
    results   = malloc((size_t)cap * sizeof(double));
    if (!positions || !results) {
        free(positions); free(results); positions = NULL; results = NULL;
        fclose(f);
        fprintf(stderr, "[texel] OOM allocating corpus (%d positions)\n", cap);
        return false;
    }

    char line[2048];
    n_pos = 0;
    int parse_fail = 0;
    while (fgets(line, sizeof(line), f)) {
        /* Find the result token. Order matters: "1/2-1/2" before "1-0" /
           "0-1" so we don't grab the leading "1" of a draw. */
        double r = -1.0;
        char *cut = strstr(line, "1/2-1/2");
        if (cut) r = 0.5;
        else if ((cut = strstr(line, "1-0"))) r = 1.0;
        else if ((cut = strstr(line, "0-1"))) r = 0.0;
        if (r < 0) continue;
        *cut = '\0';

        /* Trim trailing whitespace / EPD operator junk left in the FEN slot. */
        size_t L = strlen(line);
        while (L > 0 && (line[L-1] == ' ' || line[L-1] == '\t' ||
                         line[L-1] == 'c' || line[L-1] == '9' ||
                         line[L-1] == '"' || line[L-1] == ';' ||
                         line[L-1] == '\r' || line[L-1] == '\n'))
            line[--L] = '\0';

        if (!pos_from_fen(&positions[n_pos], line)) { parse_fail++; continue; }
        results[n_pos++] = r;
    }
    fclose(f);
    fprintf(stderr, "[texel] loaded %d positions (%d FEN parse failures) from %s\n",
            n_pos, parse_fail, path);
    return n_pos > 0;
}

/* Dump tuned values to a file in a paste-ready C format. Snapshotted after
   every pass so the user can Ctrl-C without losing progress. */
static void snapshot(const char *path) {
    FILE *f = fopen(path, "w");
    if (!f) { perror(path); return; }
    fprintf(f, "/* Texel-tuned eval values. Generated %ld.\n"
               "   Replace the corresponding arrays in eval.c. */\n\n",
               (long)time(NULL));
    /* Material */
    fprintf(f, "int MATERIAL_MG[6] = { %4d, %4d, %4d, %4d, %4d, 0 };\n",
            MATERIAL_MG[0], MATERIAL_MG[1], MATERIAL_MG[2],
            MATERIAL_MG[3], MATERIAL_MG[4]);
    fprintf(f, "int MATERIAL_EG[6] = { %4d, %4d, %4d, %4d, %4d, 0 };\n\n",
            MATERIAL_EG[0], MATERIAL_EG[1], MATERIAL_EG[2],
            MATERIAL_EG[3], MATERIAL_EG[4]);
    /* PSTs */
    static const char *NAME[6] = { "PAWN", "KNIGHT", "BISHOP", "ROOK",
                                   "QUEEN", "KING" };
    int *mg[6] = { PST_PAWN_MG, PST_KNIGHT_MG, PST_BISHOP_MG,
                   PST_ROOK_MG, PST_QUEEN_MG,  PST_KING_MG };
    int *eg[6] = { PST_PAWN_EG, PST_KNIGHT_EG, PST_BISHOP_EG,
                   PST_ROOK_EG, PST_QUEEN_EG,  PST_KING_EG };
    for (int p = 0; p < 6; p++) {
        for (int phase = 0; phase < 2; phase++) {
            int *arr = phase ? eg[p] : mg[p];
            const char *suf = phase ? "EG" : "MG";
            fprintf(f, "int PST_%s_%s[64] = {\n", NAME[p], suf);
            for (int r = 0; r < 8; r++) {
                fprintf(f, "   ");
                for (int c = 0; c < 8; c++)
                    fprintf(f, " %4d,", arr[r*8 + c]);
                fprintf(f, "\n");
            }
            fprintf(f, "};\n");
        }
    }
    fclose(f);
}

void texel_run(const char *epd_path, TexelMode mode) {
    if (!load_epd(epd_path)) return;

    /* Always register material — material is what every PST and structure
       term plugs into, so freezing it while tuning PSTs would chase the
       wrong optimum. PSTs are optional (material-only mode skips them for
       a fast 10-param smoke test that's almost impossible to overfit). */
    register_material();
    if (mode == TEXEL_MODE_FULL) register_psts();
    fprintf(stderr, "[texel] mode=%s, registered %d parameters\n",
            (mode == TEXEL_MODE_FULL) ? "full" : "material-only", n_params);

    fit_K();

    double best_E = mse();
    int    step   = 8;
    int    pass   = 0;

    fprintf(stderr, "[texel] starting with MSE=%.7f step=%d\n", best_E, step);

    while (step > 0) {
        pass++;
        double t0 = now_s();
        int    n_improved = 0;
        for (int i = 0; i < n_params; i++) {
            int orig = *params[i].ptr;

            /* Try +step. */
            int v = orig + step;
            if (v > params[i].hi) v = params[i].hi;
            *params[i].ptr = v;
            double E_plus = mse();

            /* Try -step. */
            v = orig - step;
            if (v < params[i].lo) v = params[i].lo;
            *params[i].ptr = v;
            double E_minus = mse();

            /* Pick the better of (orig, +step, -step). Tiebreak on +step
               vs -step prefers +step (arbitrary, deterministic). */
            if (E_plus < best_E && E_plus <= E_minus) {
                v = orig + step;
                if (v > params[i].hi) v = params[i].hi;
                *params[i].ptr = v;
                best_E = E_plus;
                n_improved++;
            } else if (E_minus < best_E) {
                v = orig - step;
                if (v < params[i].lo) v = params[i].lo;
                *params[i].ptr = v;
                best_E = E_minus;
                n_improved++;
            } else {
                *params[i].ptr = orig;
            }
        }

        double dt = now_s() - t0;
        fprintf(stderr, "[texel] pass %d step=%d MSE=%.7f improved=%d/%d "
                        "time=%.1fs\n",
                pass, step, best_E, n_improved, n_params, dt);

        snapshot("./texel-snapshot.txt");

        if (n_improved == 0) {
            step /= 2;
            if (step > 0)
                fprintf(stderr, "[texel] no improvement, shrinking step to %d\n",
                        step);
        }
    }

    fprintf(stderr, "[texel] converged. final MSE=%.7f. "
                    "snapshot written to ./texel-snapshot.txt\n", best_E);

    free(positions); positions = NULL;
    free(results);   results   = NULL;
    n_pos = 0;
    n_params = 0;
}
