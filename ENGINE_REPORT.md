# chess-c — Engine Code Review & Presentation Notes

A walk-through of the engine for use as speaker notes. Organized by component,
with file/line pointers, the *why* behind each design choice, and the rough
strength contribution. Sources read: `src/main.c`, `board.{c,h}`, `search.{c,h}`,
`eval.c`, `tt.{c,h}`, `uci.c`, `opening.c`, `chat.c`, `bench.c`.

> **Snapshot note (this doc is from 2026-05-21, commit `c4773b6`).** Two big
> things shipped *after* this snapshot and are NOT reflected throughout below:
> (1) a **from-scratch NNUE eval** (768→256→1, +207 Elo over the hand-crafted
> eval) is now the production default — see `src/nnue.{c,h}`, `docs/nnue-format.md`;
> (2) **pondering** (`go ponder` on the opponent's clock) — the search now runs
> on a background thread. The live bridge is also **`asynclio-bot`** (the
> python-chess `lichess-bot-c.service` was removed). CLAUDE.md / README.md are
> the current source of truth.

---

## 1. Headline numbers

| Metric | Value |
|---|---|
| Total source | ~4,050 lines of C engine + ~4,600 lines vendored Fathom |
| Search core (`search.c`) | 830 lines |
| Move generation (`board.c`) | 876 lines, magic bitboards |
| Eval (`eval.c`) | 283 lines, PeSTO tapered |
| Syzygy adapter (`syzygy.c`) | 180 lines wrapping Fathom (`external/tbprobe.c`) |
| Hardware | Raspberry Pi 4, 4 cores, 384 MB hash in production, 939 MB Syzygy 3-4-5 on disk |
| Playing strength | ≈ Stockfish Skill 9–10, ~1900–2000 Elo at 2 s/move (gauntlet 2026-05-17) — TB endgames now perfect |
| Time control | Rapid + classical, runs 24/7 on Lichess as **rpiBot73** |

---

## 2. Architecture overview

```
                 main.c
                   │
        ┌──────────┼──────────────┐
        │          │              │
     board.c     uci.c        opening.c
     (movegen)  (protocol)   (Polyglot book)
        │          │              │
        │       search.c          │
        │     (PVS + LMR +        │
        │      null-move + …)     │
        │          │              │
        │        tt.c             │
        │     (xor-key TT)        │
        │          │              │
        └────►  eval.c  ◄─────────┘
              (PeSTO tapered)
                   │
                chat.c
        (Lichess relay messages)
```

Single binary, no external dependencies beyond libc + pthreads + libm. Talks
UCI on stdin/stdout; the asynclio-bot bridge connects that to the Lichess API.

---

## 3. Move generation (`board.c`)

**Representation.** Bitboard per (color, piece type): `pieces[2][6]`. Moves are
packed into a single `uint32_t` (`board.h:46`) — from, to, piece, capture,
promotion, flags. Eight ALU operations is enough to pack/unpack everything.

**Sliders: magic bitboards** (`board.c:82–215`). Per-square magic constants are
generated at startup by trial-and-error (`init_magic_sq`, `board.c:140`) until
a multiplier collapses all relevant-occupancy subsets into distinct indices.
The tables (`rook_tbl[64][4096]`, `bishop_tbl[64][512]`) live in static memory
— ~2 MB total. Rook/bishop attacks then cost one mask + multiply + shift +
table lookup.

> Note: the CLAUDE.md description "magic-free sliders" is stale — the actual
> implementation is magic bitboards, which is the modern fast path.

**Prefetch hints** (`board.c:201–205`). Inside the mobility loop and SEE swap
chain, the engine issues `__builtin_prefetch` on the *next* slider's bucket
while the current one is still computing. Speculative magic-multiply paid in
parallel with the previous lookup hides L2 latency on the Pi 4's modest cache.

**Zobrist hashing** (`board.c:9–33`). Xorshift-based PRNG produces independent
random keys for (color × piece × square), EP files, castling masks, and side.
Hash is updated incrementally inside `pos_do_move` — never recomputed.

**Static Exchange Evaluation** (`board.c:355+`). Iteratively replaces the
attacker with the smallest available defender, recomputing slider attacks
against the shrinking occupancy bitboard. Naturally handles x-rays, EP, and
in-chain promotions. SEE uses **decoupled piece values** (`SEE_VAL[]`,
`board.c:322`) so SEE results don't shift with eval tuning.

**Mobility** (`board.c:266–311`). Per-piece weights: N=4, B=5, R=2, Q=1. The
weights are not "how strong the piece is" — they reflect *the marginal value
of one extra square*. A queen already reaches half the board; an extra square
matters less than for a knight that only ever sees 8. Enemy-pawn-attacked
squares are excluded from the destination mask.

**Validated by perft.** Startpos depth 5 = 4,865,609 — matches the canonical
value. `make test` runs this in CI.

---

## 4. Search (`search.c`)

This is the heart of the engine. The whole thing is one ~800-line file —
deliberately, because every prune interacts with every other prune.

### 4.1 Iterative deepening + aspiration windows

`run_id` (`search.c:497`) iterates depth 1, 2, 3, … and after depth 4 narrows
the search window to ±50 cp around the previous score. On fail-high/fail-low
the window widens exponentially (`window *= 2`) until it sticks. Most
iterations land inside the window, which gives a free 5–10 % node reduction
versus full-width.

### 4.2 Principal Variation Search (PVS)

For each move:
- First move: full window `(α, β)` (it's the PV move from move ordering)
- Subsequent moves: zero-window `(α, α+1)` — cheap "is this better than α?" probe
- Re-search full window only if the zero-window probe says yes

Most non-PV moves are refuted instantly by the zero-window probe — that's the
whole reason PVS beats plain α-β.

### 4.3 Pruning stack

Listed in order of activation per node:

| Technique | Code | Effect |
|---|---|---|
| **TT cutoff** | `search.c:266` | Skip whole subtree if cached at sufficient depth |
| **Mate-distance pruning** | `search.c:249–254` | Tighten `α`/`β` when mate scores are bounded |
| **Check extension** | `search.c:258` | `depth++` in check — tactics where it matters |
| **Reverse futility / static null** | `search.c:288–292` | If `eval − 90·depth ≥ β`, fail-high instantly |
| **Null-move pruning** | `search.c:295–317` | Skip our turn, search at `depth − 1 − R`; if it still ≥ β, prune |
| **Late Move Pruning (LMP)** | `search.c:365–368` | At low depth, late quiets in well-ordered list ≈ unreachable |
| **Frontier futility** | `search.c:373–375` | At depth 1, skip quiets that can't reach α |
| **SEE pruning in qsearch** | `search.c:183–185` | Drop captures with losing SEE before paying legality cost |
| **Late Move Reductions (LMR)** | `search.c:395–408` | Reduce depth on later moves; re-search if surprising |

**LMR table** (`search.c:678–690`): `LMR[d][m] = round(0.5 + log(d)·log(m)/2)`.
A logarithmic shape gives smooth scaling — old constant-step formula
under-reduced at high depth and over-reduced at low depth.

**Re-search ladder** (`search.c:411–422`): reduced zero-window → full-depth
zero-window → full-window. Most moves stop at the first probe, so the ladder
costs almost nothing.

### 4.4 Move ordering

The single biggest determinant of pruning quality. Score per move
(`search.c:104–122`):

| Slot | Score | Source |
|---|---|---|
| TT move | 1,000,000 | Last best move at this hash |
| Good capture (SEE ≥ 0) | 100,000 + MVV-LVA | "Worth the trade" |
| Queen promotion | 95,000 | Always strong |
| Killer 0 | 90,000 | Quiet that cut at this ply, current iteration |
| Killer 1 | 80,000 | Previous killer (slides down) |
| Counter-move | 70,000 | "After opp played X→Y, this move cut last time" |
| Bad capture (SEE < 0) | 60,000 + MVV-LVA | Below killers, above history |
| Quiet | history[c][f][t] | Long-term cutoff statistics |

**History gravity** (`search.c:443–460`): on a beta cutoff the cutoff move
gets `bonus = depth²` pulled toward `HISTORY_MAX`, and earlier-tried quiets
that *didn't* cut get the symmetric **malus**. Self-limiting (no global ÷2
cliff). Symmetric malus is the most recent change — it speeds learning by
penalizing losers, not just rewarding winners.

**Selection sort, not full sort** (`pick_move`, `search.c:125`): we usually
take a cutoff in the first few moves. Picking only the next-best each
iteration costs O(n) instead of O(n log n), and beta-cutoff lets us skip the
tail entirely.

### 4.5 Quiescence search

(`search.c:136–206`) — only captures + queen promotions, with **two important
twists**:

1. **In-check qsearch generates all evasions** (no stand-pat). Detects mate
   correctly and prevents tactical blind-spots one ply past horizon.
2. **Pseudo-legal generation + SEE prune + late legality check**
   (`search.c:155–198`, commit `c4773b6`). Saves a `pos_do_move` +
   `sq_attacked_by` on every SEE-discarded capture, which empirically is
   ~half of them.

### 4.6 Repetition detection

Two layers:

- **Inside the search**: `is_repetition` (`search.c:88`) walks back by 2
  plies, bounded by halfmove clock (irreversible moves break the chain).
  2-fold within search treated as draw.
- **Game-history seed** (`search.c:657–668`, commit `7951f05`): UCI front-end
  collects all hashes from `position … moves …` and seeds them into the
  worker's `rep_stack` before searching. Without this the search would
  re-blunder into positions the arbiter would claim as repetition.

### 4.7 Lazy SMP

(`search.c:748–803`) N worker threads run independent iterative deepening
from the root, sharing only the TT. No work-splitting, no locking — different
move orders + aspiration jitter produce diverse trees, the strongest line
naturally wins the TT race. Stops via a single `atomic_bool`. On the Pi 4
with 4 threads, gives ~2× practical strength gain vs single-threaded.

### 4.8 Time management

(`uci.c:13–48`) Allocate `our_time / moves_left + inc·3/4`, then × 0.9. Cap
at `our_time / 2` so a big increment can't blow the clock on one move.

**Soft budget** (`search.c:621`): after each ID iteration, predict next
iteration as ~2× the last. If that wouldn't finish, don't start it. The old
formula compared *total* elapsed to budget and aborted at ~50% regardless of
where the last ply landed — leaving half the thinking time on the table.
Fixed 2026-05-17.

---

## 5. Transposition table (`tt.{c,h}`)

**XOR trick** (`tt.c:11–19`). Each entry stores
`xor_key = real_key ^ hash(data)`. On probe, recompute `hash(data)` and XOR
back; if it doesn't match the original key, reject. Two benefits:

1. **Lock-free race safety** for Lazy SMP — torn writes (some fields from
   thread A, others from B) fail the check and are discarded.
2. **Sentinel correctness** — empty slot has all-zero fields, so
   `entry_hash = 0` and any non-zero probe key fails. No magic "is this
   slot empty?" flag needed.

**Replacement scheme** (`tt.c:42–64`). Same position → always refresh;
deeper → replace shallower; same-depth from older `go` → replace stale.
The TT *ages* — each search bumps `generation`, so a 64-MB table doesn't
fossilize with results from move 1 of the game.

**Cached static eval** (`tt.h:13`, `search.c:285`). Each entry caches
`evaluate(pos)` from first visit. Saves a full eval pass on revisits used
for null-move / reverse-futility margins. Saves ~5% of total eval cost.

**Hashfull stride sampling** (`tt.c:66–77`). UCI hashfull is 1000-bucket
sample *strided across the full table* instead of just the first 1000
entries — far more representative.

---

## 6. Evaluation (`eval.c`)

Pure PeSTO-style tapered eval. ~280 lines, no neural net.

**Terms:**
- Material (MG/EG pairs, `eval.c:9–10`)
- Piece-square tables (MG/EG pairs for all 6 pieces, `eval.c:16–140`)
- **Mobility** with per-piece weights (`board.c:266`)
- **King safety** — pawn shield count × 10 cp, MG only (`eval.c:228–238`)
- **Pawn structure** — doubled, isolated, passed (rank-scaled MG/EG arrays)
- **Bishop pair** — +30 MG / +50 EG (`eval.c:270`)

**Tapered blend** (`eval.c:279–280`):
```
score = (mg · phase + eg · (24 − phase)) / 24
```
Phase = sum of non-pawn material weights (N=1, B=1, R=2, Q=4), clamped to 24.
This is why the king's PST naturally transitions from "stay back" (MG) to
"centralize" (EG) — no explicit king-walk code.

**Key choice: passed pawn bonuses are *much* larger in EG.** A 6th-rank
passer in EG is worth ~rook-value (160 cp), versus 90 cp in MG. Reflects
that endgame defenders can't easily stop a runner.

---

## 7. UCI front-end (`uci.c`)

- Standard UCI handshake; declares `Hash`, `Threads`, `Move Overhead`,
  `Minimum Thinking Time`, plus Syzygy options (`SyzygyPath`,
  `SyzygyProbeLimit`, `Syzygy50MoveRule`, `SyzygyProbeDepth`). All are
  honored — `Move Overhead` is subtracted from the time budget,
  `Minimum Thinking Time` is a floor (capped by what's actually safe so it
  can't forfeit), and `SyzygyPath` re-inits Fathom on change.
- Build-id embedded via auto-generated `src/build_id.h` (git SHA) — emitted
  as `info string BUILD …` on `uci` and `ucinewgame` so the dashboard always
  finds it in journalctl regardless of process age.
- Custom `bench [depth]` and `bench_timed [ms]` commands for the in-repo
  A/B perf infrastructure.
- Robust FEN fallback: malformed FEN → reset to startpos (prevents the rest
  of the session from operating on a kingless zeroed board).

---

## 7.5 Syzygy tablebases (`syzygy.{c,h}` + `external/tbprobe.c`)

Endgame perfection for ≤5 piece positions. Fathom (CC0, Jon Dart) is vendored
in `external/` and compiled into the binary; our `syzygy.c` is a ~180-line
adapter that translates between the engine's `Position` struct and Fathom's
flat per-type bitboard API.

**On-disk footprint.** 290 files (.rtbw WDL + .rtbz DTZ), 939 MB, downloaded
from `tablebase.lichess.ovh` to `/home/bertrand/syzygy/`. Loaded at startup;
silent if absent (engine still runs without).

**Two probe sites:**

1. **Root probe** (`syzygy_probe_root`, called once at the top of
   `find_best_move_smp_hist`). Uses Fathom's `tb_probe_root` which is
   DTZ-aware and not thread-safe — must run *before* spawning SMP workers.
   On a hit, the engine returns immediately at depth 1, saves the entire
   move budget, and emits a synthetic info line with `tbhits 1` so the
   dashboard sees the TB resolution.
2. **In-search WDL probe** (`syzygy_probe_wdl`, in `alpha_beta` after the
   TT lookup). Gated by `ply > 0`, `depth >= SyzygyProbeDepth`,
   `halfmove == 0`, no castling rights. Returns ±SEARCH_MATE/2 — large
   enough to dominate normal eval, small enough that mate-distance pruning
   keeps working. Lets 7-piece positions reduce to 5-piece subtrees with
   perfect WDL info.

**Score mapping** (`wdl_to_score`):
| Fathom WDL | Engine score |
|---|---|
| TB_WIN | +SEARCH_MATE/2 (450,000 cp) |
| TB_CURSED_WIN | +100 cp (folded to 0 when Syzygy50MoveRule=true) |
| TB_DRAW | 0 |
| TB_BLESSED_LOSS | -100 cp (folded to 0 with the rule) |
| TB_LOSS | -SEARCH_MATE/2 |

**UCI options:**
- `SyzygyPath` (string, default `/home/bertrand/syzygy/`) — re-inits Fathom
  on change.
- `SyzygyProbeLimit` (0–7, default 7) — caps max piece count probed.
- `Syzygy50MoveRule` (check, default true) — folds cursed/blessed into draw,
  matching what the arbiter actually claims.
- `SyzygyProbeDepth` (0–100, default 1) — skip the in-search probe at low
  remaining depth (qsearch leaves are too cheap to be worth a probe).

**Safety.** Fathom is built with `-DNDEBUG -w` — assertions disabled, no
warnings — so a malformed FEN can't crash the engine. Real lichess games
only feed legal positions through the normal `pos_gen_moves` legality
filter, so this purely protects manual UCI input.

**Observed effect.** In a 6-piece KRPP vs KR test, the search reached
depth 11 with `tbhits 2057` (each hit is a perfect subtree resolution that
replaces a full alpha-beta call). For 3-5 piece roots, search becomes a
single TB probe — the engine returns instantly with the right answer.

## 8. Opening book (`opening.c`)

Loads any Polyglot `.bin`. Position is converted to the canonical 781-key
Polyglot hash (verified against `0x463b96181691fc9c` for startpos), book is
binary-searched, weighted-random pick among matching entries.

**Important subtlety** (`opening.c:50–58`): EP square is only hashed if a
friendly pawn can *legally* capture there — matches `EnPassantMode::Legal`
in python-chess and the standard Polyglot generators. Without this, every
double-push would mis-hash and book hits would silently miss.

> Bug history: pre-2026-05-17 the engine used a blocked piece-key layout and
> inverted side-to-move. Result: no book hit ever fired despite "Opening
> book loaded." in the logs. Fixed and verified against the canonical
> startpos key.

---

## 9. Engine chat (`chat.c`)

Differentiator versus most bots: emits a one-line natural-language comment
per move, posted to the Lichess game chat. Priority chain:

1. Greeting on first move (`engine ready, build <sha>`)
2. Mate detection (`mate in N` / `facing mate in N`)
3. Promotion (`promote to queen, eval +5.23`)
4. Queen/rook capture (`captures queen, eval +3.18`)
5. Eval swing ≥ 150 cp (`eval +0.45 -> -1.21`)

**Edge case handled** (commit `d3d8bca`, `chat.c:76–83`): when the *previous*
score was in the mate band, the raw delta is ~90,000 cp and would print
something like "eval +8999.97 → +0.50". Skip in that case — the mate-priority
block already said what mattered.

---

## 10. Testing & performance infrastructure

| Tool | Purpose |
|---|---|
| `make test` | perft startpos depth 4 + 5 (197,281 / 4,865,609). Run after any movegen change. |
| `make bench-baseline` | Snapshot current binary as `chess-engine-c.baseline` |
| `make bench-compare` | Interleaved A↔B runs, reports min-time delta. Pi-throttling aware. |
| `bench` (UCI) | Single fixed-depth run over 8 positions. `total_nodes` is deterministic — proves a refactor didn't change the search tree. |
| `bench_timed` (UCI) | Same but time-bounded. Useful for time-management changes. |
| Gauntlet vs Stockfish | Out-of-repo `gauntlets/` runs the bot vs SF Skill 1–20 |

**Why interleaved-min**: the Pi 4 throttles aggressively. Running A then B
back-to-back lets thermal state contaminate the measurement. Interleaving
keeps both binaries on the same heat curve, then reporting the *min* of each
gives the no-throttle ground truth.

---

## 11. Production deployment

- Runs under `asynclio-bot.service` on Raspberry Pi 4, 24/7 (the engine is
  spawned per-game; pondering enabled). The earlier `lichess-bot-c.service` was
  removed 2026-06-05.
- 384 MB hash, 4 threads (Lazy SMP)
- Account: **rpiBot73** on lichess.org
- Accepts rated standard rapid + classical
- Matchmaking: when idle, challenges other bots at 10+0 / 10+5 / 15+10
- Web dashboard at port 8080 — live game state, system metrics, build SHA,
  per-game charts, repertoire stats

Built-in safety: dashboard exposes `/api/status` so any CPU-heavy command
(benchmarks, gauntlets, restarts) can check `rpibot73` is idle before
starting — restarting mid-game forfeits the game.

---

## 12. What's notable to highlight in a talk

1. **It's small.** ~3,850 lines for a 1900–2000 Elo engine. PeSTO + classical
   search + good move ordering goes a long way.
2. **Move ordering is everything.** 7-tier ranking (TT → good cap → promo
   → killer → counter → bad cap → history) with symmetric history gravity.
3. **The TT XOR trick** — locks would dominate runtime in Lazy SMP. Storing
   `key ^ hash(data)` is a one-line solution that simultaneously handles
   torn writes AND the "is this slot empty?" sentinel problem.
4. **Tapered eval explains a lot of "intelligence" for free.** The king
   walks to the center in the endgame not because we coded it, but because
   the EG king PST rewards centralization and the phase factor smoothly
   activates it.
5. **Magic bitboards on a Pi.** Tiny ARM cores, but `mask × magic >> shift`
   + L1-resident table is still the fastest slider attack lookup. Prefetch
   hints hide the 4 MB-table miss penalty.
6. **Engine chat.** The bot *talks* during games — mate countdowns,
   "captures queen", eval swings. Visible, fun, and zero strength cost.
7. **Repetition seed from game history.** A subtle bug class: search-internal
   rep detection can blunder into a draw the arbiter will claim from prior
   game positions. UCI front-end feeds the move walk in, fixing this.
8. **Built-in A/B bench infrastructure.** Every speed change is validated
   with interleaved-min runs on the deploy target itself, with deterministic
   node-count regression detection.
9. **Syzygy 3-4-5 piece tablebases.** Vendored Fathom (CC0). Root probe is
   DTZ-aware and runs *before* SMP workers fork (Fathom's DTZ is documented
   not thread-safe). In-search WDL probe gates further search on perfect
   information. In a typical 5-piece endgame the entire move budget is
   saved — the engine returns the right answer at depth 1 with zero search
   nodes. The 939 MB on disk is the difference between blundering a won
   KRPKR and converting it cleanly.

---

## 13. Known gaps (be honest in the talk)

- ~~**No neural-network eval (NNUE).**~~ **DONE (after this snapshot).** A
  from-scratch 768→256→1 NNUE eval now ships as the production default and
  measured **+207 Elo** over the hand-crafted eval (200-game WSL gauntlet). It
  uses an incremental accumulator on the search hot path; the classical PeSTO
  eval remains as a fallback. See the NNUE section in CLAUDE.md.
- **6-7 piece tablebases not on disk.** 5-piece is ~939 MB and fits easily;
  6-piece is ~150 GB and 7-piece is ~17 TB. Most practical endgames are
  covered by 3-5 piece, so the diminishing returns kick in fast.

---

*Generated 2026-05-21 from a read of the working tree at commit `c4773b6`.*
