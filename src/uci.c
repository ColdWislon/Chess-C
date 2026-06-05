#include "uci.h"
#include "board.h"
#include "search.h"
#include "perft.h"
#include "chat.h"
#include "bench.h"
#include "syzygy.h"
#include "texel.h"
#include "nnue.h"
#include "datagen.h"
#include "eval.h"
#include "build_id.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <pthread.h>
#include <time.h>
#include <limits.h>

static int parse_time(const char *line, const Position *pos,
                      int move_overhead, int min_thinking, int *depth_out) {
    /* Parse go command time fields; returns time in ms for our side.
       *depth_out is set if "depth N" present, else 0.
       move_overhead: ms subtracted from the budget as a comms/lag safety margin.
       min_thinking:  floor on returned time, capped by what's actually safe so
                      honoring it can never cause a time forfeit. */
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

    long alloc;
    long max_safe;   /* ceiling that honoring min_thinking must not exceed */

    if (movetime > 0) {
        /* GUI gave us an explicit per-move budget. Spend (movetime - overhead)
           and treat that same value as the ceiling — exceeding it risks forfeit. */
        max_safe = movetime - move_overhead;
        if (max_safe < 1) max_safe = 1;
        alloc = max_safe;
    } else {
        long our_time = (pos->side == WHITE) ? wtime : btime;
        long opp_time = (pos->side == WHITE) ? btime : wtime;
        long inc      = (pos->side == WHITE) ? winc  : binc;
        if (our_time <= 0) our_time = 10000;
        if (opp_time <= 0) opp_time = our_time;
        long moves_left = (movestogo > 0) ? movestogo : 30;
        alloc = our_time / moves_left + (inc > 0 ? inc * 3 / 4 : 0);
        alloc = alloc * 9 / 10;

        /* Time pressure bonus: when we have significantly more time than the
           opponent, spend more to play stronger and maintain the pressure.
           Scale: up to +50% extra when we have 3x their time. */
        if (opp_time > 0 && our_time > opp_time * 2) {
            long ratio = our_time / (opp_time > 0 ? opp_time : 1);
            if (ratio > 4) ratio = 4;
            alloc = alloc * (100 + (ratio - 1) * 25) / 100;
        }

        /* Never spend more than half our remaining time on a single move —
           guards against blowing the clock when inc*3/4 swamps our_time at
           low time. min_thinking_time must respect this same ceiling. */
        max_safe = our_time / 2;
        if (max_safe < 1) max_safe = 1;
        if (alloc > max_safe) alloc = max_safe;
        alloc -= move_overhead;
    }

    /* Floor at Minimum Thinking Time, but never above max_safe — a bullet game
       with min_thinking=5000 must not let that option cost us the game. */
    if (alloc < min_thinking) alloc = min_thinking;
    if (alloc > max_safe)     alloc = max_safe;
    if (alloc < 1)            alloc = 1;
    return (int)alloc;
}

static long uci_ms_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

/* Per-game chat state. Lives across moves and resets on ucinewgame. Updated by
   the search thread when a real (non-ponder) move is produced; the main loop
   only mutates it on ucinewgame (which first stops+joins any running search),
   so there is no concurrent access. */
typedef struct {
    bool is_first_move;
    int  last_score;
    bool have_last;
    int  move_number;
    bool prev_was_book;
    int  ponder_searches;   /* go-ponder commands this game */
    int  ponder_hits;       /* of those, confirmed by ponderhit */
} ChatState;

/* Everything the background search thread needs. Filled by the `go` handler
   before pthread_create; the thread owns a snapshot of the position (history
   stays valid because `position`/`ucinewgame` stop+join the thread first). */
typedef struct {
    Position        pos;            /* snapshot of the root */
    const uint64_t *history;
    int             history_len;
    long            budget_ms;      /* time budget; applied to the deadline on ponderhit */
    int             depth;
    int             threads;
    TT             *tt;
    OpeningBook    *book;
    ChatState      *cs;
    bool            started_ponder; /* this search began as `go ponder` */
} SearchJob;

