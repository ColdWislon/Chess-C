/* Self-play data generation for NNUE training — see datagen.h.

   Pipeline role: this is step 1 of the NNUE loop (datagen -> train -> serialize
   -> EvalFile). Output lines feed tools/nnue/dataset.py. Quality of the labels
   is bounded by the engine's own search depth, so this is a bootstrap; once a
   first net is trained it can be plugged back in (EvalFile) and used to relabel
   for a stronger second generation. */
#include "datagen.h"
#include "board.h"
#include "search.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Minimal full-FEN serializer (the engine only ships a FEN *parser*).
   Emits placement + side + castling + ep + halfmove + fullmove so python-chess
   can parse the line directly. ── */
static void pos_to_fen(const Position *pos, char *out, size_t cap) {
    static const char PC[2][6] = {
        {'P','N','B','R','Q','K'}, {'p','n','b','r','q','k'}
    };
    char buf[128]; int b = 0;

    for (int rank = 7; rank >= 0; rank--) {
        int empty = 0;
        for (int file = 0; file < 8; file++) {
            int sq = rank * 8 + file, found = 0;
            for (int c = 0; c < 2 && !found; c++)
                for (int p = 0; p < 6; p++)
                    if (pos->pieces[c][p] & SQ_BB(sq)) {
                        if (empty) buf[b++] = (char)('0' + empty), empty = 0;
                        buf[b++] = PC[c][p]; found = 1; break;
                    }
            if (!found) empty++;
        }
        if (empty) buf[b++] = (char)('0' + empty);
        if (rank) buf[b++] = '/';
    }
    buf[b++] = ' ';
    buf[b++] = (pos->side == WHITE) ? 'w' : 'b';
    buf[b++] = ' ';

    int cb = b;
    if (pos->castling & CASTLE_WK) buf[b++] = 'K';
    if (pos->castling & CASTLE_WQ) buf[b++] = 'Q';
    if (pos->castling & CASTLE_BK) buf[b++] = 'k';
    if (pos->castling & CASTLE_BQ) buf[b++] = 'q';
    if (b == cb) buf[b++] = '-';
    buf[b++] = ' ';

    if (pos->ep_sq < 0) buf[b++] = '-';
    else { buf[b++] = (char)('a' + FILE_OF(pos->ep_sq));
           buf[b++] = (char)('1' + RANK_OF(pos->ep_sq)); }

    buf[b] = '\0';
    snprintf(out, cap, "%s %d %d", buf, pos->halfmove, pos->fullmove);
}

/* ── Game-termination helpers ── */
static bool insufficient_material(const Position *pos) {
    if (pos->pieces[WHITE][PAWN] || pos->pieces[BLACK][PAWN] ||
        pos->pieces[WHITE][ROOK] || pos->pieces[BLACK][ROOK] ||
        pos->pieces[WHITE][QUEEN]|| pos->pieces[BLACK][QUEEN])
        return false;
    int minors = popcount64(pos->pieces[WHITE][KNIGHT] | pos->pieces[WHITE][BISHOP] |
                            pos->pieces[BLACK][KNIGHT] | pos->pieces[BLACK][BISHOP]);
    return minors <= 1;   /* K vs K, K+minor vs K — clear draws */
}

/* One self-play game. Records quiet positions into fens[]/scores[]/sides[],
   returns the count and sets *result_white (1.0 white win / 0.5 draw / 0.0 loss).
   Returns -1 if the game produced nothing usable. */
#define MAX_PLIES   320
#define MAX_REC     320
#define SEARCH_MS   100000   /* effectively unlimited; depth is the real cap */

