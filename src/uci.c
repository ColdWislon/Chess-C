#include "uci.h"
#include "board.h"
#include "search.h"
#include "perft.h"
#include "chat.h"
#include "bench.h"
#include "build_id.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>

static int parse_time(const char *line, const Position *pos, int *depth_out) {
    /* Parse go command time fields; returns time in ms for our side.
       *depth_out is set if "depth N" present, else 0. */
    long wtime = -1, btime = -1, winc = 0, binc = 0;
    long movetime = -1, movestogo = -1, depth = 0;
    const char *p = line;
    while (*p) {
        while (*p == ' ') p++;
        /* Order matters: longer prefixes first so "winc" doesn't match "wtime". */
        if      (strncmp(p,"wtime",5)==0)    { p+=5; wtime     = atol(p); }
        else if (strncmp(p,"btime",5)==0)    { p+=5; btime     = atol(p); }
        else if (strncmp(p,"winc",4)==0)     { p+=4; winc      = atol(p); }
        else if (strncmp(p,"binc",4)==0)     { p+=4; binc      = atol(p); }
        else if (strncmp(p,"movetime",8)==0) { p+=8; movetime  = atol(p); }
        else if (strncmp(p,"movestogo",9)==0){ p+=9; movestogo = atol(p); }
        else if (strncmp(p,"depth",5)==0)    { p+=5; depth     = atol(p); }
        while (*p && *p != ' ') p++;
    }
    if (depth_out) *depth_out = (int)depth;
    if (depth > 0) return 60000;  /* generous cap, depth limit handles termination */
    if (movetime > 0) {
        long t = movetime - 50;
        return (int)(t > 1 ? t : 1);
    }
    long our_time = (pos->side == WHITE) ? wtime : btime;
    long inc      = (pos->side == WHITE) ? winc  : binc;
    if (our_time <= 0) our_time = 10000;
    long moves_left = (movestogo > 0) ? movestogo : 30;
    long alloc = our_time / moves_left + (inc > 0 ? inc * 3 / 4 : 0);
    alloc = alloc * 9 / 10;
    /* Never spend more than half our remaining time on a single move — guards
       against blowing the clock when inc*3/4 swamps our_time at low time. */
    if (alloc > our_time / 2) alloc = our_time / 2;
    if (alloc < 100) alloc = 100;
    return (int)alloc;
}

