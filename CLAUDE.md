# Chess Engine Project (C)

A classical UCI chess engine in C, running as an always-on bot on Raspberry Pi 4, connected to Lichess via lichess-bot.

## Repository layout

```
/home/bertrand/
├── chess-c/                  # C engine (this repo)
│   ├── src/
│   │   ├── main.c            # Entry point, loads opening book
│   │   ├── board.c/.h        # Move generation, position state, Zobrist
│   │   ├── eval.c/.h         # Material + PSTs + mobility + king safety
│   │   ├── search.c/.h       # Iterative deepening, PVS, null-move, LMR, killers, history
│   │   ├── tt.c/.h           # Transposition table
│   │   ├── opening.c/.h      # Polyglot book probe
│   │   ├── chat.c/.h         # Builds engine chat lines for lichess-bot
│   │   ├── perft.c/.h        # Move generation tests
│   │   ├── uci.c/.h          # UCI protocol handler
│   │   └── poly_keys.h       # Polyglot 781 Zobrist constants
│   ├── book.bin              # Polyglot opening book
│   ├── Makefile              # `make release`, `make test`, `make clean`
│   ├── tools/gen_poly_keys.py
│   ├── match.py / compare.py # A/B match harnesses (legacy)
│   └── chess-engine-c        # Built binary
├── lichess-bot/              # Python bridge: engine ↔ Lichess API
│   ├── config-c.yml          # CANONICAL — used by lichess-bot-c.service (rpiBot73)
│   ├── config.yml            # legacy — used by the disabled lichess-bot.service
│   └── venv/
└── chess-dashboard/          # Web dashboard (port 8080)
```

## Build

```bash
cd /home/bertrand/chess-c
make release        # → ./chess-engine-c (gcc -O3 -march=native)
make test           # perft startpos at depth 4 (197281) and 5 (4865609)
make debug          # → ./chess-engine-c-dbg (-O0 -g)
make clean
```

`src/poly_keys.h` is checked in (canonical 781 Polyglot constants). Regenerate with:
```bash
python3 tools/gen_poly_keys.py > src/poly_keys.h
```

## Run engine manually (UCI)

```bash
./chess-engine-c
# then type UCI commands:
uci
isready
position startpos
go movetime 3000
quit
```

## Services

```bash
sudo systemctl status lichess-bot-c chess-dashboard
sudo systemctl restart lichess-bot-c
sudo journalctl -u lichess-bot-c -f      # live engine logs (depth/score/nodes/nps/seldepth)
sudo journalctl -u chess-dashboard -f
```

`lichess-bot-c.service` is canonical (runs `--config config-c.yml`, account rpiBot73). The legacy `lichess-bot.service` (`config.yml`, MegaBot73) was disabled 2026-05-16 — don't accidentally re-enable it.

Dashboard: http://192.168.1.66:8080

## Don't disturb live games

The Pi has 4 cores. The running engine subprocess uses all of them under `Threads=4`. Restarting the service OR running CPU-heavy benchmarks/matches alongside a live game will hurt — or outright abandon — that game. **Before any of the following, confirm rpiBot73 is idle:**

- `sudo systemctl restart lichess-bot-c` (kills the engine subprocess → forfeits the ongoing game)
- `make bench-compare`, `make bench-compare-timed`, `make bench` (competes for the same 4 cores → live engine times out / blunders)
- `python3 ladder_match.py` or any gauntlet vs Stockfish (long-running, full CPU)
- `match.py` / `compare.py` A/B match harnesses

**Idle check (one-liner):**
```bash
curl -s http://127.0.0.1:8080/api/status?bot=rpibot73 \
  | python3 -c "import sys,json; g=json.load(sys.stdin).get('current_game'); print('IDLE' if not g else f'PLAYING {g[\"id\"]}')"
```

If it prints `PLAYING <id>`, wait. Polite poll interval: 30-60 s. The game URL is `https://lichess.org/<id>` if you want to watch it finish. Building (`make release`) does not need to wait — only the binary on disk is changed; the running service keeps using the binary it `exec()`'d at startup, until the next restart.

## Bot account

- Username: **rpiBot73** on lichess.org (legacy MegaBot73 account is idle since 2026-05-16)
- Token stored in: `/home/bertrand/lichess-bot/config-c.yml` (the canonical config — `config.yml` is the disabled legacy unit)
- Accepts: rapid and classical, rated, standard variant
- Matchmaking: enabled — challenges other bots when idle, rotates between 10+0 / 10+5 / 15+10

## Engine architecture

