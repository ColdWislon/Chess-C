# Chess-C

[![build](https://github.com/ColdWislon/Chess-C/actions/workflows/perft.yml/badge.svg)](https://github.com/ColdWislon/Chess-C/actions/workflows/perft.yml)
[![C](https://img.shields.io/badge/language-C11-blue)](src/)
[![Platform](https://img.shields.io/badge/platform-Raspberry%20Pi%204-c51a4a)](https://www.raspberrypi.com/)
[![Lichess](https://img.shields.io/badge/lichess-rpiBot73-green?logo=lichess)](https://lichess.org/@/rpiBot73)
[![License](https://img.shields.io/badge/license-MIT-yellow)](LICENSE)
[![Lines of code](https://img.shields.io/badge/lines-5k%20C-informational)](src/)

A classical UCI chess engine in C, running 24/7 as **[rpiBot73](https://lichess.org/@/rpiBot73)** on Lichess from a Raspberry Pi 4.

A classical alpha-beta core — bitboards, PVS, and the full bag of pruning heuristics — now paired with a small **from-scratch NNUE** eval (768→256→1, trained on the bot's own self-play, +207 Elo over the hand-crafted eval). The hand-tuned PeSTO eval remains as a fallback. It ponders on the opponent's clock and competes against the bots of Lichess.

> **Origin:** This is a "vibe coded" rewrite of an earlier C engine of mine, which was itself built following the [WikiChess](https://www.chessprogramming.org/Main_Page) (Chess Programming Wiki) approach. The current codebase was grown iteratively in collaboration with [Claude](https://claude.ai) — see [Built with Claude](#built-with-claude) below — rather than written from scratch in one pass.

---

## Features

| Layer | Implementation |
|-------|---------------|
| **Move generation** | Magic bitboard sliders, precomputed leaper tables |
| **Search** | Iterative deepening, PVS, aspiration windows, Lazy SMP (4 threads), pondering (`go ponder` on the opponent's clock) |
| **Pruning** | Null-move (adaptive R), LMR (log formula + history adjustment), LMP, reverse futility, extended futility, SEE pruning |
| **Move ordering** | TT move, SEE-split captures (good/bad), queen promotions, killers, counter-moves, history with gravity + symmetric malus |
| **Evaluation** | **NNUE** (768→256→1 perspective net, int-quantized, incremental accumulator) — production default; PeSTO tapered HCE (material + PSTs + mobility + king safety + pawn structure + bishop pair) as fallback |
| **Endgame** | Syzygy 3-4-5-piece tablebases via Fathom (DTZ at root, WDL in-search) |
| **Opening** | Polyglot `.bin` book |
| **Hash** | 384 MB TT with XOR-key lock-free SMP, generation aging, cached static eval |
| **Time management** | Adaptive budget with soft iteration abort |
| **Tuning** | Built-in Texel tuner (coordinate descent, L2 regularization) |
| **Protocol** | Full UCI with `Hash`, `Threads`, `SyzygyPath`, `Move Overhead`, `EvalFile`, `Ponder`, etc. |

## Architecture

```
src/
├── board.c/h     Magic bitboards, move gen, SEE, Zobrist, mobility
├── search.c/h    PVS + all pruning/reduction, Lazy SMP, time management
├── eval.c/h      Tapered PeSTO eval, pawn structure, king safety
├── tt.c/h        Transposition table (lock-free, XOR-keyed)
├── opening.c/h   Polyglot book probe
├── syzygy.c/h    Fathom adapter (3-4-5-piece WDL+DTZ)
├── nnue.c/h      NNUE inference + .nnue loader (incremental accumulator)
├── datagen.c/h   Self-play data generation for NNUE training
├── texel.c/h     Built-in eval tuner
├── chat.c/h      Lichess chat messages (mate alerts, captures, ponder hits, features)
├── bench.c/h     Deterministic benchmark positions
├── uci.c/h       UCI protocol handler (search runs on a background thread for pondering)
└── main.c        Entry point, book + tablebase + NNUE init
```

## Build

```bash
make release      # optimized binary → ./chess-engine-c  (-O3 -march=native -flto)
make test         # perft depth 4 & 5 (correctness check)
make debug        # debug binary with symbols
make bench        # run benchmark positions
```

The optimization flags are parameterized via `ARCH` and `LTO`. The default
`ARCH=-march=native` is correct for x86 (WSL gauntlets) and for a build done
on the Pi itself. **On the Raspberry Pi 4, prefer the explicit Cortex-A72
target** — older ARM GCC doesn't reliably detect the CPU through `-march=native`
and can silently drop the NEON popcount path:

```bash
make release ARCH="-mcpu=cortex-a72"   # canonical Pi 4 build
make release LTO=                      # disable link-time optimization
```

Requires GCC with C11 support and pthreads. Targets ARM (aarch64) on Pi but builds on any Linux/macOS with:

```bash
gcc -std=c11 -O3 -march=native -flto src/*.c external/tbprobe.o -o chess-engine-c -pthread -lm
```

## Run

```bash
./chess-engine-c
uci
setoption name Hash value 256
setoption name Threads value 4
setoption name SyzygyPath value /path/to/syzygy
isready
position startpos moves e2e4 e7e5
go movetime 5000
quit
```

## Live Bot

rpiBot73 plays rated rapid and classical on Lichess, challenging other bots when idle. The stack:

```
Raspberry Pi 4 (4 cores, 4 GB RAM)
  └── asynclio-bot (async Python bridge, pondering enabled)
       └── chess-engine-c (this project)
            ├── network.nnue (768→256→1 NNUE eval)
            ├── book.bin (Polyglot opening book)
            └── /home/bertrand/syzygy/ (3-4-5-piece, 939 MB)
```

- **Rating:** ~1900-2000 Elo on the classical eval (Stockfish Skill ~9-10, gauntlet 2026-05-17); the NNUE eval measured +207 Elo over that hand-crafted baseline
- **Time controls:** 10+0, 10+5, 15+10 (rapid/classical)
- **Hardware:** All 4 cores, 384 MB hash, full tablebase coverage

## Performance

On Raspberry Pi 4 (Cortex-A72 @ 1.8 GHz):
- ~2M nodes/sec (single thread)
- Reaches depth 16-18 in typical middlegame positions at tournament time controls
- Syzygy root probe returns instantly for <= 5-piece positions

## Testing & Tuning

```bash
# A/B performance benchmark (interleaved to cancel thermal drift)
make bench-baseline           # snapshot current binary
# ... make changes ...
make bench-compare BENCH_DEPTH=10 BENCH_RUNS=5

# Self-play gauntlet (requires fast-chess)
tools/wsl-ab-gauntlet.sh main feature-branch

# Texel tuning
make corpus                   # generate EPD from lichess games
printf 'texel quiet-labeled.epd\nquit\n' | ./chess-engine-c
```

## Dashboard

A companion web dashboard (port 8080) shows live game status, engine stats, and recent results. See `chess-dashboard/`.

## Project Philosophy

This engine is a learning project and a live experiment in chess programming (see [Origin](#chess-c) above for how it came to be). The goal isn't to beat the top engines, but to build the strongest possible engine on minimal hardware: first squeezing classical hand-crafted heuristics as far as they go (~2000 Elo), then adding a small from-scratch NNUE eval (+207 Elo) trained on the bot's own self-play — all on a Raspberry Pi 4 CPU, no GPU.

## Built with Claude

This engine is developed in collaboration with [Claude](https://claude.ai) (Anthropic). Claude helps with architecture decisions, search/eval improvements, code review, and automated gauntlet testing. The human provides direction, chess knowledge, and final judgment on what ships to production.

## License

Engine source: MIT. Vendored Fathom (`external/`): CC0.
