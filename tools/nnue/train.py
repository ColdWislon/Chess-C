"""Train the small NNUE on datagen output, then serialize to .nnue.

Usage (on WSL with a CUDA GPU):
    python3 train.py data/selfplay.txt --epochs 30 --out ../../network.nnue

Loss is MSE between sigmoid(pred-logit) and the blended WDL+eval target. The net
output is a logit y; the engine reads cp = y*SCALE, so win_prob = sigmoid(y).
"""
import argparse
import torch
import torch.nn as nn
from torch.utils.data import DataLoader

from model import NNUE
from dataset import FenDataset, collate
from serialize import serialize
from features import N_DEFAULT


def main():
    ap = argparse.ArgumentParser(description="Train a small NNUE for chess-c")
    ap.add_argument('data', help='datagen output file (FEN | cp | result lines)')
    ap.add_argument('--epochs', type=int, default=30)
    ap.add_argument('--batch', type=int, default=16384)
    ap.add_argument('--lr', type=float, default=1e-3)
    ap.add_argument('--hidden', type=int, default=N_DEFAULT)
    ap.add_argument('--lambda', dest='lam', type=float, default=0.6,
                    help='eval/result blend: 1.0=pure eval, 0.0=pure WDL')
    ap.add_argument('--val-split', type=float, default=0.05)
    ap.add_argument('--out', default='network.nnue')
    ap.add_argument('--checkpoint', default='network.pt')
    ap.add_argument('--device', default='cuda' if torch.cuda.is_available() else 'cpu')
    args = ap.parse_args()

    print(f"device: {args.device}")
    print(f"loading {args.data} (lambda={args.lam}) ...")
    ds = FenDataset(args.data, lambda_=args.lam)
    n_val = max(1, int(len(ds) * args.val_split))
    n_train = len(ds) - n_val
    train_ds, val_ds = torch.utils.data.random_split(
        ds, [n_train, n_val], generator=torch.Generator().manual_seed(0))
    print(f"positions: {len(ds)}  (train {n_train} / val {n_val})")

    train_dl = DataLoader(train_ds, batch_size=args.batch, shuffle=True,
                          collate_fn=collate, num_workers=2, drop_last=False)
    val_dl = DataLoader(val_ds, batch_size=args.batch, shuffle=False,
                        collate_fn=collate, num_workers=2)

    dev = torch.device(args.device)
    model = NNUE(args.hidden).to(dev)
    opt = torch.optim.Adam(model.parameters(), lr=args.lr)
    sched = torch.optim.lr_scheduler.StepLR(opt, step_size=10, gamma=0.5)
    loss_fn = nn.MSELoss()

    def run_epoch(dl, train):
        model.train(train)
        total, count = 0.0, 0
        torch.set_grad_enabled(train)
        for si, so, ni, no, tgt in dl:
            si, so, ni, no, tgt = (x.to(dev) for x in (si, so, ni, no, tgt))
            pred = torch.sigmoid(model(si, so, ni, no))
            loss = loss_fn(pred, tgt)
            if train:
                opt.zero_grad(); loss.backward(); opt.step()
            total += loss.item() * tgt.size(0); count += tgt.size(0)
        return total / max(1, count)

    best_val = float('inf')
    for ep in range(1, args.epochs + 1):
        tr = run_epoch(train_dl, True)
        va = run_epoch(val_dl, False)
        sched.step()
        flag = ''
        if va < best_val:
            best_val = va
            torch.save({'model': model.state_dict(), 'N': model.N}, args.checkpoint)
            serialize(model.cpu(), args.out); model.to(dev)
            flag = '  *saved'
        print(f"epoch {ep:3d}/{args.epochs}  train {tr:.5f}  val {va:.5f}{flag}")

    print(f"done. best val {best_val:.5f}  ->  {args.out} (+ {args.checkpoint})")


if __name__ == '__main__':
    main()