static int play_game(TT *tt, int depth, int rng_open_plies,
                     char fens[][160], int *scores, int *sides,
                     double *result_white) {
    Position pos;
    pos_startpos(&pos);
    tt_clear(tt);

    uint64_t hist[MAX_PLIES + 8];
    int hist_len = 0;
    int rec = 0;
    double res = 0.5;            /* default: draw (move cap / unresolved) */
    int decisive_streak = 0, decisive_winner = -1;

    for (int ply = 0; ply < MAX_PLIES; ply++) {
        Move legal[MAX_MOVES];
        int nlegal = pos_gen_moves(&pos, legal);

        if (nlegal == 0) {                       /* mate or stalemate */
            if (pos_in_check(&pos))
                res = (pos.side == WHITE) ? 0.0 : 1.0;   /* mover is checkmated */
            else
                res = 0.5;                                /* stalemate */
            break;
        }
        if (pos.halfmove >= 100 || insufficient_material(&pos)) { res = 0.5; break; }

        /* 3-fold repetition. */
        int reps = 0;
        for (int i = 0; i < hist_len; i++) if (hist[i] == pos.hash) reps++;
        if (reps >= 2) { res = 0.5; break; }

        Move chosen;
        int score = 0;

        if (ply < rng_open_plies) {
            /* Random opening ply for game variety — not recorded. */
            chosen = legal[rand() % nlegal];
        } else {
            bool have = false;
            chosen = find_best_move_score(&pos, SEARCH_MS, depth, tt, &score, &have);
            if (!have || chosen == MOVE_NONE) chosen = legal[0];

            /* Decisive-score adjudication: 4 consecutive plies at |score| >= 2000
               (mate-ish / crushing) ends the game to save self-play time. */
            int winner = (score >= 0) == (pos.side == WHITE) ? WHITE : BLACK;
            if (abs(score) >= 2000) {
                if (decisive_winner == winner) decisive_streak++;
                else { decisive_winner = winner; decisive_streak = 1; }
            } else { decisive_streak = 0; decisive_winner = -1; }

            /* Record only QUIET, non-check, non-mate-score positions: these are
               what a static eval should learn. Captures/promos and in-check
               nodes are intentionally dropped (their eval is search territory). */
            bool quiet = MOVE_CAPTURE(chosen) == PIECE_NONE
                      && MOVE_PROMO(chosen)   == PIECE_NONE;
            if (quiet && !pos_in_check(&pos) && abs(score) < 2000 && rec < MAX_REC) {
                pos_to_fen(&pos, fens[rec], 160);
                scores[rec] = score;
                sides[rec]  = pos.side;
                rec++;
            }

            if (decisive_streak >= 4) {
                res = (decisive_winner == WHITE) ? 1.0 : 0.0;
                break;
            }
        }

        if (hist_len < MAX_PLIES + 8) hist[hist_len++] = pos.hash;
        Position next;
        pos_do_move(&pos, chosen, &next);
        pos = next;
    }

    if (rec == 0) return -1;
    *result_white = res;
    return rec;
}

void datagen_run_cmd(const char *args, TT *tt) {
    /* Parse: <out-path> [games] [depth] [seed] */
    char path[1024] = {0};
    const char *q = args;
    while (*q == ' ' || *q == '\t') q++;
    if (!*q) { fprintf(stderr, "usage: datagen <out-path> [games] [depth] [seed]\n"); return; }

    int i = 0;
    while (*q && *q != ' ' && *q != '\t' && i < (int)sizeof(path) - 1) path[i++] = *q++;
    path[i] = '\0';
    while (*q == ' ' || *q == '\t') q++;

    int games = 100, depth = 8;
    unsigned seed = 1;
    if (*q) { games = atoi(q); while (*q && *q != ' ') q++; while (*q == ' ') q++; }
    if (*q) { depth = atoi(q); while (*q && *q != ' ') q++; while (*q == ' ') q++; }
    if (*q) { seed  = (unsigned)strtoul(q, NULL, 10); }
    if (games < 1)  games = 1;
    if (depth < 1)  depth = 1;
    if (depth > 20) depth = 20;

    srand(seed);

    FILE *out = fopen(path, "w");
    if (!out) { fprintf(stderr, "datagen: cannot open %s for writing\n", path); return; }

    fprintf(stderr, "datagen: %d games, depth %d, seed %u -> %s\n",
            games, depth, seed, path);

    static char fens[MAX_REC][160];
    static int  scores[MAX_REC], sides[MAX_REC];
    long total_pos = 0;

    for (int g = 0; g < games; g++) {
        int rng_open = 4 + (rand() % 5);   /* 4..8 random opening plies */
        double res_w;
        int rec = play_game(tt, depth, rng_open, fens, scores, sides, &res_w);
        if (rec < 0) continue;

        for (int k = 0; k < rec; k++) {
            /* Result from this position's side-to-move POV. */
            double r = (sides[k] == WHITE) ? res_w : (1.0 - res_w);
            fprintf(out, "%s | %d | %.1f\n", fens[k], scores[k], r);
        }
        total_pos += rec;

        if ((g + 1) % 10 == 0 || g + 1 == games) {
            fprintf(stderr, "datagen: %d/%d games, %ld positions\n",
                    g + 1, games, total_pos);
            fflush(out);
        }
    }

    fclose(out);
    fprintf(stderr, "datagen: done — %ld positions written to %s\n", total_pos, path);
}
