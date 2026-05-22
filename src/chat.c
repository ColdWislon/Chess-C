#include "chat.h"
#include "search.h"
#include "build_id.h"
#include <stdio.h>
#include <stdlib.h>

#define SWING_THRESHOLD_CP 150
#define MATE_WINDOW        1000

static const char *role_name(int piece) {
    switch (piece) {
        case PAWN:   return "pawn";
        case KNIGHT: return "knight";
        case BISHOP: return "bishop";
        case ROOK:   return "rook";
        case QUEEN:  return "queen";
        case KING:   return "king";
        default:     return "piece";
    }
}

static void fmt_score(int cp, char *buf, int n) {
    float v = cp / 100.0f;
    if (v >= 0.0f) snprintf(buf, n, "+%.2f", v);
    else           snprintf(buf, n, "%.2f", v);
}

/* Feature-brag lines, one per slot. The chat layer picks slot
   `(move_number - 2) % N_FEATURE_LINES` on quiet moves (when no
   higher-priority chat fires). Keep each line short — Lichess chat truncates
   ~140 chars and a one-liner reads better. */
static int feature_line(const ChatContext *ctx, int slot,
                        char *out, int out_size) {
    switch (slot) {
        case 0:
            snprintf(out, out_size,
                     "search: PVS + null-move + LMR + aspiration windows");
            return 1;
        case 1:
            if (ctx->threads > 1)
                snprintf(out, out_size,
                         "lazy SMP across %d threads, %d MB hash",
                         ctx->threads, ctx->hash_mb);
            else
                snprintf(out, out_size,
                         "single-threaded search, %d MB transposition table",
                         ctx->hash_mb);
            return 1;
        case 2:
            if (ctx->tb_largest > 0)
                snprintf(out, out_size,
                         "syzygy %d-piece endgame tables loaded",
                         ctx->tb_largest);
            else
                snprintf(out, out_size,
                         "magic-bitboard move generation");
            return 1;
        case 3:
            if (ctx->was_book_move)
                snprintf(out, out_size, "still in polyglot opening book");
            else if (ctx->book_loaded)
                snprintf(out, out_size,
                         "PeSTO tapered eval, polyglot book on the shelf");
            else
                snprintf(out, out_size,
                         "PeSTO tapered eval: material + PSTs + mobility");
            return 1;
        case 4:
            if (ctx->depth_reached > 0)
                snprintf(out, out_size,
                         "last search: depth %d", ctx->depth_reached);
            else
                snprintf(out, out_size,
                         "quiescence search with SEE pruning");
            return 1;
    }
    return 0;
}
#define N_FEATURE_LINES 5

int chat_build(const ChatContext *ctx, char *out, int out_size) {
    /* ── Move 1: greeting summarizes what's actually compiled in.
       Tries to be concise (<140 chars) and to mention the headline features:
       search, SMP, hash, Syzygy if loaded, book if loaded. */
    if (ctx->is_first_move) {
        char smp[40];
        if (ctx->threads > 1)
            snprintf(smp, sizeof(smp), ", lazy SMP %dT", ctx->threads);
        else
            smp[0] = '\0';

        char tb[40];
        if (ctx->tb_largest > 0)
            snprintf(tb, sizeof(tb), ", syzygy %d-piece", ctx->tb_largest);
        else
            tb[0] = '\0';

        snprintf(out, out_size,
                 "engine ready, build %s, PVS + null-move + LMR%s, %dMB hash%s%s",
                 BUILD_GIT_SHA,
                 smp,
                 ctx->hash_mb,
                 tb,
                 ctx->book_loaded ? ", polyglot book" : "");
        return 1;
    }

    /* ── Mate scores ── (highest priority — tactical news first) */
    if (ctx->have_score) {
        int s = ctx->score;
        if (s >= SEARCH_MATE - MATE_WINDOW) {
            int plies = SEARCH_MATE - s;
            if (plies < 1) plies = 1;
            int moves = (plies + 1) / 2;
            snprintf(out, out_size, "mate in %d", moves);
            return 1;
        }
        if (s <= -SEARCH_MATE + MATE_WINDOW) {
            int plies = SEARCH_MATE + s;
            if (plies < 1) plies = 1;
            int moves = (plies + 1) / 2;
            snprintf(out, out_size, "facing mate in %d", moves);
            return 1;
        }
    }

    /* ── Promotion ── */
    int promo = MOVE_PROMO(ctx->our_move);
    if (promo != PIECE_NONE) {
        char sbuf[32] = "";
        if (ctx->have_score) {
            char tmp[16];
            fmt_score(ctx->score, tmp, sizeof(tmp));
            snprintf(sbuf, sizeof(sbuf), ", eval %s", tmp);
        }
        snprintf(out, out_size, "promote to %s%s", role_name(promo), sbuf);
        return 1;
    }

    /* ── Heavy-piece capture ── */
    int captured = MOVE_CAPTURE(ctx->our_move);
    if (captured == QUEEN || captured == ROOK) {
        char sbuf[32] = "";
        if (ctx->have_score) {
            char tmp[16];
            fmt_score(ctx->score, tmp, sizeof(tmp));
            snprintf(sbuf, sizeof(sbuf), ", eval %s", tmp);
        }
        snprintf(out, out_size, "captures %s%s", role_name(captured), sbuf);
        return 1;
    }

    /* ── Syzygy root hit — the engine knew the perfect answer. */
    if (ctx->was_root_tb_hit) {
        const char *verdict =
              (ctx->tb_wdl_band > 0) ? "winning"
            : (ctx->tb_wdl_band < 0) ? "losing"
                                     : "drawn";
        snprintf(out, out_size, "syzygy tablebase: %s", verdict);
        return 1;
    }

    /* ── Just stepped out of the opening book — announce it once. */
    if (ctx->just_left_book) {
        if (ctx->depth_reached > 0)
            snprintf(out, out_size,
                     "out of book — searched to depth %d", ctx->depth_reached);
        else
            snprintf(out, out_size, "out of book");
        return 1;
    }

    /* ── Eval swing ── (suppressed when last_score was in the mate band —
       the raw delta would be absurd and the mate-priority block above
       already said what mattered). */
    if (ctx->have_score && ctx->have_last) {
        if (ctx->last_score >=  SEARCH_MATE - MATE_WINDOW ||
            ctx->last_score <= -SEARCH_MATE + MATE_WINDOW) {
            /* fall through to rotation below */
        } else {
            int delta = ctx->score - ctx->last_score;
            int abs_delta = delta < 0 ? -delta : delta;
            if (abs_delta >= SWING_THRESHOLD_CP) {
                char prev[16], cur[16];
                fmt_score(ctx->last_score, prev, sizeof(prev));
                fmt_score(ctx->score,      cur,  sizeof(cur));
                snprintf(out, out_size, "eval %s -> %s", prev, cur);
                return 1;
            }
        }
    }

    /* ── Feature rotation: lowest-priority slot. Fires when nothing
       tactical is happening — fills early-game silence with a couple of
       brief feature mentions, then goes quiet after one full cycle.
       move_number 2 → slot 0, move 3 → slot 1, etc.
       We cap at one full cycle: after that, return silent so the chat
       doesn't keep spamming feature lines all game. */
    if (ctx->move_number >= 2) {
        int idx = ctx->move_number - 2;
        if (idx < N_FEATURE_LINES)
            return feature_line(ctx, idx, out, out_size);
    }

    return 0;
}
