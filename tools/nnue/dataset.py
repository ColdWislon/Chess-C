"""Dataset + collate for datagen output lines: `<FEN> | <cp> | <result>`.

cp and result are already from the side-to-move POV (see src/datagen.c), so the
training target needs no perspective flip:

    target = lambda_ * sigmoid(cp / SCALE) + (1 - lambda_) * result

EmbeddingBag wants a flat 1-D index tensor plus per-sample offsets, so the
collate fn concatenates each sample's active features and records bag boundaries.
"""
import math
import torch
from torch.utils.data import Dataset
from features import active_features, SCALE


def _sigmoid(x):
    return 1.0 / (1.0 + math.exp(-x))


class FenDataset(Dataset):
    def __init__(self, path, lambda_=0.6):
        self.samples = []
        self.lambda_ = lambda_
        with open(path) as f:
            for line in f:
                line = line.strip()
                if not line:
                    continue
                try:
                    fen_part, cp_part, res_part = line.split('|')
                    cp = int(cp_part.strip())
                    res = float(res_part.strip())
                except ValueError:
                    continue  # skip malformed lines rather than crash a long run
                stm_feats, nstm_feats, _ = active_features(fen_part.strip())
                target = lambda_ * _sigmoid(cp / SCALE) + (1.0 - lambda_) * res
                self.samples.append((stm_feats, nstm_feats, target))

    def __len__(self):
        return len(self.samples)

    def __getitem__(self, i):
        return self.samples[i]


def collate(batch):
    stm_idx, stm_off, nstm_idx, nstm_off, tgt = [], [], [], [], []
    so = no = 0
    for stm_feats, nstm_feats, target in batch:
        stm_off.append(so);  so += len(stm_feats);  stm_idx.extend(stm_feats)
        nstm_off.append(no); no += len(nstm_feats); nstm_idx.extend(nstm_feats)
        tgt.append(target)
    return (
        torch.tensor(stm_idx, dtype=torch.long),
        torch.tensor(stm_off, dtype=torch.long),
        torch.tensor(nstm_idx, dtype=torch.long),
        torch.tensor(nstm_off, dtype=torch.long),
        torch.tensor(tgt, dtype=torch.float32),
    )
