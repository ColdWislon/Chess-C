# Chess Engine Project (C)

A classical UCI chess engine in C, running as an always-on bot on Raspberry Pi 4, connected to Lichess via lichess-bot.

## Repository layout

```
/home/bertrand/
├── chess-c/                  # C engine (this repo)
│   ├── src/
│   │   ├── main.c            # Entry point, loads opening book + syzygy
│   │   ├── board.c/.h        # Move generation, position state, Zobrist, magic bitboards
│   │   ├── eval.c/.h         # Material + PSTs + mobility + king safety (mutable; texel-tunable)
│   │   ├── search.c/.h       # Iterative deepening, PVS, null-move, LMR, killers, history + PROF probes
│   │   ├── tt.c/.h           # Transposition table (XOR-key, lock-free SMP)
│   │   ├── opening.c/.h      # Polyglot book probe
│   │   ├── syzygy.c/.h       # Adapter over external/tbprobe (Fathom)
│   │   ├── texel.c/.h        # Coordinate-descent eval tuner (`texel` UCI cmd)
│   │   ├── chat.c/.h         # Builds engine chat lines for lichess-bot
│   │   ├── perft.c/.h        # Move generation tests
│   │   ├── bench.c/.h        # In-repo perf bench positions + runner
│   │   ├── uci.c/.h          # UCI protocol handler
│   │   ├── poly_keys.h       # Polyglot 781 Zobrist constants
│   │   └── build_id.h        # Auto-regen at build (git SHA)
│   ├── external/             # Vendored Fathom (Syzygy probe, CC0)
│   ├── tools/
│   │   ├── safe-restart-bot.sh        # STALE: targets removed lichess-bot-c — use `systemctl restart asynclio-bot`
│   │   ├── gen-corpus.py              # Build texel corpus from lichess games (PEP-668 safe venv)
│   │   ├── apply-texel-snapshot.py    # Auto-patch src/eval.c arrays from a snapshot
│   │   ├── wsl-gauntlet.sh            # Snapshot-based A/B gauntlet (texel-specific)
│   │   ├── wsl-ab-gauntlet.sh         # Generalized A/B gauntlet by git ref
│   │   ├── gauntlet-progress.py       # Clean-screen filter w/ ETA banner for fast-chess
│   │   ├── openings-small.epd         # 20 opening positions for variety in gauntlets
│   │   ├── bench_compare*.sh          # Pi A/B perf comparison (interleaved-min)
│   │   ├── nightly_gauntlet.sh        # Stockfish ladder match (systemd timer)
│   │   └── gen_poly_keys.py / gen_build_info.* / reenable-matchmaking.sh
│   ├── book.bin              # Polyglot opening book (gitignored, `make book`)
│   ├── Makefile              # `make release|test|clean|bench|corpus`
│   ├── match.py / compare.py # Legacy A/B harness, used by wsl-gauntlet via monkey-patch
│   └── chess-engine-c        # Built binary
├── syzygy/                   # 3-4-5-piece tablebases, ~939 MB (gitignored)
├── lichess-bot/              # Python bridge: engine ↔ Lichess API
│   ├── config-c.yml          # token source for asynclio-bot's service-run.sh (rpiBot73)
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
sudo systemctl status asynclio-bot chess-dashboard
sudo systemctl restart asynclio-bot
sudo journalctl -u asynclio-bot -f       # live engine logs (depth/score/nodes/nps/seldepth + NNUE load)
sudo journalctl -u chess-dashboard -f
```

