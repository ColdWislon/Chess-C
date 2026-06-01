#!/usr/bin/env bash
# NNUE-vs-HCE gauntlet: pit the same engine binary against itself, A=HCE
# (no EvalFile), B=NNUE (EvalFile=<net>). Sister to tools/wsl-ab-gauntlet.sh
# but holds the binary constant and varies only the eval, so the Elo signal
# is purely the net's quality.
#
# Usage:
#   tools/nnue/nnue-vs-hce-gauntlet.sh <path-to-net.nnue>
#
# Env (defaults shown):
#   GAMES=200          total games
#   CONCURRENCY=12     parallel games (leave a few cores for the OS)
#   MOVETIME_MS=500    time per move per side
#   OPENINGS=...       EPD file (default: tools/openings-small.epd)
#   PGN_OUT=...        PGN output (default: ab-gauntlet-nnue.pgn)
#
# Engine A is always launched with a CWD that has NO `network.nnue` present,
# so its startup auto-load fails and it falls back to HCE. Engine B is told
# EvalFile=<absolute path> via UCI, which overrides any startup state.
set -euo pipefail

REPO="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$REPO"

NET="${1:-}"
[ -n "$NET" ] || { echo "usage: $0 <path-to-net.nnue>"; exit 1; }
[ -f "$NET" ] || { echo "ERROR: net not found: $NET"; exit 1; }
NET_ABS="$(cd "$(dirname "$NET")" && pwd)/$(basename "$NET")"

GAMES="${GAMES:-200}"
CONCURRENCY="${CONCURRENCY:-12}"
MOVETIME_MS="${MOVETIME_MS:-500}"
OPENINGS="${OPENINGS:-$REPO/tools/openings-small.epd}"
PGN_OUT="${PGN_OUT:-$REPO/ab-gauntlet-nnue.pgn}"
ST_SEC=$(awk "BEGIN { printf \"%.3f\", $MOVETIME_MS / 1000.0 }")
ROUNDS=$(( GAMES / 2 ))

command -v fast-chess >/dev/null || { echo "ERROR: fast-chess not on PATH"; exit 1; }
[ -f "$OPENINGS" ] || { echo "ERROR: openings file not found: $OPENINGS"; exit 1; }
[ -x "$REPO/chess-engine-c" ] || { echo "ERROR: chess-engine-c not built — run make release"; exit 1; }

# Stage the binary in a separate dir for engine A so its CWD has NO
# network.nnue → guaranteed HCE startup. Engine B runs from $REPO and is
# explicitly told EvalFile=<abs path> via UCI option, which overrides
# whatever was/wasn't auto-loaded.
STAGE_HCE="$(mktemp -d)"
trap 'rm -rf "$STAGE_HCE"' EXIT
cp "$REPO/chess-engine-c" "$STAGE_HCE/chess-engine-c"
echo "[hce stage] $STAGE_HCE  (no network.nnue here → engine falls back to HCE)"
echo "[nnue net]  $NET_ABS"
echo "[games]     $GAMES  rounds=$ROUNDS  concurrency=$CONCURRENCY  st=${ST_SEC}s"
echo

fast-chess \
    -engine cmd="$STAGE_HCE/chess-engine-c" name=hce  proto=uci \
                  dir="$STAGE_HCE" \
    -engine cmd="$REPO/chess-engine-c"     name=nnue proto=uci \
                  dir="$REPO" \
                  option.EvalFile="$NET_ABS" \
    -each tc=inf st="$ST_SEC" \
    -games 2 -rounds "$ROUNDS" \
    -concurrency "$CONCURRENCY" \
    -openings file="$OPENINGS" format=epd order=random \
    -repeat -recover \
    -pgnout "file=$PGN_OUT" \
    -ratinginterval 10 \
    2>&1 | python3 "$REPO/tools/gauntlet-progress.py" "$GAMES"

echo
echo "═══════════════════════════════════════════════════════════"
echo "  PGN saved to: $PGN_OUT"
echo "  Elo from fast-chess output is from HCE's perspective."
echo "  Positive Elo → HCE is stronger; negative → NNUE is stronger."
echo "═══════════════════════════════════════════════════════════"
