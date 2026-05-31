# NNUE format — single source of truth

This document pins down the *exact* conventions the C engine (`src/nnue.c`) and the
WSL PyTorch trainer (`tools/nnue/`) must agree on, byte for byte. If you change
anything here, change it in **both** places and re-run the cross-validation gate
(`tools/nnue/check_parity.py`).

## 1. Network architecture: `768 → N → 1` perspective net

A "perspective" (a.k.a. dual-accumulator) network. `N = 256` by default.

```
                 side-to-move POV
INPUT  768 (us) ─┐
                 ├─ W (768×N), bias b ─► accumulator A[us], A[them]  (two perspectives)
INPUT  768 (them)┘
                          │  clipped-ReLU, clamp [0, 1]   (float) / [0, QA] (int)
                          ▼
            h = concat( crelu(A[stm]), crelu(A[¬stm]) )      # length 2N
                          │  O (length 2N), bias ob
                          ▼
                       y  (scalar)
```

The **side to move's** accumulator is always concatenated **first**. This makes
the net symmetric: it always evaluates "from the mover's point of view", matching
`evaluate()`'s contract (positive = good for side to move).

## 2. Feature indexing (768 inputs per perspective)

Inputs are `64 squares × 6 piece types × 2 relative colors = 768`. For a given
perspective `persp ∈ {WHITE=0, BLACK=1}` and a piece of `color`, `piece_type`
(`PAWN=0 … KING=5`, per `board.h`) on `square` (`a1=0 … h8=63`):

```
relative_color = (color == persp) ? 0 : 1          # 0 = our piece, 1 = their piece
relative_sq    = (persp == WHITE) ? square : (square ^ 56)   # vertical flip for black
index          = relative_color * 384 + piece_type * 64 + relative_sq
```

`square ^ 56` flips the rank (a1↔a8), so "our pawns march toward rank 8" in *both*
perspectives. `index` is in `[0, 768)`.

The accumulator for a perspective is `A[persp] = b + Σ_pieces W[:, index]`.

## 3. Quantization

| Constant | Value | Meaning |
|---|---|---|
| `QA`    | 255 | feature-weight / accumulator scale |
| `QB`    | 64  | output-weight scale |
| `SCALE` | 400 | logit → centipawn scale (1 pawn ≈ logistic 400 cp) |

**Float forward (training):** activations clamp to `[0, 1]`, output `y` is a logit.
`win_prob = sigmoid(y)` and the corresponding centipawn eval is `cp = y * SCALE`
(so `win_prob = sigmoid(cp / SCALE)`, the usual logistic with 400 cp scale).

**Quantize (float → int), see `serialize.py`:**
```
W_int[f][n] = round(W[f][n] * QA)        int16
b_int[n]    = round(b[n]    * QA)        int16
O_int[j]    = round(O[j]    * QB)        int16
ob_int      = round(ob * QA * QB)        int32
```
Serializer warns if any weight saturates int16.

**Int inference (engine), see `nnue.c`:**
```
acc[n]  = b_int[n] + Σ_active W_int[f][n]            # int32
cr[n]   = clamp(acc[n], 0, QA)                       # clipped ReLU, int32 in [0,255]
out     = ob_int + Σ_{j<2N} cr_concat[j] * O_int[j]  # int64 (can exceed int32!)
cp      = out * SCALE / (QA * QB)                    # = out * 400 / 16320, integer divide
```
`out` **must** accumulate in 64-bit: `2N · 255 · 32767 ≈ 4.3e9` overflows int32.

## 4. `.nnue` binary file format (little-endian)

```
offset  type         name              count
0       char[4]      magic = "CNUE"    1
4       u32          version = 1       1
8       u32          hidden N          1     (=256)
12      u32          n_features        1     (=768)
16      i16          W (feature wts)   768*N   row-major: W[f*N + n]
…       i16          b (feature bias)  N
…       i16          O (output wts)    2*N     [ us-half (N) | them-half (N) ]
…       i32          ob (output bias)  1
```

Total bytes = `16 + (768*N + N + 2*N)*2 + 4`. For `N=256`: `16 + 197888*2 + 4 = 395796`.

Loader rejects on magic/version/N/n_features mismatch and falls back to the
hand-crafted eval.

## 5. Invariants the parity test checks

`tools/nnue/check_parity.py` serializes a fixed pseudo-random net, then asserts the
C engine's `eval nnue <fen>` output equals the Python int-path eval for a battery of
FENs (exact integer match — both run the *quantized* path). Any drift means the two
implementations have diverged and must be reconciled before training is meaningful.
