#!/usr/bin/env bash
# Re-label our self-play FENs with Stockfish scores at fixed depth/nodes.
# Input lines: `<FEN> | <cp_old> | <result>`  (datagen output)
# Output lines: `<FEN> | <cp_new> | <result>` — same shape, just stronger label.
#
# Rationale: NNUE trained on its own engine's labels can at best mimic that
# engine. Re-labeling with Stockfish gives the net a target ABOVE HCE quality
# so the trained net can actually exceed HCE in play.
#
#   tools/nnue/stockfish-relabel.sh [in] [out] [depth] [workers]
#
# Defaults: data/selfplay.txt -> data/sf-relabeled.txt, depth 10, 14 workers.
# Workers should be < nproc to leave breathing room for the system.
set -euo pipefail

REPO="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$REPO"

IN="${1:-data/selfplay.txt}"
OUT="${2:-data/sf-relabeled.txt}"
DEPTH="${3:-10}"
WORKERS="${4:-14}"

# Stockfish binary: prefer $STOCKFISH env var, else search PATH and a few
# common WSL locations (apt installs vary; we may also be using a downloaded
# binary in ~/local-bin/).
SF="${STOCKFISH:-}"
if [ -z "$SF" ]; then
    for cand in \
        "$(command -v stockfish 2>/dev/null)" \
        "$HOME/local-bin/stockfish" \
        "/usr/games/stockfish" \
        "/usr/bin/stockfish"; do
        if [ -n "$cand" ] && [ -x "$cand" ]; then SF="$cand"; break; fi
    done
fi
[ -n "$SF" ] && [ -x "$SF" ] \
    || { echo "ERROR: stockfish not found (set STOCKFISH=/path or apt install)"; exit 1; }
echo "  stockfish: $SF"
[ -f "$IN" ] || { echo "ERROR: input not found: $IN"; exit 1; }

TMP_DIR="$(mktemp -d)"
trap 'rm -rf "$TMP_DIR"' EXIT

INPUT_LINES=$(wc -l < "$IN")
echo "stockfish relabel:"
echo "  input:   $IN  ($INPUT_LINES lines)"
echo "  output:  $OUT"
echo "  depth:   $DEPTH"
echo "  workers: $WORKERS"
echo

# Split input into $WORKERS roughly-equal chunks.
mkdir -p "$TMP_DIR/in" "$TMP_DIR/out" data
split -d -n "l/$WORKERS" "$IN" "$TMP_DIR/in/chunk_"

t0=$(date +%s)
for chunk in "$TMP_DIR"/in/chunk_*; do
    name=$(basename "$chunk")
    out="$TMP_DIR/out/$name"
    (
        # Drive one persistent Stockfish process. `go depth N` blocks until
        # the search completes; we parse the final `info depth N ... score cp X`
        # line for the score. python-chess would be tidier but a fast text
        # pipeline keeps it dependency-free.
        python3 -u "$REPO/tools/nnue/sf_label_worker.py" \
            --in "$chunk" --out "$out" --depth "$DEPTH" --sf "$SF"
    ) &
done
wait
t1=$(date +%s)

cat "$TMP_DIR"/out/chunk_* > "$OUT"
relabeled=$(wc -l < "$OUT")
echo
printf "done in %ds — %s lines relabeled (%.0f/s)\n" \
    "$((t1-t0))" "$relabeled" \
    "$(awk "BEGIN { printf \"%.0f\", $relabeled / ($t1-$t0) }")"
ls -la "$OUT"
