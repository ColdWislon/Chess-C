#!/usr/bin/env bash
# Bootstrap a venv for NNUE training on WSL (or any Linux box with a GPU).
# PEP-668-safe: never touches system python. Mirrors the corpus venv pattern.
#
#   tools/nnue/setup-wsl.sh           # create .venv-nnue + install deps
#   source .venv-nnue/bin/activate    # then train
set -euo pipefail

cd "$(dirname "$0")/../.."          # repo root
VENV=.venv-nnue

command -v python3 >/dev/null || { echo "python3 not found"; exit 1; }

if [[ ! -x "$VENV/bin/python" ]]; then
  echo "creating $VENV ..."
  python3 -m venv "$VENV" || {
    echo "venv creation failed — try: sudo apt install python3-venv python3-full"; exit 1; }
fi

"$VENV/bin/pip" install --quiet --upgrade pip

# Detect an NVIDIA GPU; prefer the CUDA wheel when present, else CPU wheel.
if command -v nvidia-smi >/dev/null && nvidia-smi >/dev/null 2>&1; then
  echo "NVIDIA GPU detected — installing CUDA torch wheel"
  "$VENV/bin/pip" install --quiet torch numpy
else
  echo "no GPU detected — installing CPU torch wheel (training will be slow)"
  "$VENV/bin/pip" install --quiet torch numpy --index-url https://download.pytorch.org/whl/cpu
fi

echo
echo "ready. Activate with:  source $VENV/bin/activate"
echo "then e.g.:  python3 tools/nnue/train.py data/selfplay.txt --epochs 30 --out network.nnue"