`asynclio-bot.service` is the **sole** bridge as of 2026-06-05 (custom async bridge, based on Heiaha/asyncLio-bot, at `/home/bertrand/asyncLio-bot`, launched via `service-run.sh`, account rpiBot73). It reads the `lip_` token from `/home/bertrand/lichess-bot/config-c.yml` and runs the engine binary (`/home/bertrand/chess-c/chess-engine-c`); the engine is spawned **per-game** with `uci_options` (Threads/Hash/EvalFile) from `/home/bertrand/asyncLio-bot/config.yml`, stderr inherited to the journal, the CHAT relay ported, and **pondering enabled** (`ponder: true` → `go ponder`/`ponderhit`/`stop`; the engine thinks on the opponent's clock and emits `CHAT ponder hit N/M …` lines). The old `lichess-bot-c.service` (python-chess `lichess-bot.py --config config-c.yml`) was **removed 2026-06-05** (unit file deleted; backup at `/home/bertrand/lichess-bot/lichess-bot-c.service.removed-backup`). Its config `config-c.yml` **stays** — `service-run.sh` still reads the token from it. The older `lichess-bot.service` (`config.yml`, MegaBot73) was disabled 2026-05-16 — don't re-enable it. The dashboard reads the journal units listed in `_journal_units` (`dashboard/server.py`). **Deploy/restart: `sudo systemctl restart asynclio-bot` after an idle-check** — `tools/safe-restart-bot.sh` is stale (it names the deleted `lichess-bot-c`). To restore the old bridge: re-create the unit from the backup, `daemon-reload`, then `systemctl disable --now asynclio-bot && systemctl enable --now lichess-bot-c`. Ad-hoc foreground trial: `asyncLio-bot/run-test.sh` (stops the live bot first).

Dashboard: http://192.168.1.66:8080

## Don't disturb live games

The Pi has 4 cores. The running engine subprocess uses all of them under `Threads=4`. Restarting the service OR running CPU-heavy benchmarks/matches alongside a live game will hurt — or outright abandon — that game. **Before any of the following, confirm rpiBot73 is idle:**

- `sudo systemctl restart asynclio-bot` (kills the engine subprocess → forfeits the ongoing game)
- `make bench-compare`, `make bench-compare-timed`, `make bench` (competes for the same 4 cores → live engine times out / blunders)
- `python3 ladder_match.py` / `match.py` / `compare.py` (legacy A/B harnesses)
- `python3 /tmp/lmr-gauntlet.py` / `/tmp/texel-gauntlet.py` (Pi self-play wrappers around match.py)
- `texel <epd>` UCI command (eats one core for hours on a real corpus — run on WSL)

**Idle check (one-liner):**
```bash
curl -s http://127.0.0.1:8080/api/status?bot=rpibot73 \
  | python3 -c "import sys,json; g=json.load(sys.stdin).get('current_game'); print('IDLE' if not g else f'PLAYING {g[\"id\"]}')"
```

If it prints `PLAYING <id>`, wait. Polite poll interval: 30-60 s. The game URL is `https://lichess.org/<id>` if you want to watch it finish. Building (`make release`) does not need to wait — only the binary on disk is changed; the running service keeps using the binary it `exec()`'d at startup, until the next restart.

**Deploy a new binary:** idle-check (above), then `sudo systemctl restart asynclio-bot`, then verify the new build SHA in `journalctl -u asynclio-bot`. (`tools/safe-restart-bot.sh` is stale — it polls idle + restarts the **removed** `lichess-bot-c`; don't use it until rewritten for asynclio-bot.)

## Bot account

- Username: **rpiBot73** on lichess.org (legacy MegaBot73 account is idle since 2026-05-16)
- Token stored in: `/home/bertrand/lichess-bot/config-c.yml` (the canonical config — `config.yml` is the disabled legacy unit)
- Accepts: rapid and classical, rated, standard variant
- Matchmaking: enabled — challenges other bots when idle, rotates between 10+0 / 10+5 / 15+10

## Engine architecture

| Layer | Detail |
|---|---|
| Move generation | Bitboards, magic-bitboard sliders (`board.c`, magics generated at startup) |
| Search | Iterative deepening, PVS, LMR, NMP, RFP, futility, LMP, killers, history (gravity + symmetric malus), counter-moves, aspiration windows, check extensions |
| Quiescence | Pseudo-captures → SEE-prune → legality. In-check generates all evasions. Queen-promotion bonus. |
| Transposition table | 64 MB source default; **384 MB in production** via `Hash` in `config-c.yml`. Zobrist-keyed, replace-by-depth, XOR-key for lock-free SMP, cached static eval. |
| Repetition | 2-fold within search treated as draw; halfmove clock bounded; rep_stack seeded from game history (via `find_best_move_smp_hist`) |
| Evaluation | PeSTO tapered: material + PSTs (MG/EG, mutable for texel) + mobility + king safety + pawn structure (passed/doubled/isolated) + bishop pair |
| Opening book | Polyglot `.bin` file — `/home/bertrand/chess-c/book.bin` (path hardcoded in `main.c`, falls back to `./book.bin`) |
| Tablebases | **Syzygy 3-4-5-piece** via Fathom (`external/tbprobe.c`, vendored CC0). Files at `/home/bertrand/syzygy/` (~939 MB). Root probe is DTZ-aware; in-search probe is WDL-only. |
| Time management | `total_time / moves_left * 0.9 + inc*3/4`, capped at `our_time / 2`, minus `Move Overhead`. Soft budget aborts next iteration if predicted 2× last wouldn't fit. |
| Tuning probes | `info string PROF …` per ID iteration — ordering / lmr_research / nmp_yield / see_qprune rates + RFP/futility/LMP/asp_widens counts (see PROF section). |
| UCI options | Hash, Threads (Lazy SMP), Move Overhead, Minimum Thinking Time, SyzygyPath, SyzygyProbeLimit, Syzygy50MoveRule, SyzygyProbeDepth — all honored. |

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
- `chess-engine-c.service` doesn't exist as a separate unit — the engine is launched as a per-game child process by `asynclio-bot.service`.

## Syzygy tablebases

Implemented via vendored Fathom (`external/tbprobe.c`, CC0). Loaded at startup from `/home/bertrand/syzygy/` (default — override via `SyzygyPath` UCI option). 3-4-5 piece WDL+DTZ on disk (~939 MB, 290 files from `tablebase.lichess.ovh`).

- **Root probe** (`syzygy_probe_root`) is called once at the top of `find_best_move_smp_hist` before any worker is spawned (Fathom's DTZ probe is documented not thread-safe). On a hit, the engine returns immediately at depth 1 with `tbhits 1` in the info line — saves the entire move budget.
- **In-search WDL probe** is in `alpha_beta` after the TT lookup, gated by `ply > 0`, `depth >= SyzygyProbeDepth` (default 1), `halfmove == 0`, no castling rights. Returns ±SEARCH_MATE/2 for wins/losses — high enough to dominate eval, low enough that mate-distance pruning keeps working.
- **UCI options**: `SyzygyPath` (re-initializes Fathom on change), `SyzygyProbeLimit` (0–7), `Syzygy50MoveRule` (true folds cursed-win/blessed-loss into draw), `SyzygyProbeDepth` (skip probe at low remaining depth).
- Fathom is built with `-w -DNDEBUG` so a malformed FEN can't trip an internal assertion. Real lichess games only feed legal positions.

## PROF instrumentation (find where to gain Elo)

`search.c` emits one extra `info string PROF …` line per completed ID iteration with derived rates that surface tuning targets:

```
info string PROF ordering=94.4% lmr_research=0.5% nmp_yield=79.7%
                  see_qprune=63.6% rfp=N fut=N lmp=N asp_widens=N
```

Rule-of-thumb thresholds (the things you'd actually act on):

| Metric | Healthy | Implies if off |
|---|---|---|
| `ordering` (cutoffs_first / cutoffs) | > 90 % | < 85 % → move ordering weak |
| `lmr_research` | 10-20 % | < 5 % → LMR too conservative; > 25 % → too aggressive |
| `nmp_yield` (nullcuts / nmp_attempts) | > 50 % | < 30 % → NMP wasting work |
| `see_qprune` (see_qprunes / qnodes) | 30-70 % | < 30 % → SEE not pulling weight in qsearch |
| `asp_widens` per search | ≤ 2 | > 2 → window starting too tight |

Quick sample (5 s middlegame search on x86):
```bash
printf 'position fen r1bq1rk1/pp2bppp/2n1pn2/3p4/3P1B2/2NBP3/PP3PPP/R2QK1NR w KQ - 0 9\ngo movetime 5000\nquit\n' \
  | ./chess-engine-c 2>&1 | grep "string PROF" | tail -3
```

The PROF line is a separate `info string` so dashboard parsers keyed off the depth-line schema don't need to change.

## Autonomous improvement loop (`/improve`)

A custom Claude Code slash command that runs the full propose → review → gauntlet → push pipeline end-to-end. Project-scoped agents live at `.claude/agents/`:

| Agent | Role | Tools | Output |
|---|---|---|---|
| `engine-improver` | Read PROF + source, propose ONE small diff (≤30 LOC, src/ only) | Bash, Read, Grep | unified diff + rationale |
| `engine-reviewer` | Verify scope, run perft, vote APPROVE/REJECT | Bash, Read, Grep | verdict |
| `engine-gauntler` | Build baseline + variant, idle-check, 40-game Pi gauntlet, restart service | Bash, Read | Elo / LOS / W-D-L |
| `engine-deployer` | If Elo ≥ +20 / LOS ≥ 75%: commit + push to `auto-improve/<topic>` branch (NEVER main) | Bash, Read | branch name + next-steps |

Trigger with `/improve` in a Claude Code session. The orchestrator:
- Refuses if working tree dirty or not on main
- Reverts src/ on any stage failure
- Restarts `asynclio-bot` if the gauntlet stopped it
- Pushes to feature branch only — production never touched

The user is expected to take it from there: WSL 200+ game confirmation via `tools/wsl-ab-gauntlet.sh main <branch>`, then `git merge --ff-only` + idle-check + `sudo systemctl restart asynclio-bot` if confirmed.

## Tuning + gauntlet workflow

The combined loop for any eval/search change:

1. **PROF** → identify weak metric (e.g. `lmr_research=0.5%` → LMR too conservative)
2. **Branch** → `git checkout -b <change>-tune`, apply minimal change, push
3. **Pi gauntlet** (40 games, ~30 min, ±100 Elo confidence) → direction check
   - Build baseline + variant binaries, stop service, run `/tmp/<change>-gauntlet.py -n 40 -t 20`, restart service
4. **WSL gauntlet** (200-400 games, ~20 min on 12-core, ±25-40 Elo confidence) → magnitude
   - `tools/wsl-ab-gauntlet.sh main <change>-tune` — see "Gauntlet infrastructure" below
5. **Deploy**: merge branch → main, idle-check rpiBot73, `sudo systemctl restart asynclio-bot`. Production unaffected until then.

**What's been tried** (see git log for details, branches still on origin):
- `lmr-tune` (`0.85 + log(d)·log(m)/2.0`) — Pi +53 ± 113, WSL +9 ± 24. Marginal real gain.
- `lmr-tune-v2` (`1.0 + log(d)·log(m)/1.75`) — pending WSL confirmation.
- Texel snapshot from 87k bot games — Pi -61 ± 114, WSL -145 ± 41. **Discarded** (corpus too noisy, coordinate descent overfits).

## Gauntlet infrastructure

Two scripts, both pipe through `tools/gauntlet-progress.py` for clean output (ETA banner, no UCI noise).

**`tools/wsl-ab-gauntlet.sh <ref-A> <ref-B>` — generalized, branch-based.** Builds each ref's binary in turn, runs `fast-chess` head-to-head, restores the original branch on exit via trap. Auto-fetches missing refs from origin. Use this for any search/eval change committed to a branch.

```bash
GAMES=400 CONCURRENCY=12 tools/wsl-ab-gauntlet.sh main lmr-tune-v2
```

**`tools/wsl-gauntlet.sh [snapshot.txt]` — snapshot-based, texel-specific.** Applies a `texel-snapshot.txt` to `src/eval.c`, builds, then reverts. For testing an uncommitted snapshot file.

**Requires `fast-chess`** (`cutechess-cli` dropped from recent Ubuntu repos):
```bash
git clone --depth 1 https://github.com/Disservin/fast-chess.git ~/fast-chess
cd ~/fast-chess && make -j$(nproc)
sudo ln -sf "$(pwd)/app/fast-chess" /usr/local/bin/fast-chess
```

Output format on screen (per rating-interval block):
```
[120/400  elapsed 8:14  ETA 19:21]
  main vs lmr-tune-v2  32 - 67 - 21  Elo +21.7 ±28.4  LOS 93.4%  (main ahead)
```

**Read fast-chess's Elo numbers from engine-A's perspective** — positive = A ahead, LOS = chance A is truly better. The `(X ahead)` annotation makes it obvious.

## Texel-style eval tuning

`src/texel.{c,h}` implements coordinate-descent over the material + PST values (778 parameters), fitted against a labeled EPD corpus. With L2 regularization toward starting values so single-noisy-direction overfitting can't happen. UCI command:

```bash
make corpus                                       # generates corpus from rpiBot73's lichess games
printf 'texel quiet-labeled.epd\nquit\n' | ./chess-engine-c
# → snapshots ./texel-snapshot.txt every pass
python3 tools/apply-texel-snapshot.py             # auto-patches src/eval.c
make release
tools/wsl-ab-gauntlet.sh main HEAD                # confirm before commit/deploy
```

**Modes:** `texel <epd>` tunes material+PSTs (778 params, default). `texel <epd> material` tunes only material (10 params; fast smoke test, ~impossible to overfit).

**Corpus generation** — `tools/gen-corpus.py` pulls rated games via the Lichess API and writes `<FEN> c9 "<result>";` lines. Ethereal's `quiet-labeled.epd` (the classic corpus) is no longer hosted, so this is the replacement. Self-bootstraps a `.venv-corpus/` to side-step PEP-668 (Debian/Ubuntu refuses `pip install` against system python).

**On a Pi 4, full corpus is slow** — coordinate descent on 778 params × 87k positions is ~30 min/pass. Run on a faster host (WSL = ~10× faster), copy the snapshot back. The texel approach as currently implemented is **mildly to badly counterproductive** on bot-vs-bot corpora — see "Known gaps".

## NNUE (small perspective net)

A `768 → 256 → 1` perspective NNUE pipeline. **Status: trained + deployed.** A λ=1.0 (pure Stockfish-eval target) net beats the hand-crafted PeSTO eval by **+207 ±52 Elo** (200-game WSL gauntlet vs HCE) and ships in production (`network.nnue`, loaded on rpiBot73). The engine uses NNUE only when a `.nnue` net is loaded (UCI `EvalFile`, env `EVALFILE`, or `./network.nnue` auto-loaded at startup); otherwise it falls back to the hand-crafted PeSTO eval. `evaluate()` routes through `nnue_evaluate()` transparently — search is agnostic.

| Piece | Where |
|---|---|
| Spec (arch + quant + file format) — **single source of truth** | `docs/nnue-format.md` |
| C inference + `.nnue` loader (incremental accumulator, int-quantized) | `src/nnue.c/.h` |
| Self-play data generation (`datagen` UCI cmd) | `src/datagen.c/.h` |
| WSL PyTorch trainer (dataset/model/train/serialize) | `tools/nnue/` |
| C↔Python parity gate (pure python, runs on the Pi) | `tools/nnue/check_parity.py` |
| Workflow docs | `tools/nnue/README.md` |

Conventions: feature index `rel_color*384 + piece*64 + rel_sq` (rel_sq = `sq^56` for the black perspective); side-to-move accumulator concatenated first; quant `QA=255 QB=64 SCALE=400`; `.nnue` magic `CNUE` v1, little-endian.

**Datagen** (CPU-heavy — WSL, or Pi only when idle): `datagen <out> [games] [depth] [seed]` → `<FEN> | <cp> | <result>` lines (quiet, non-check, non-mate positions; cp + result both side-to-move POV). Use distinct seeds to parallelise.

**Incremental accumulator** (the search hot path): `nnue_acc_refresh()` builds an `NNUEAcc` from scratch (used at root); `nnue_acc_advance()` derives the post-move accumulator from the pre-move one in O(touched features) (≤4 per move); `nnue_evaluate()` is the slow refresh+eval fallback. `nnue_acc_self_test()` asserts advance==refresh byte-for-byte over a perft tree — the in-engine correctness contract for the accumulator. Gauntlet-confirmed identical in strength to full-refresh (perf-only change).

**Parity gate is the contract** — after any change to `src/nnue.c` or `tools/nnue/features.py`, run `python3 tools/nnue/check_parity.py` (no torch needed). It builds a random quantized net and asserts the engine's `eval` matches the Python int path exactly. Must pass before any trained net is meaningful.

**Net workflow**: `datagen` → Stockfish-relabel the FENs (`tools/nnue/stockfish-relabel.sh`) → `tools/nnue/setup-wsl.sh` + `train.py --lambda 1.0` on WSL → `check_parity.py` → `setoption EvalFile` → `tools/nnue/nnue-vs-hce-gauntlet.sh` vs HCE → deploy only if Elo confirms. The two levers that produced the shipped +207 net: Stockfish relabeling of training targets (+90) and a pure-eval target `--lambda 1.0` (drops the noisy self-play game-result term, +231). Deeper SF labels (depth 12 vs 10) gave **no** further gain. There's also a UCI `eval` command (prints static eval of the current position; used by the parity gate). **Deploy target is the `asyncLio-bot` bridge** (see Services) — `tools/safe-restart-bot.sh` targets the retired `lichess-bot-c`, so restart `asynclio-bot` directly after an idle-check.

## Known gaps

- **NNUE: trained + deployed (+207 Elo vs HCE), incremental accumulator landed.** The `768→256→1` net ships in production on rpiBot73 (see "NNUE" section). Classical eval ceiling was ~2200–2400 Elo; the NNUE net measured +207 ±52 over HCE at fixed 0.5s/move on x86, and the incremental accumulator keeps Pi NPS healthy enough that the gain carries to ARM. Remaining strength levers (untested): more self-play data volume/diversity, a wider net (raises Pi eval cost), or training data from stronger self-play. Deeper Stockfish labels (depth 12 vs 10) were tested and gave **no** gain — not a lever.
- **6-7-piece tablebases not on disk** (5-piece is ~939 MB; 6-piece would be ~150 GB).
- **Texel tuner's optimizer is the bottleneck.** Coordinate descent on a noisy bot-game corpus (87k positions, 778 params) reliably overfits. Tested snapshot lost -145 ± 41 Elo on WSL. Two paths to make texel useful: (a) cleaner corpus (Stockfish-vs-Stockfish self-play at depth 12+), or (b) better optimizer (Adam / finite-difference gradient descent). Until one of those, stick to manual targeted changes per PROF signals.
- **GPU on Pi 4 is not useful** — VideoCore VI is a graphics GPU (~32 GFLOPS, no tensor cores, immature compute stack). Chess search is sequential anyway; GPU dispatch overhead would dominate the eval cost. Only path to neural play on this hardware is NNUE on CPU.

## Lichess matchmaking — known issues

**100 bot-vs-bot games per day limit:** Lichess caps each bot at 100 games vs other bots per 24 hours. Active bots (maia, GarboBot, sargon, turkjs, Jibbby, halcyonbot) hit this limit by midnight European time. The counter resets around 07:35 each morning. Challenges fail with `"played 100 games against other bots today"` until then.

**Permanent block list** (bots that never accept, configured in `asyncLio-bot/config.yml` under `blocklist:`):
- `maia2200_10n` — not accepting challenges
- `ChessChildren`, `honzovy-sachy-2` — don't accept bot challenges
- `DarkOnWeakBot` — not accepting at the moment
- `Demolito_L6` — wants classical only
- `anti-bot` — does not accept

**Rate limiting — two distinct flavors:**

1. **`/api/stream/event` 429** — triggered by restarting lichess-bot in tight loops (the event stream gets reopened on every restart). `RestartSec=60` in the service file prevents the systemd auto-restart cycle from hitting this. Recovery: stop, kill stray procs, wait, start.

2. **`/api/challenge/<bot>` 429** — triggered by issuing outgoing challenges too fast. Surfaces as `ERROR {'error': 'Too many requests.` in `matchmaking.py:100` and applies even on a clean fresh start if the token has been challenging recently. Matchmaking has its own backoff (`matchmaking.py:265` schedules the next attempt) so the bot recovers without intervention — it just sits idle until the window opens. Observed cleared after ~5-10 min.

Recovery if either type is stuck:
1. `sudo systemctl stop asynclio-bot`
2. `ps aux | grep -E 'asynclio|main.py' | grep -v grep | awk '{print $2}' | xargs sudo kill -9`
3. Wait for it to clear, then `sudo systemctl start asynclio-bot`

(Line refs above point at the old python-chess `matchmaking.py`; the live bridge is asynclio-bot, whose equivalent is `asyncLio-bot/matchmaker.py` — the backoff behavior is analogous.)

**Stray processes:** Never run the bridge (`asyncLio-bot/main.py`) manually in the background while the systemd service is also running.

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
