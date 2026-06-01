#!/usr/bin/env bash
# Fan out `datagen` across all cores. Each worker writes its own .txt; we cat
# them together at the end and report total positions.
#
#   tools/nnue/parallel-datagen.sh [workers] [games_per_worker] [depth]
#
# Defaults (16 / 2000 / 8) target ~3M positions in ~20 min on a 16-core WSL host.
set -euo pipefail

WORKERS="${1:-16}"
GAMES="${2:-2000}"
DEPTH="${3:-8}"

REPO="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$REPO"

mkdir -p data data/logs
OUT="data/selfplay.txt"
TMP_DIR="data/parts"
rm -rf "$TMP_DIR" "$OUT"
mkdir -p "$TMP_DIR"

echo "datagen: $WORKERS workers x $GAMES games @ depth $DEPTH"
echo "outputs: $TMP_DIR/sp_<i>.txt  -> concatenated to $OUT"
echo

t0=$(date +%s)
for i in $(seq 1 "$WORKERS"); do
  (
    seed=$((1000 + i))
    out="$TMP_DIR/sp_${i}.txt"
    log="data/logs/sp_${i}.log"
    printf 'datagen %s %d %d %d\nquit\n' "$out" "$GAMES" "$DEPTH" "$seed" \
      | ./chess-engine-c >"$log" 2>&1
  ) &
done
wait
t1=$(date +%s)

cat "$TMP_DIR"/sp_*.txt > "$OUT"
pos=$(wc -l <"$OUT")
echo
printf "done in %ds — %s positions (%.0f pos/s)\n" "$((t1-t0))" "$pos" \
       "$(awk "BEGIN { printf \"%.0f\", $pos / ($t1-$t0) }")"
ls -la "$OUT"