typedef struct {
    SearchJob     *job;
    SearchControl *ctl;
    atomic_bool   *is_ponder;       /* true while pondering; cleared on ponderhit */
} ThreadArg;

/* Background search thread: probe book, run the search under the shared
   SearchControl, then emit exactly one `bestmove`. While pondering we must NOT
   emit bestmove until `ponderhit` or `stop` arrives — if the search finished
   early (book/TB/mate/max-depth) we block here until one of them does. The
   chat line + per-game chat-state update only happen for a genuinely played
   move (ponder hit converts is_ponder→false; a ponder miss leaves it true). */
static void *search_thread_fn(void *arg) {
    ThreadArg *ta = arg;
    SearchJob *j  = ta->job;

    Move best     = book_probe(j->book, &j->pos);
    bool was_book = (best != MOVE_NONE);
    int  score    = 0;
    bool have_score = false;
    int  depth_reached = 0;

    if (!was_book)
        best = find_best_move_smp_ctl(&j->pos, ta->ctl,
                                      j->depth > 0 ? j->depth : 64,
                                      j->tt, j->threads,
                                      j->history, j->history_len,
                                      &score, &have_score, &depth_reached);

    /* Hold bestmove until ponderhit (is_ponder cleared) or stop. */
    while (atomic_load_explicit(ta->is_ponder, memory_order_relaxed) &&
           !atomic_load_explicit(&ta->ctl->stop, memory_order_relaxed)) {
        struct timespec ts = { 0, 2 * 1000 * 1000 }; /* 2 ms poll */
        nanosleep(&ts, NULL);
    }
    bool ponder = atomic_load_explicit(ta->is_ponder, memory_order_relaxed);

    /* Tally ponder outcomes (this thread is the only writer of cs; the next
       search thread sees these via the join in stop_and_join). A ponder search
       that ended with is_ponder still set was a miss; one cleared by ponderhit
       was a hit and produced the move we're about to play. Book moves don't
       count — returning a book move instantly isn't pondering. */
    bool ponder_real = j->started_ponder && !was_book;
    bool ponder_hit  = ponder_real && !ponder;
    if (ponder_real) j->cs->ponder_searches++;
    if (ponder_hit)  j->cs->ponder_hits++;

    if (best != MOVE_NONE) {
        if (!ponder) {
            ChatState *cs = j->cs;
            ChatContext cc = {
                .our_move        = best,
                .score           = score,
                .have_score      = have_score,
                .last_score      = cs->last_score,
                .have_last       = cs->have_last,
                .is_first_move   = cs->is_first_move,
                .move_number     = cs->move_number,
                .was_book_move   = was_book,
                .just_left_book  = cs->prev_was_book && !was_book,
                .was_root_tb_hit = syzygy_last_root_was_hit(),
                .tb_wdl_band     = syzygy_last_root_wdl_band(),
                .depth_reached   = depth_reached,
                .threads         = j->threads,
                .hash_mb         = j->tt->mb,
                .tb_largest      = syzygy_largest(),
                .book_loaded     = (j->book != NULL),
                .nnue_loaded     = nnue_is_loaded(),
                /* Announce only the first few hits so a high-hit-rate game
                   doesn't spam; the rate keeps climbing in the shown lines. */
                .ponder_hit      = ponder_hit && (cs->ponder_hits <= 3),
                .ponder_hits     = cs->ponder_hits,
                .ponder_searches = cs->ponder_searches,
            };
            char chat_buf[200];
            if (chat_build(&cc, chat_buf, sizeof(chat_buf)))
                printf("info string CHAT %s\n", chat_buf);
            cs->is_first_move = false;
            if (have_score) { cs->last_score = score; cs->have_last = true; }
            cs->prev_was_book = was_book;
            cs->move_number++;
        }
        char buf[6];
        move_to_uci(best, buf);
        printf("bestmove %s\n", buf);
    } else {
        printf("bestmove 0000\n");
    }
    fflush(stdout);
    return NULL;
}

/* Stop the running search (if any) and join its thread. Safe to call when no
   search is running. Setting stop releases a parked ponder thread; the thread
   still prints a bestmove (python-chess discards it on a miss/abort). */