| Layer | Detail |
|---|---|
| Move generation | Bitboards, magic-bitboard sliders (`board.c`, magics generated at startup) |
| Search | Iterative deepening, PVS, LMR, null-move pruning, killers, history, counter-moves, aspiration windows, check extensions |
| Quiescence | Captures only, MVV-LVA ordering, queen-promotion bonus, SEE pruning |
| Transposition table | 64 MB source default; **384 MB in production** via `Hash` in `config-c.yml`. Zobrist-keyed, replace-by-depth, XOR-key for lock-free SMP. |
| Repetition | 2-fold within search treated as draw; halfmove clock bounded; rep_stack seeded from game history |
| Evaluation | Material + piece-square tables + mobility + king safety + pawn structure |
| Opening book | Polyglot `.bin` file — `/home/bertrand/chess-c/book.bin` (path hardcoded in `main.c`, falls back to `./book.bin`) |
| Tablebases | **Syzygy 3-4-5-piece** via Fathom (`external/tbprobe.c`, vendored CC0). Files at `/home/bertrand/syzygy/` (~939 MB). Root probe is DTZ-aware; in-search probe is WDL-only. |
| Time management | `total_time / moves_left * 0.9`; soft budget predicts next iteration as 2× the last and aborts before starting it if that wouldn't fit in the remaining budget (fixed 2026-05-17 — old formula aborted at 50% of total budget regardless of iteration time) |
| UCI options | Hash (1-4096 MB), Threads (Lazy SMP), Move Overhead, Minimum Thinking Time, SyzygyPath, SyzygyProbeLimit, Syzygy50MoveRule, SyzygyProbeDepth — all honored. |

## Engine chat (lichess relay)

The engine emits one line per move on stdout:

```
info string CHAT <message>
```

lichess-bot's `engine_wrapper.py` reads `result.info["string"]`, strips the `CHAT ` prefix, and posts via the Lichess chat API after the move is made. Priority chain in `src/chat.c` (only the highest-priority message that matches gets emitted per move):

1. First move of new game → `engine ready, build <sha>, PVS + null-move + LMR, lazy SMP <N>T, <H>MB hash, syzygy <P>-piece, polyglot book` — built dynamically from what's actually compiled in and loaded.
2. Mate detected / facing mate → `mate in N` / `facing mate in N`
3. Promotion → `promote to <piece>, eval ±X.XX`
4. Capture of queen/rook → `captures <piece>, eval ±X.XX`
5. Syzygy root probe hit → `syzygy tablebase: winning|drawn|losing`
6. First non-book move after a book sequence → `out of book — searched to depth N`
7. Eval swing ≥ 150 cp vs previous turn → `eval ±X.XX -> ±Y.YY`
8. **Feature rotation** (low-priority filler — fires only when 2-7 didn't and we're still on the first few moves): cycles through 5 short feature mentions (search techniques → SMP/hash → Syzygy → eval/book → search depth) on moves 2..6, then goes silent for the rest of the game. The point is to surface what the engine does in the chat early on without spamming the user.

State (`is_first_move`, `last_score`, `have_last`, `move_number`, `prev_was_book`) lives in `uci.c::uci_run` and resets on `ucinewgame`. python-chess only keeps the **last** `info string` of a search in `info["string"]`, so the engine emits exactly one CHAT line per move (right before `bestmove`).

### Chat smoke test (no Lichess needed)

```bash
printf 'uci\nucinewgame\nposition startpos\ngo movetime 200\nposition fen rnb1kbnr/pppp1ppp/8/4p3/4P2q/5N2/PPPP1PPP/RNBQKB1R w KQkq - 1 3\ngo movetime 600\nquit\n' \
  | ./chess-engine-c 2>/dev/null | grep -E "^(info string|bestmove)"
```

Expect: a `CHAT engine ready ...` greeting on the first move, then a `CHAT captures queen ...` on the second.

## Key implementation notes

- Piece encoding (board.h): `PAWN=0 KNIGHT=1 BISHOP=2 ROOK=3 QUEEN=4 KING=5 PIECE_NONE=6`. Different from Rust/shakmaty (which started at 1).
- `Move` is a packed `uint32_t` — see encoding comment in `board.h:46`. Use the `MOVE_FROM/TO/PIECE/CAPTURE/PROMO/FLAGS` macros, never bit-mask manually.
- Search returns scores from side-to-move POV in centipawns. Mate scores are stored in TT as ply-independent values; converted on probe/store.
- `info depth … score cp …` goes to **stderr** (parsed from journalctl by the dashboard). Only UCI responses + `info string CHAT …` go to stdout.
- `chess-engine-c.service` doesn't exist as a separate unit — the engine is launched as a child process by `lichess-bot-c.service`.

## Syzygy tablebases

Implemented via vendored Fathom (`external/tbprobe.c`, CC0). Loaded at startup from `/home/bertrand/syzygy/` (default — override via `SyzygyPath` UCI option). 3-4-5 piece WDL+DTZ on disk (~939 MB, 290 files from `tablebase.lichess.ovh`).

