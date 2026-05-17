# chess-c

A classical UCI chess engine in C, running as an always-on bot on a Raspberry Pi 4. Plays as [rpiBot73](https://lichess.org/@/rpiBot73) on lichess via `lichess-bot`.

- `src/` — engine (board, eval, search, TT, UCI, opening book, A/B bench)
- `dashboard/` — small HTTP+SSE dashboard at `http://<pi>:8080`
- `tools/` — Polyglot key generator, bench-compare scripts
- `CLAUDE.md` — full build / run / deploy / architecture reference

## Build & test

```bash
make release        # → ./chess-engine-c
make test           # perft startpos depth 4 (197281) and 5 (4865609)
make bench-compare BENCH_DEPTH=10  # A/B against ./chess-engine-c.baseline
```

## Engine highlights

Bitboards with magic sliders, PVS + null-move + LMR + LMP + frontier futility,
killers + history, aspiration windows, transposition table with XOR-key race
protection and generation-based aging, Polyglot opening book, Lazy SMP.
PeSTO-style tapered eval with material, PSTs, mobility, king safety, pawn
structure, and bishop pair.

## Dashboard secrets

The dashboard reads the lichess API token from `$LICHESS_TOKEN_RPIBOT73`
(see `dashboard/.env.example`). The systemd unit loads it via
`EnvironmentFile=/home/bertrand/.config/chess-dashboard.env` (mode 600).
Nothing in this repo contains a real token; if you fork, generate your own at
<https://lichess.org/account/oauth/token>.