static void stop_and_join(bool *have_thread, pthread_t thr, SearchControl *ctl) {
    if (*have_thread) {
        atomic_store_explicit(&ctl->stop, true, memory_order_relaxed);
        pthread_join(thr, NULL);
        *have_thread = false;
    }
}

void uci_run(OpeningBook *book) {
    TT tt;
    tt_init(&tt, 64); /* 64 MB default */

    Position pos;
    pos_startpos(&pos);

    int  threads          = 1;   /* honored if Threads UCI option is set */
    int  move_overhead    = 30;  /* ms; matches declared default */
    int  min_thinking_time = 20; /* ms; matches declared default */

    /* Chat-supporting state. move_number is 1 on the first move we play in a
       game and bumps after every bestmove. prev_was_book tracks the previous
       move's source so we can fire "out of book" exactly once at the
       transition. All reset on ucinewgame. Updated by the search thread. */
    ChatState cs = {
        .is_first_move = true, .last_score = 0, .have_last = false,
        .move_number = 1, .prev_was_book = false,
    };

    /* Background-search control. The search runs on `search_thr` so the main
       loop can process `ponderhit` / `stop` while it runs. `ctl` carries the
       (mutable) deadline + stop flag; `is_ponder` is true between `go ponder`
       and the matching `ponderhit`/`stop`. */
    pthread_t     search_thr;
    bool          have_thread = false;
    SearchControl ctl;
    atomic_bool   is_ponder;
    SearchJob     job;
    ThreadArg     targ = { .job = &job, .ctl = &ctl, .is_ponder = &is_ponder };
    atomic_init(&ctl.deadline_ms, 0);
    atomic_init(&ctl.stop, false);
    atomic_init(&is_ponder, false);

    /* Game-history hashes: positions we passed through before the current
       root, in chronological order. Seeded into the search's rep_stack so
       2-fold via game history is visible (the search's own stack only knows
       positions reached inside its own tree). */
    uint64_t game_history[1024];
    int      game_history_len = 0;

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
            printf("option name SyzygyPath type string default /home/bertrand/syzygy/\n");
            printf("option name SyzygyProbeLimit type spin default 7 min 0 max 7\n");
            printf("option name Syzygy50MoveRule type check default true\n");
            printf("option name SyzygyProbeDepth type spin default 1 min 0 max 100\n");
            printf("option name EvalFile type string default <none>\n");
            printf("option name Ponder type check default false\n");
            printf("uciok\n");
            fflush(stdout);

        } else if (strcmp(line, "isready") == 0) {
            printf("readyok\n");
            fflush(stdout);

        } else if (strncmp(line, "setoption", 9) == 0) {
            stop_and_join(&have_thread, search_thr, &ctl);
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
                } else if (strncmp(name, "Move Overhead", 13) == 0) {
                    int v = atoi(val);
                    if (v < 0)    v = 0;
                    if (v > 5000) v = 5000;
                    move_overhead = v;
                } else if (strncmp(name, "Minimum Thinking Time", 21) == 0) {
                    int v = atoi(val);
                    if (v < 0)    v = 0;
                    if (v > 5000) v = 5000;
                    min_thinking_time = v;
                } else if (strncmp(name, "SyzygyPath", 10) == 0) {
                    /* Strip leading spaces and trailing whitespace from val. */
                    while (*val == ' ') val++;
                    char path[1024];
                    size_t n = strlen(val);
                    while (n > 0 && (val[n-1] == ' ' || val[n-1] == '\t' ||
                                     val[n-1] == '\r' || val[n-1] == '\n')) n--;
                    if (n >= sizeof(path)) n = sizeof(path) - 1;
                    memcpy(path, val, n); path[n] = '\0';
                    bool ok = syzygy_init(path);
                    fprintf(stderr,
                            "info string Syzygy %s (largest %d-piece) path=%s\n",
                            ok ? "loaded" : "FAILED to load",
                            syzygy_largest(), path);
                    fflush(stderr);
                } else if (strncmp(name, "SyzygyProbeLimit", 16) == 0) {
                    int v = atoi(val);
                    if (v < 0) v = 0;
                    if (v > 7) v = 7;
                    syzygy_set_probe_limit(v);
                } else if (strncmp(name, "Syzygy50MoveRule", 16) == 0) {
                    /* UCI check option: "true" / "false" string. */
                    bool use = (strncmp(val, "true", 4) == 0);
                    syzygy_set_50_move_rule(use);
                } else if (strncmp(name, "SyzygyProbeDepth", 16) == 0) {
                    syzygy_set_probe_depth(atoi(val));
                } else if (strncmp(name, "EvalFile", 8) == 0) {
                    /* Load an .nnue net. Strip surrounding whitespace; the
                       sentinel "<none>" (or empty) unloads back to HCE. */
                    while (*val == ' ') val++;
                    char path[1024];
                    size_t n = strlen(val);
                    while (n > 0 && (val[n-1] == ' ' || val[n-1] == '\t' ||
                                     val[n-1] == '\r' || val[n-1] == '\n')) n--;
                    if (n >= sizeof(path)) n = sizeof(path) - 1;
                    memcpy(path, val, n); path[n] = '\0';
                    if (n == 0 || strcmp(path, "<none>") == 0) {
                        nnue_free();
                        fprintf(stderr, "info string NNUE: unloaded (using HCE)\n");
                    } else if (!nnue_load(path)) {
                        fprintf(stderr, "info string NNUE: failed to load %s\n",
                                path);
                    }
                    fflush(stderr);
                }
            }

        } else if (strcmp(line, "ucinewgame") == 0) {
            stop_and_join(&have_thread, search_thr, &ctl);
            pos_startpos(&pos);
            tt_clear(&tt);
            cs.is_first_move = true;
            cs.have_last     = false;
            cs.move_number   = 1;
            cs.prev_was_book = false;
            cs.ponder_searches = 0;
            cs.ponder_hits     = 0;
            game_history_len = 0;
            /* Re-emit build id at game start so the dashboard's journal
               scan (which only looks back a few hundred lines) finds it
               even when the engine has been running for a long time. */
            fprintf(stderr, "info string BUILD %s\n", BUILD_GIT_SHA);
            fflush(stderr);

        } else if (strncmp(line, "position", 8) == 0) {
            stop_and_join(&have_thread, search_thr, &ctl);
            const char *p = line + 8;
            while (*p == ' ') p++;
            /* New root position: clear the history walk we're about to rebuild. */
            game_history_len = 0;
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
                        /* Push the position we're leaving BEFORE doing the
                           move — gives us the chronological list of prior
                           positions, with the current root excluded. */
                        if (game_history_len <
                            (int)(sizeof(game_history)/sizeof(game_history[0])))
                            game_history[game_history_len++] = pos.hash;
                        Position next;
                        pos_do_move(&pos, mv, &next);
                        pos = next;
                    }
                }
            }

        } else if (strncmp(line, "go", 2) == 0) {
            /* Any prior search should already be finished (the GUI waits for
               bestmove), but join defensively to keep state race-free. */
            stop_and_join(&have_thread, search_thr, &ctl);

            int depth = 0;
            int time_ms = parse_time(line + 2, &pos, move_overhead,
                                     min_thinking_time, &depth);
            /* `go ponder …` → search the predicted position on the opponent's
               clock. python-chess sends the clock fields too, so parse_time
               already gives us the budget to apply when `ponderhit` lands. */
            bool pondering = (strstr(line, "ponder") != NULL);

            /* Bump TT generation so entries written before this search are
               considered "stale" and can be replaced even by shallow new ones.
               This is the standard TT-aging trick. */
            tt_new_generation(&tt);

            atomic_store_explicit(&ctl.stop, false, memory_order_relaxed);
            atomic_store_explicit(&ctl.deadline_ms,
                pondering ? PONDER_DEADLINE_INF : (uci_ms_now() + time_ms),
                memory_order_relaxed);
            atomic_store_explicit(&is_ponder, pondering, memory_order_relaxed);

            job.pos         = pos;            /* snapshot the root */
            job.history     = game_history;
            job.history_len = game_history_len;
            job.budget_ms   = time_ms;
            job.depth       = depth;
            job.threads     = threads;
            job.tt          = &tt;
            job.book        = book;
            job.cs          = &cs;
            job.started_ponder = pondering;

            if (pthread_create(&search_thr, NULL, search_thread_fn, &targ) == 0) {
                have_thread = true;
            } else {
                /* Spawn failed — run synchronously so we never silently stall
                   (blocks the loop, but pondering is moot in this fallback). */
                search_thread_fn(&targ);
            }

        } else if (strcmp(line, "ponderhit") == 0) {
            /* Opponent played the predicted move: convert the infinite-deadline
               ponder search into a normal timed one. The same search keeps its
               TT/PV/history and stops when the budget elapses. */
            if (have_thread) {
                atomic_store_explicit(&ctl.deadline_ms,
                                      uci_ms_now() + job.budget_ms,
                                      memory_order_relaxed);
                atomic_store_explicit(&is_ponder, false, memory_order_relaxed);
            }

        } else if (strcmp(line, "stop") == 0) {
            /* Abort the current search; the thread emits its best move so far.
               On a ponder miss the GUI discards it and sends a fresh position. */
            if (have_thread)
                atomic_store_explicit(&ctl.stop, true, memory_order_relaxed);

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

        } else if (strncmp(line, "texel", 5) == 0) {
            /* `texel <path-to-epd> [material|full]`
               Runs Texel tuning on the given corpus. Synchronous, can take
               hours on the Pi for a large corpus — best run on a faster
               host. The tuned values get pasted back into src/eval.c.
               Mode: `material` (10 params, ~minutes, hard to overfit) or
               `full` (778 params, default — material + all 12 PSTs). */
            const char *q = line + 5;
            while (*q == ' ') q++;
            if (!*q) {
                fprintf(stderr, "usage: texel <epd-path> [material|full]\n");
            } else {
                /* Split off the path (first whitespace-bounded token) so
                   the mode flag isn't smuggled into the filename. */
                char path[1024];
                int  i = 0;
                while (*q && *q != ' ' && *q != '\t' && i < (int)sizeof(path)-1)
                    path[i++] = *q++;
                path[i] = '\0';
                while (*q == ' ' || *q == '\t') q++;
                TexelMode mode = TEXEL_MODE_FULL;
                if (strncmp(q, "material", 8) == 0) mode = TEXEL_MODE_MATERIAL;
                texel_run(path, mode);
            }

        } else if (strcmp(line, "eval") == 0) {
            /* Print the static eval of the current position (side-to-move POV).
               Routes through NNUE when a net is loaded — used by the C/Python
               parity gate (tools/nnue/check_parity.py). */
            printf("eval %d\n", evaluate(&pos));
            fflush(stdout);

        } else if (strncmp(line, "datagen", 7) == 0) {
            /* `datagen <out-path> [games] [depth]` — self-play data generation
               for NNUE training. CPU-heavy: run on WSL, never alongside a live
               game (see CLAUDE.md idle check). */
            datagen_run_cmd(line + 7, &tt);

        } else if (strncmp(line, "perft", 5) == 0) {
            int depth = atoi(line + 6);
            if (depth < 1) depth = 1;
            uint64_t nodes = perft(&pos, depth);
            printf("nodes %llu\n", (unsigned long long)nodes);
            fflush(stdout);

        } else if (strncmp(line, "nnuetest", 8) == 0) {
            /* `nnuetest <depth>` — walk a perft tree and verify that the
               incremental accumulator update produces the same bytes as a
               full refresh on every move. Depth 3-4 covers enough move
               types to be a strong correctness check. */
            int depth = atoi(line + 8);
            if (depth < 1) depth = 3;
            (void)nnue_acc_self_test(&pos, depth);

        } else if (strcmp(line, "quit") == 0) {
            stop_and_join(&have_thread, search_thr, &ctl);
            break;
        }
    }

    stop_and_join(&have_thread, search_thr, &ctl);
    tt_free(&tt);
}