void uci_run(OpeningBook *book) {
    TT tt;
    tt_init(&tt, 64); /* 64 MB default */

    Position pos;
    pos_startpos(&pos);

    bool is_first_move = true;
    int  last_score    = 0;
    bool have_last     = false;
    int  threads       = 1;   /* honored if Threads UCI option is set */

    char line[4096];
    while (fgets(line, sizeof(line), stdin)) {
        /* strip newline */
        line[strcspn(line, "\r\n")] = '\0';

        if (strcmp(line, "uci") == 0) {
            printf("id name ChessEngine-C build %s\n", BUILD_GIT_SHA);
            printf("id author ChessEngineBot\n");
            /* Echo build id as info-string so the dashboard's journal parser
               picks it up at startup without needing a UCI-level handshake. */
            fprintf(stderr, "info string BUILD %s\n", BUILD_GIT_SHA);
            fflush(stderr);
            printf("option name Hash type spin default 64 min 1 max 4096\n");
            printf("option name Threads type spin default 1 min 1 max 512\n");
            printf("option name Move Overhead type spin default 30 min 0 max 5000\n");
            printf("option name Minimum Thinking Time type spin default 20 min 0 max 5000\n");
            printf("uciok\n");
            fflush(stdout);

        } else if (strcmp(line, "isready") == 0) {
            printf("readyok\n");
            fflush(stdout);

        } else if (strncmp(line, "setoption", 9) == 0) {
            /* Parse: setoption name <NAME> value <VAL> */
            char *name = strstr(line, "name");
            char *val  = strstr(line, "value");
            if (name && val) {
                name += 5;
                val  += 6;
                if (strncmp(name, "Hash", 4) == 0) {
                    int mb = atoi(val);
                    if (mb >= 1 && mb <= 4096) {
                        tt_free(&tt);
                        tt_init(&tt, mb);
                    }
                } else if (strncmp(name, "Threads", 7) == 0) {
                    int t = atoi(val);
                    if (t < 1)   t = 1;
                    if (t > 512) t = 512;
                    threads = t;
                }
            }

        } else if (strcmp(line, "ucinewgame") == 0) {
            pos_startpos(&pos);
            tt_clear(&tt);
            is_first_move = true;
            have_last     = false;
            /* Re-emit build id at game start so the dashboard's journal
               scan (which only looks back a few hundred lines) finds it
               even when the engine has been running for a long time. */
            fprintf(stderr, "info string BUILD %s\n", BUILD_GIT_SHA);
            fflush(stderr);

        } else if (strncmp(line, "position", 8) == 0) {
            const char *p = line + 8;
            while (*p == ' ') p++;
            if (strncmp(p, "startpos", 8) == 0) {
                pos_startpos(&pos);
                p += 8;
            } else if (strncmp(p, "fen", 3) == 0) {
                p += 3;
                while (*p == ' ') p++;
                if (!pos_from_fen(&pos, p)) {
                    /* Malformed FEN — fall back to startpos so subsequent
                       commands don't operate on a zeroed (kingless) board. */
                    pos_startpos(&pos);
                }
                /* skip past the FEN (6 tokens) */
                for (int tok = 0; tok < 6 && *p; tok++) {
                    while (*p && *p != ' ') p++;
                    while (*p == ' ') p++;
                }
            }
            /* apply moves */
            const char *m = strstr(p, "moves");
            if (m) {
                m += 5;
                while (*m) {
                    while (*m == ' ') m++;
                    if (!*m) break;
                    char token[8] = {0};
                    int i = 0;
                    while (*m && *m != ' ' && i < 7) token[i++] = *m++;
                    Move mv = move_from_uci(&pos, token);
                    if (mv != MOVE_NONE) {
                        Position next;
                        pos_do_move(&pos, mv, &next);
                        pos = next;
                    }
                }
            }

        } else if (strncmp(line, "go", 2) == 0) {
            int depth = 0;
            int time_ms = parse_time(line + 2, &pos, &depth);

            /* Bump TT generation so entries written before this search are
               considered "stale" and can be replaced even by shallow new ones.
               This is the standard TT-aging trick. */
            tt_new_generation(&tt);

            int  score      = 0;
            bool have_score = false;

            Move best = book_probe(book, &pos);
            if (best == MOVE_NONE)
                best = find_best_move_smp(&pos, time_ms,
                                          depth > 0 ? depth : 64,
                                          &tt, threads, &score, &have_score);

            if (best != MOVE_NONE) {
                ChatContext cc = {
                    .our_move      = best,
                    .score         = score,
                    .have_score    = have_score,
                    .last_score    = last_score,
                    .have_last     = have_last,
                    .is_first_move = is_first_move,
                };
                char chat_buf[160];
                if (chat_build(&cc, chat_buf, sizeof(chat_buf))) {
                    printf("info string CHAT %s\n", chat_buf);
                }
                is_first_move = false;
                if (have_score) {
                    last_score = score;
                    have_last  = true;
                }

                char buf[6];
                move_to_uci(best, buf);
                printf("bestmove %s\n", buf);
            } else {
                printf("bestmove 0000\n");
            }
            fflush(stdout);

        } else if (strncmp(line, "bench_timed", 11) == 0) {
            int ms = 1000;
            const char *q = line + 11;
            while (*q == ' ') q++;
            if (*q) {
                int v = atoi(q);
                if (v >= 50 && v <= 60000) ms = v;
            }
            run_bench_timed(ms, &tt);

        } else if (strncmp(line, "bench", 5) == 0) {
            int d = 8;
            const char *q = line + 5;
            while (*q == ' ') q++;
            if (*q) {
                int v = atoi(q);
                if (v > 0 && v <= 30) d = v;
            }
            run_bench(d, &tt);

        } else if (strncmp(line, "perft", 5) == 0) {
            int depth = atoi(line + 6);
            if (depth < 1) depth = 1;
            uint64_t nodes = perft(&pos, depth);
            printf("nodes %llu\n", (unsigned long long)nodes);
            fflush(stdout);

        } else if (strcmp(line, "quit") == 0) {
            break;
        }
    }

    tt_free(&tt);
}