- **Root probe** (`syzygy_probe_root`) is called once at the top of `find_best_move_smp_hist` before any worker is spawned (Fathom's DTZ probe is documented not thread-safe). On a hit, the engine returns immediately at depth 1 with `tbhits 1` in the info line — saves the entire move budget.
- **In-search WDL probe** is in `alpha_beta` after the TT lookup, gated by `ply > 0`, `depth >= SyzygyProbeDepth` (default 1), `halfmove == 0`, no castling rights. Returns ±SEARCH_MATE/2 for wins/losses — high enough to dominate eval, low enough that mate-distance pruning keeps working.
- **UCI options**: `SyzygyPath` (re-initializes Fathom on change), `SyzygyProbeLimit` (0–7), `Syzygy50MoveRule` (true folds cursed-win/blessed-loss into draw), `SyzygyProbeDepth` (skip probe at low remaining depth).
- Fathom is built with `-w -DNDEBUG` so a malformed FEN can't trip an internal assertion. Real lichess games only feed legal positions.

## Texel-style eval tuning

`src/texel.{c,h}` implements coordinate-descent over the material + PST values (778 parameters), fitted against a labeled EPD corpus. UCI command:

```bash
make corpus               # ~50 MB, 750k positions from Ethereal's quiet-labeled.epd
printf 'texel quiet-labeled.epd\nquit\n' | ./chess-engine-c
# → snapshots ./texel-snapshot.txt after every pass; paste the arrays back into src/eval.c
```

The tuner refits the sigmoid `K` once at start (compensates for our eval's cp scale being different from Stockfish's), then runs passes of ±step probes per parameter; step halves when no parameter improves, exits when step reaches 0. Snapshot file is written every pass so SIGINT doesn't lose work.

**On a Pi 4, the full corpus is slow** — coordinate descent on 778 params × 750k positions is ~30 min/pass. Run on a faster host (eval is portable), paste the snapshot, rebuild, gauntlet-test the new binary, commit if it improved.

## Known gaps

- No neural-network eval (NNUE). Classical eval ceiling is ~2200–2400 Elo with perfect tuning; NNUE adds ~400+ Elo but requires a training pipeline.
- 6-7 piece tablebases not on disk (5-piece is ~939 MB; 6-piece would be ~150 GB).

## Lichess matchmaking — known issues

**100 bot-vs-bot games per day limit:** Lichess caps each bot at 100 games vs other bots per 24 hours. Active bots (maia, GarboBot, sargon, turkjs, Jibbby, halcyonbot) hit this limit by midnight European time. The counter resets around 07:35 each morning. Challenges fail with `"played 100 games against other bots today"` until then.

**Permanent block list** (bots that never accept, configured in `config-c.yml`):
- `maia2200_10n` — not accepting challenges
- `ChessChildren`, `honzovy-sachy-2` — don't accept bot challenges
- `DarkOnWeakBot` — not accepting at the moment
- `Demolito_L6` — wants classical only
- `anti-bot` — does not accept

**Rate limiting — two distinct flavors:**

1. **`/api/stream/event` 429** — triggered by restarting lichess-bot in tight loops (the event stream gets reopened on every restart). `RestartSec=60` in the service file prevents the systemd auto-restart cycle from hitting this. Recovery: stop, kill stray procs, wait, start.

2. **`/api/challenge/<bot>` 429** — triggered by issuing outgoing challenges too fast. Surfaces as `ERROR {'error': 'Too many requests.` in `matchmaking.py:100` and applies even on a clean fresh start if the token has been challenging recently. Matchmaking has its own backoff (`matchmaking.py:265` schedules the next attempt) so the bot recovers without intervention — it just sits idle until the window opens. Observed cleared after ~5-10 min.

Recovery if either type is stuck:
1. `sudo systemctl stop lichess-bot`
2. `ps aux | grep lichess | grep -v grep | awk '{print $2}' | xargs sudo kill -9`
3. Wait for it to clear, then `sudo systemctl start lichess-bot`

**Stray processes:** Never run `lichess-bot.py` manually in the background while the systemd service is also running.

## Adding/updating the opening book

```bash
wget https://github.com/michaeldv/donna_opening_books/raw/master/gm2600.bin \
     -O /home/bertrand/chess-c/book.bin
sudo systemctl restart lichess-bot
```

## Testing

Always run `make test` after touching `board.c`, `search.c`, or `uci.c` — a move generation bug will corrupt everything silently.

Known perft values (startpos):
- depth 1 → 20
- depth 2 → 400
- depth 3 → 8902
- depth 4 → 197281
- depth 5 → 4865609

## A/B perf benchmark

In-repo bench for measuring perf changes without resorting to gauntlet matches. Useful as a fast pre-flight before any change to `board.c`, `search.c`, or `eval.c`.

```bash
make bench-baseline                           # snapshots current binary as chess-engine-c.baseline
# edit code
make bench-compare BENCH_DEPTH=10 BENCH_RUNS=5  # interleaves baseline ↔ current, reports min-time delta
make bench BENCH_DEPTH=10                     # standalone single run
```

The Pi 4 throttles aggressively (`vcgencmd get_throttled` is checked and warned). The compare script interleaves A↔B per iteration to keep both binaries on the same thermal state, then headlines the **min** time of each. `total_nodes` is deterministic by construction (single thread, TT cleared between positions, no time pressure) — if it differs after a change, the script flags ⚠ "search tree changed" so pure-perf claims can be distinguished from behavior shifts.

Implementation: `src/bench.{c,h}` (positions + runner), `src/search.c::search_bench` (deterministic fixed-depth wrapper), UCI `bench [depth]` command, `tools/bench_compare.sh` (interleaved-min runner). Foot-gun: `make clean` deletes `chess-engine-c.baseline`, so snapshot the baseline AFTER any clean rebuild and BEFORE editing.
