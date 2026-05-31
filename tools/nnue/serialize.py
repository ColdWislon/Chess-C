"""Quantize a trained float model -> .nnue binary. See docs/nnue-format.md §3-4.

Float weights are scaled (W,b by QA; O by QB; ob by QA*QB), rounded to ints, and
written little-endian in the exact layout src/nnue.c::nnue_load expects. Warns
loudly if any weight saturates int16 — that means the net needs smaller weights
(lower LR / more reg) or the quant scheme needs revisiting.
"""
import struct
import numpy as np
from features import QA, QB, N_FEATURES


def _to_i16(arr, name):
    lo, hi = arr.min(), arr.max()
    if lo < -32768 or hi > 32767:
        n_sat = int((arr < -32768).sum() + (arr > 32767).sum())
        print(f"  WARNING: {name} saturates int16 "
              f"(range [{lo},{hi}], {n_sat} clipped) — net too hot")
    return np.clip(arr, -32768, 32767).astype('<i2')


def serialize(model, path):
    N = model.N
    W = model.ft.weight.detach().cpu().numpy()          # [768, N]
    b = model.ft_bias.detach().cpu().numpy()            # [N]
    O = model.out.weight.detach().cpu().numpy()[0]      # [2N]  (us half | them half)
    ob = float(model.out.bias.detach().cpu().numpy()[0])

    W_i = _to_i16(np.round(W * QA), "feature weights")
    b_i = _to_i16(np.round(b * QA), "feature bias")
    O_i = _to_i16(np.round(O * QB), "output weights")
    ob_i = int(round(ob * QA * QB))

    with open(path, 'wb') as f:
        f.write(b'CNUE')
        f.write(struct.pack('<III', 1, N, N_FEATURES))
        # W is [768, N] C-order -> flattened row-major == W[f*N + n], matching C.
        f.write(W_i.tobytes())
        f.write(b_i.tobytes())
        f.write(O_i.tobytes())
        f.write(struct.pack('<i', ob_i))

    expect = 16 + (N_FEATURES * N + N + 2 * N) * 2 + 4
    print(f"  wrote {path}  (N={N}, {expect} bytes)")


if __name__ == '__main__':
    import argparse
    import torch
    from model import NNUE
    ap = argparse.ArgumentParser(description="Convert a trained .pt model to .nnue")
    ap.add_argument('checkpoint', help='path to .pt saved by train.py')
    ap.add_argument('-o', '--out', default='network.nnue')
    args = ap.parse_args()
    ckpt = torch.load(args.checkpoint, map_location='cpu')
    model = NNUE(ckpt['N'])
    model.load_state_dict(ckpt['model'])
    serialize(model, args.out)
