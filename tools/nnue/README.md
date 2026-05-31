# NNUE training pipeline

A small `768 → 256 → 1` perspective NNUE for chess-c, end to end: self-play data
on the engine → train on WSL/GPU → quantize → load via UCI `EvalFile`. The exact
arch / quantization / file format is pinned in [`docs/nnue-format.md`](../../docs/nnue-format.md).

```
 src/datagen.c            tools/nnue/                       src/nnue.c
┌──────────────┐  txt   ┌─────────────────────────────┐  .nnue  ┌──────────────┐
│ self-play    │ ─────► │ dataset → model → train     │ ──────► │ load + eval  │
│ `datagen`    │        │        → serialize (quant)  │         │ `EvalFile`   │
└──────────────┘        └─────────────────────────────┘         └──────────────┘
```

## Files

| File | Runs where | Purpose |
|---|---|---|
| `features.py` | anywhere (pure python) | FEN → 768 feature indices; the shared spec |
| `dataset.py` | WSL | parse `FEN \| cp \| result`, build training targets |
| `model.py` | WSL | the EmbeddingBag perspective net |
| `train.py` | WSL (GPU) | training loop, saves `.pt` + `.nnue` |
| `serialize.py` | WSL | float model → int-quantized `.nnue` |
| `check_parity.py` | **Pi or WSL** | proves C ↔ Python compute identical evals |
| `setup-wsl.sh` | WSL | bootstrap `.venv-nnue` (PEP-668 safe) |

## End-to-end workflow

**1. Generate self-play data** (CPU-heavy — run on WSL, or on the Pi only when
rpiBot73 is idle; see CLAUDE.md). Use different `seed`s to parallelise:

```bash
make release
mkdir -p data
# datagen <out> [games] [depth] [seed]
printf 'datagen data/sp1.txt 5000 8 1\nquit\n' | ./chess-engine-c
printf 'datagen data/sp2.txt 5000 8 2\nquit\n' | ./chess-engine-c   # parallel, other core
cat data/sp*.txt > data/selfplay.txt
```

Each line is `<FEN> | <cp> | <result>` with `cp` and `result` from the
side-to-move POV. Only quiet, non-check, non-mate-score positions are emitted.

**2. Train** (WSL with a CUDA GPU):

```bash
tools/nnue/setup-wsl.sh
source .venv-nnue/bin/activate
python3 tools/nnue/train.py data/selfplay.txt --epochs 30 --out network.nnue
```

`train.py` writes the best-val checkpoint as both `network.pt` and the quantized
`network.nnue`. Watch for the serializer's `WARNING: ... saturates int16` — if it
fires, the net is too hot (lower `--lr`, or it needs more data).

**3. Verify the format still matches** (must pass before you trust any net):

```bash
make release                       # rebuild engine if src/nnue.c changed
python3 tools/nnue/check_parity.py # exact C vs Python integer-eval match
```

**4. Load it in the engine:**

```bash
./chess-engine-c
> setoption name EvalFile value network.nnue
> position startpos
> go movetime 2000
```

Or set `EvalFile` in `config-c.yml`, or drop `network.nnue` next to the binary
(auto-loaded at startup), or `export EVALFILE=/path/to/net`.

**5. Gauntlet before deploying** — NNUE vs HCE is a big change; confirm Elo the
usual way (`tools/wsl-ab-gauntlet.sh`) and only then ship via `safe-restart-bot.sh`.

## Notes & next steps

- **Inference is full-refresh per node** (correctness-first). The obvious follow-up
  optimization is an incremental accumulator (add/sub only the moved piece's column
  in make/unmake) — important on the Pi 4's A72, where eval cost dominates NPS.
- **Bootstrap quality is search-bounded.** Once a first net plays decently, plug it
  back in via `EvalFile` and re-run `datagen` to relabel at higher quality
  (data spiral) for a stronger gen-2 net.
- **Pi 4 caveat:** Cortex-A72 has no int8 dot-product instruction, so NNUE NPS
  takes a real hit (see CLAUDE.md "Known gaps"). Keep `N` small; a Pi 5 (A76)
  roughly doubles throughput.
