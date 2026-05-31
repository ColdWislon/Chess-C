# Chess-C

[![build](https://github.com/ColdWislon/Chess-C/actions/workflows/perft.yml/badge.svg)](https://github.com/ColdWislon/Chess-C/actions/workflows/perft.yml)
[![C](https://img.shields.io/badge/language-C11-blue)](src/)
[![Platform](https://img.shields.io/badge/platform-Raspberry%20Pi%204-c51a4a)](https://www.raspberrypi.com/)
[![Lichess](https://img.shields.io/badge/lichess-rpiBot73-green?logo=lichess)](https://lichess.org/@/rpiBot73)
[![License](https://img.shields.io/badge/license-MIT-yellow)](LICENSE)
[![Lines of code](https://img.shields.io/badge/lines-5k%20C-informational)](src/)

A classical UCI chess engine in C, running 24/7 as **[rpiBot73](https://lichess.org/@/rpiBot73)** on Lichess from a Raspberry Pi 4.

No NNUE, no training data — just bitboards, alpha-beta, and hand-tuned heuristics competing against the bots of Lichess.

> **Origin:** This is a "vibe coded" rewrite of an earlier C engine of mine, which was itself built following the [WikiChess](https://www.chessprogramming.org/Main_Page) (Chess Programming Wiki) approach. The current codebase was grown iteratively in collaboration with [Claude](https://claude.ai) — see [Built with Claude](#built-with-claude) below — rather than written from scratch in one pass.

---

## Features

| Layer | Implementation |
|-------|---------------|
| **Move generation** | Magic bitboard sliders, precomputed leaper tables |
| **Search** | Iterative deepening, PVS, aspiration windows, Lazy SMP (4 threads) |
| **Pruning** | Null-move (adaptive R), LMR (log formula + history adjustment), LMP, reverse futility, extended futility, SEE pruning |
| **Move ordering** | TT move, SEE-split captures (good/bad), queen promotions, killers, counter-moves, history with gravity + symmetric malus |
| **Evaluation** | PeSTO tapered (MG/EG), material + PSTs + mobility + king safety + pawn structure + bishop pair |
| **Endgame** | Syzygy 3-4-5-piece tablebases via Fathom (DTZ at root, WDL in-search) |
| **Opening** | Polyglot `.bin` book |
| **Hash** | 384 MB TT with XOR-key lock-free SMP, generation aging, cached static eval |
| **Time management** | Adaptive budget with soft iteration abort |
| **Tuning** | Built-in Texel tuner (coordinate descent, L2 regularization) |
| **Protocol** | Full UCI with `Hash`, `Threads`, `SyzygyPath`, `Move Overhead`, etc. |

## Architecture

```
src/
├── board.c/h     Magic bitboards, move gen, SEE, Zobrist, mobility
├── search.c/h    PVS + all pruning/reduction, Lazy SMP, time management
├── eval.c/h      Tapered PeSTO eval, pawn structure, king safety
├── tt.c/h        Transposition table (lock-free, XOR-keyed)
├── opening.c/h   Polyglot book probe
├── syzygy.c/h    Fathom adapter (3-4-5-piece WDL+DTZ)
├── texel.c/h     Built-in eval tuner
├── chat.c/h      Lichess chat messages (mate alerts, captures, features)
├── bench.c/h     Deterministic benchmark positions
├── uci.c/h       UCI protocol handler
└── main.c        Entry point, book + tablebase init
```

## Build

```bash
make release      # optimized binary → ./chess-engine-c
make test         # perft depth 4 & 5 (correctness check)
make debug        # debug binary with symbols
make bench        # run benchmark positions
```

Requires GCC with C11 support and pthreads. Targets ARM (aarch64) on Pi but builds on any Linux/macOS with:

```bash
gcc -std=c11 -O3 -march=native src/*.c external/tbprobe.o -o chess-engine-c -pthread -lm
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
  └── lichess-bot (Python bridge)
       └���─ chess-engine-c (this project)
            ├── book.bin (Polyglot opening book)
            └── /home/bertrand/syzygy/ (3-4-5-piece, 939 MB)
```

- **Rating:** ~1900-2000 Elo (equivalent to Stockfish Skill Level 9-10)
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

This engine is a learning project and a live experiment in classical chess programming. It started as a previous C engine I wrote by working through the techniques documented on the [Chess Programming Wiki](https://www.chessprogramming.org/Main_Page), and was then rebuilt "vibe coding" style — iterating conversationally with Claude on architecture, search, and eval rather than following a fixed spec. The goal isn't to beat the top engines (that requires NNUE + massive compute), but to build the strongest possible classical engine on minimal hardware and see how far hand-crafted heuristics can go.

## Built with Claude

This engine is developed in collaboration with [Claude](https://claude.ai) (Anthropic). Claude helps with architecture decisions, search/eval improvements, code review, and automated gauntlet testing. The human provides direction, chess knowledge, and final judgment on what ships to production.

## License

Engine source: MIT. Vendored Fathom (`external/`): CC0.
