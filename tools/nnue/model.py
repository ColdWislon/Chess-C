"""The 768 -> N -> 1 perspective network (PyTorch). See docs/nnue-format.md.

The feature transformer is an nn.EmbeddingBag(mode='sum'): summing the rows of a
[768, N] weight matrix for the active features is exactly the sparse linear layer
W·x that the engine computes incrementally. The SAME transformer (shared weights)
is applied to both perspectives; the side-to-move accumulator is concatenated
first, matching src/nnue.c.
"""
import torch
import torch.nn as nn
from features import N_FEATURES, N_DEFAULT


class NNUE(nn.Module):
    def __init__(self, n_hidden=N_DEFAULT):
        super().__init__()
        self.N = n_hidden
        self.ft = nn.EmbeddingBag(N_FEATURES, n_hidden, mode='sum')
        self.ft_bias = nn.Parameter(torch.zeros(n_hidden))
        self.out = nn.Linear(2 * n_hidden, 1)
        # Small init keeps quantized weights well inside int16.
        nn.init.normal_(self.ft.weight, std=0.05)
        nn.init.normal_(self.out.weight, std=0.05)
        nn.init.zeros_(self.out.bias)

    def _accum(self, idx, offsets):
        # clipped ReLU into [0, 1] — the float analogue of the int [0, QA] clamp
        return torch.clamp(self.ft(idx, offsets) + self.ft_bias, 0.0, 1.0)

    def forward(self, stm_idx, stm_off, nstm_idx, nstm_off):
        us = self._accum(stm_idx, stm_off)
        them = self._accum(nstm_idx, nstm_off)
        h = torch.cat([us, them], dim=1)        # [B, 2N], stm half first
        return self.out(h).squeeze(1)           # logit y; cp = y*SCALE, p = sigmoid(y)
