#!/usr/bin/env bash
# Parallel baseline-vs-texel gauntlet for WSL (or any multi-core x86 host).
#
# Builds two binaries from the current git tree:
#   - chess-engine-c.baseline: default eval, current HEAD
#   - chess-engine-c.texel:    snapshot applied to src/eval.c, then reverted
# Then runs cutechess-cli with N concurrent games over a small opening book.
#
# Usage:
#   tools/wsl-gauntlet.sh [snapshot.txt]
#
# Tweakable via env vars (defaults shown):
#   GAMES=200          total games (must be even — colors alternate per pair)
#   CONCURRENCY=8      parallel games. With 16 cores, 8-12 is sane.
#                      Each game spawns 2 engine procs; OS needs 1-2 cores too.
#   MOVETIME_MS=500    time per move per side. 500ms gives ~depth 10-12 on x86.
#   OPENINGS=...       path to EPD file. Default: tools/openings-small.epd
#   PGN_OUT=...        path to PGN output. Default: texel-wsl-gauntlet.pgn
#
# Requires: cutechess-cli (sudo apt install cutechess-cli)
# Refuses to run if src/eval.c is dirty — commit or stash your edits first.

set -euo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO"

SNAPSHOT="${1:-texel-snapshot.txt}"
GAMES="${GAMES:-200}"
CONCURRENCY="${CONCURRENCY:-8}"
MOVETIME_MS="${MOVETIME_MS:-500}"
OPENINGS="${OPENINGS:-$REPO/tools/openings-small.epd}"
PGN_OUT="${PGN_OUT:-$REPO/texel-wsl-gauntlet.pgn}"

# Convert ms → seconds with one decimal place (cutechess wants seconds-as-float).
ST_SEC=$(awk "BEGIN { printf \"%.3f\", $MOVETIME_MS / 1000.0 }")
ROUNDS=$(( GAMES / 2 ))

echo "════════════════════════════════════════════════════════════"
echo "  parallel gauntlet: baseline vs texel"
echo "  snapshot:    $SNAPSHOT"
echo "  games:       $GAMES  (rounds: $ROUNDS, 2 games each — colors swap)"
echo "  concurrency: $CONCURRENCY  ($(nproc) cores detected)"
echo "  movetime:    ${MOVETIME_MS}ms per side per move"
echo "  openings:    $OPENINGS"
echo "  pgn out:     $PGN_OUT"
echo "════════════════════════════════════════════════════════════"

# ── Sanity checks ────────────────────────────────────────────────
command -v cutechess-cli >/dev/null \
    || { echo "ERROR: cutechess-cli not found. Install with:"; \
         echo "    sudo apt install cutechess-cli"; exit 1; }
[ -f "$SNAPSHOT" ] \
    || { echo "ERROR: snapshot file not found: $SNAPSHOT"; exit 1; }
[ -f "$OPENINGS" ] \
    || { echo "ERROR: openings file not found: $OPENINGS"; exit 1; }
if ! git diff --quiet src/eval.c 2>/dev/null; then
    echo "ERROR: src/eval.c has uncommitted changes — commit or stash first."
    echo "       (this script needs a clean baseline to compare against)"
    exit 1
fi

# ── 1. Build baseline binary (current HEAD source) ──────────────
echo
echo "[1/4] building baseline binary…"
make -s release
cp chess-engine-c chess-engine-c.baseline
echo "      → chess-engine-c.baseline"

# ── 2. Apply snapshot, build texel binary ───────────────────────
echo
echo "[2/4] applying snapshot + building texel binary…"
python3 tools/apply-texel-snapshot.py "$SNAPSHOT" src/eval.c
make -s release
cp chess-engine-c chess-engine-c.texel
echo "      → chess-engine-c.texel"

# ── 3. Revert eval.c so source tree stays canonical ─────────────
echo
echo "[3/4] reverting src/eval.c from backup…"
BACKUP=$(ls -t src/eval.c.bak.* 2>/dev/null | head -1)
[ -n "$BACKUP" ] || { echo "ERROR: no backup file found in src/"; exit 1; }
mv "$BACKUP" src/eval.c
make -s release    # rebuild chess-engine-c so it matches baseline source
echo "      reverted, chess-engine-c rebuilt from baseline source"

ls -l chess-engine-c chess-engine-c.baseline chess-engine-c.texel

# ── 4. Run the gauntlet ─────────────────────────────────────────
echo
echo "[4/4] cutechess-cli match starting…"
echo
cutechess-cli \
    -engine cmd="$REPO/chess-engine-c.baseline" name=baseline proto=uci \
    -engine cmd="$REPO/chess-engine-c.texel"    name=texel    proto=uci \
    -each tc=inf st="$ST_SEC" \
    -games 2 -rounds "$ROUNDS" \
    -concurrency "$CONCURRENCY" \
    -openings file="$OPENINGS" format=epd order=random \
    -repeat \
    -recover \
    -pgnout "$PGN_OUT" \
    -ratinginterval 10

echo
echo "═══════════════════════════════════════════════════════════"
echo "  PGN saved to: $PGN_OUT"
echo "  if texel won meaningfully (Elo > ~+10 with tight error),"
echo "  paste this snapshot into src/eval.c on the Pi and gauntlet"
echo "  again there (architecture-portable values) before deploying."
echo "═══════════════════════════════════════════════════════════"
