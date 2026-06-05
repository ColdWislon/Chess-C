#!/usr/bin/env bash
# Generalized A/B gauntlet — pit two git refs (branches or commits) against
# each other via fast-chess (or cutechess-cli). Designed for WSL on a
# multi-core host but works on any machine with both.
#
# Usage:
#   tools/wsl-ab-gauntlet.sh <ref-A> <ref-B>
#
# Tweakable via env vars (defaults shown):
#   GAMES=200          total games
#   CONCURRENCY=8      parallel games
#   MOVETIME_MS=500    time per move per side
#   OPENINGS=...       EPD file (default: tools/openings-small.epd)
#   PGN_OUT=...        PGN output (default: ab-gauntlet.pgn)
#
# Examples:
#   tools/wsl-ab-gauntlet.sh main lmr-tune
#   tools/wsl-ab-gauntlet.sh main HEAD~5
#   GAMES=400 CONCURRENCY=12 tools/wsl-ab-gauntlet.sh main feature/foo
#
# Sister script: tools/wsl-gauntlet.sh is the snapshot-based variant for
# testing a texel-snapshot.txt without committing it to a branch.

set -euo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO"

REF_A="${1:-}"
REF_B="${2:-}"
[ -n "$REF_A" ] && [ -n "$REF_B" ] \
    || { echo "usage: $0 <ref-A> <ref-B>"; exit 1; }

GAMES="${GAMES:-200}"
CONCURRENCY="${CONCURRENCY:-8}"
MOVETIME_MS="${MOVETIME_MS:-500}"
OPENINGS="${OPENINGS:-$REPO/tools/openings-small.epd}"
PGN_OUT="${PGN_OUT:-$REPO/ab-gauntlet.pgn}"

ST_SEC=$(awk "BEGIN { printf \"%.3f\", $MOVETIME_MS / 1000.0 }")
ROUNDS=$(( GAMES / 2 ))

echo "════════════════════════════════════════════════════════════"
echo "  A/B gauntlet: $REF_A vs $REF_B"
echo "  games:       $GAMES  (rounds: $ROUNDS)"
echo "  concurrency: $CONCURRENCY  ($(nproc) cores)"
echo "  movetime:    ${MOVETIME_MS}ms per side per move"
echo "  openings:    $OPENINGS"
echo "  pgn out:     $PGN_OUT"
echo "════════════════════════════════════════════════════════════"

# ── Pick match runner ───────────────────────────────────────────
if command -v fast-chess >/dev/null;       then MATCH_BIN=fast-chess
elif command -v cutechess-cli >/dev/null;  then MATCH_BIN=cutechess-cli
else
    echo "ERROR: neither fast-chess nor cutechess-cli on PATH"
    echo "Install fast-chess: see tools/wsl-gauntlet.sh comment header"
    exit 1
fi
echo "  match runner: $MATCH_BIN"

# ── Sanity checks ───────────────────────────────────────────────
[ -f "$OPENINGS" ] || { echo "ERROR: openings file not found: $OPENINGS"; exit 1; }

# Refuse with uncommitted changes — we're about to git checkout, would clobber
if ! git diff --quiet || ! git diff --cached --quiet; then
    echo "ERROR: working tree has uncommitted changes."
    echo "       commit, stash (git stash), or revert before running A/B."
    exit 1
fi
# Resolve a ref: if not local, try fetching from origin and creating a local
# branch. Avoids the common "git pull doesn't update other branches" gotcha.
ensure_ref() {
    local ref="$1"
    git rev-parse --verify "$ref" >/dev/null 2>&1 && return 0
    echo "[note] '$ref' not local — trying to fetch from origin"
    if git fetch --quiet origin "$ref:$ref" 2>/dev/null; then
        return 0
    fi
    # Fallback: fetch as origin/<ref> (read-only)
    git fetch --quiet origin "$ref" 2>/dev/null \
        && git rev-parse --verify "origin/$ref" >/dev/null 2>&1
}
ensure_ref "$REF_A" \
    || { echo "ERROR: ref-A '$REF_A' not found locally or in origin"; exit 1; }
ensure_ref "$REF_B" \
    || { echo "ERROR: ref-B '$REF_B' not found locally or in origin"; exit 1; }

ORIG_REF=$(git rev-parse --abbrev-ref HEAD)
[ "$ORIG_REF" = "HEAD" ] && ORIG_REF=$(git rev-parse HEAD)
echo "  starting on: $ORIG_REF"

# Restore original ref on exit (incl. errors), so a botched build doesn't
# leave the user stranded on the wrong branch.
cleanup() {
    echo "  restoring to $ORIG_REF"
    git checkout --quiet -- build-info.json 2>/dev/null || true
    git checkout "$ORIG_REF" >/dev/null 2>&1 || true
}
trap cleanup EXIT

build_ref() {
    local ref="$1" tag="$2"
    echo
    echo "[build $tag] git checkout $ref"
    git checkout --quiet "$ref"
    make -s release
    # make regenerates tracked build artifacts (build-info.json); discard them
    # so they can't block the next checkout. Safe: the tree was clean at start,
    # so anything dirty here was created by our own build.
    git checkout --quiet -- build-info.json 2>/dev/null || true
    cp chess-engine-c "chess-engine-c.$tag"
    echo "[build $tag] → chess-engine-c.$tag"
}

build_ref "$REF_A" "A"
build_ref "$REF_B" "B"

# Back to original ref before the gauntlet (and trap will re-do on exit).
git checkout --quiet "$ORIG_REF"

ls -la chess-engine-c.A chess-engine-c.B

# ── Run the gauntlet ───────────────────────────────────────────
case "$MATCH_BIN" in
    fast-chess)    PGNOUT_ARGS=(-pgnout "file=$PGN_OUT") ;;
    cutechess-cli) PGNOUT_ARGS=(-pgnout "$PGN_OUT")      ;;
esac

# Sanitize ref names for use as fast-chess engine names (no slashes/spaces).
NAME_A=$(echo "$REF_A" | tr '/ ' '__')
NAME_B=$(echo "$REF_B" | tr '/ ' '__')

echo
echo "[$MATCH_BIN] $REF_A vs $REF_B starting…  (progress + ETA via gauntlet-progress.py)"
echo
"$MATCH_BIN" \
    -engine cmd="$REPO/chess-engine-c.A" name="$NAME_A" proto=uci \
    -engine cmd="$REPO/chess-engine-c.B" name="$NAME_B" proto=uci \
    -each tc=inf st="$ST_SEC" \
    -games 2 -rounds "$ROUNDS" \
    -concurrency "$CONCURRENCY" \
    -openings file="$OPENINGS" format=epd order=random \
    -repeat \
    -recover \
    "${PGNOUT_ARGS[@]}" \
    -ratinginterval 10 \
    2>&1 | python3 "$REPO/tools/gauntlet-progress.py" "$GAMES"

echo
echo "═══════════════════════════════════════════════════════════"
echo "  PGN saved to: $PGN_OUT"
echo "  Elo from fast-chess output is from $NAME_A's perspective."
echo "  Positive Elo → $NAME_A is stronger; negative → $NAME_B is stronger."
echo "═══════════════════════════════════════════════════════════"
