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

int chat_build(const ChatContext *ctx, char *out, int out_size) {
    if (ctx->is_first_move) {
        snprintf(out, out_size, "engine ready, build %s", BUILD_GIT_SHA);
        return 1;
    }

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

    if (ctx->have_score && ctx->have_last) {
        /* If last_score was in the mate band but current isn't, the raw delta
           (~±90000+ cp) blows past SWING_THRESHOLD_CP and would emit a
           visually absurd "eval +8999.97 -> +0.50" line. Skip in that case —
           current=mate is already handled by the mate-priority block above. */
        if (ctx->last_score >=  SEARCH_MATE - MATE_WINDOW ||
            ctx->last_score <= -SEARCH_MATE + MATE_WINDOW)
            return 0;
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

    return 0;
}
