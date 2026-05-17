#!/usr/bin/env bash
# A/B-compare the current build against a stashed baseline binary.
#
# Usage:
#   make bench-baseline           # before your change
#   <edit code>
#   make bench-compare            # after the change
#
# Or directly:
#   ./tools/bench_compare.sh [DEPTH]
#
# Runs both binaries at fixed depth and prints node/time/nps deltas. Bench is
# deterministic by construction (single thread, TT cleared between positions,
# no time pressure), so total_nodes should be identical for pure perf changes.
# If it differs, the change altered the search tree — flagged as ⚠ in output.
#
# Why interleaved iterations: the Pi 4 throttles aggressively under
# under-voltage / heat, so a single back-to-back A then B comparison can show
# 1.5-2x variance for the SAME binary. Interleaving (A B A B A B) keeps both
# binaries on the same thermal state per iteration. We take the BEST (min)
# time of each binary — that's the run closest to nominal CPU frequency.
# Per-iteration ratios are also reported so the user can see noise level.

set -euo pipefail

DEPTH="${1:-8}"
RUNS="${BENCH_RUNS:-3}"
REPO="$(cd "$(dirname "$0")/.." && pwd)"
NEW="$REPO/chess-engine-c"
OLD="$REPO/chess-engine-c.baseline"

if [ ! -x "$NEW" ]; then
    echo "error: $NEW not built (run 'make release')" >&2
    exit 1
fi
if [ ! -x "$OLD" ]; then
    echo "error: no baseline at $OLD" >&2
    echo "  build before your change, then run: make bench-baseline" >&2
    exit 1
fi

# Throttling warning — surfaces silent slowdowns the user should know about
if command -v vcgencmd >/dev/null 2>&1; then
    THR="$(vcgencmd get_throttled 2>/dev/null | awk -F= '{print $2}')"
    if [ -n "$THR" ] && [ "$THR" != "0x0" ]; then
        echo "⚠ Pi reports throttled=$THR (likely under-voltage or thermal cap)." >&2
        echo "  Numbers will still be valid via interleaved-min, but absolute NPS will be low." >&2
    fi
fi

run_once() {
    # Capture "Total time (ms) : N" and "Total nodes : N" only
    local bin="$1"
    printf 'bench %s\nquit\n' "$DEPTH" | "$bin" 2>/dev/null \
        | grep -E "^(Total time|Total nodes)" \
        | awk -F: '{print $2}' | tr -d ' '
}

# Collect $RUNS interleaved iterations
OLD_TIMES=()
NEW_TIMES=()
OLD_NODES=""
NEW_NODES=""

echo "── $RUNS interleaved iteration(s) at depth $DEPTH (baseline ↔ current)..." >&2
for i in $(seq 1 "$RUNS"); do
    # OLD first
    read -r ot on < <(run_once "$OLD" | xargs)
    # NEW second
    read -r nt nn < <(run_once "$NEW" | xargs)

    OLD_TIMES+=("$ot")
    NEW_TIMES+=("$nt")
    OLD_NODES="$on"
    NEW_NODES="$nn"

    ratio=$(awk -v a="$nt" -v b="$ot" 'BEGIN { if (b == 0) print "n/a"; else printf "%.3f", a/b }')
    printf "  iter %d/%d: baseline=%sms current=%sms  ratio=%s\n" \
        "$i" "$RUNS" "$ot" "$nt" "$ratio" >&2
done

# Min time across iterations is most representative on throttled hardware
min() { printf '%s\n' "$@" | sort -n | head -1; }
OLD_MIN=$(min "${OLD_TIMES[@]}")
NEW_MIN=$(min "${NEW_TIMES[@]}")

OLD_NPS=$(awk -v n="$OLD_NODES" -v t="$OLD_MIN" 'BEGIN { if (t == 0) print 0; else printf "%d", n*1000/t }')
NEW_NPS=$(awk -v n="$NEW_NODES" -v t="$NEW_MIN" 'BEGIN { if (t == 0) print 0; else printf "%d", n*1000/t }')

pct()   { awk -v a="$1" -v b="$2" 'BEGIN { if (b == 0) print "n/a"; else printf "%+.2f%%", (a-b)*100.0/b }'; }
delta() { awk -v a="$1" -v b="$2" 'BEGIN { printf "%+d", a-b }'; }

NODE_FLAG=""
if [ "$OLD_NODES" != "$NEW_NODES" ]; then
    NODE_FLAG=" ⚠ search tree changed"
fi

printf "\n"
printf "  (taking MIN time across %d iter — most representative under throttling)\n\n" "$RUNS"
printf "                     %15s   %15s   %s\n" "BEFORE" "AFTER" "Δ"
printf "  Min time (ms)    : %15s   %15s   %s (%s)\n" \
    "$OLD_MIN"   "$NEW_MIN"   "$(delta "$NEW_MIN" "$OLD_MIN")"     "$(pct "$NEW_MIN" "$OLD_MIN")"
printf "  Total nodes      : %15s   %15s   %s%s\n" \
    "$OLD_NODES" "$NEW_NODES" "$(delta "$NEW_NODES" "$OLD_NODES")" "$NODE_FLAG"
printf "  Nodes/second     : %15s   %15s   %s (%s)\n" \
    "$OLD_NPS"   "$NEW_NPS"   "$(delta "$NEW_NPS" "$OLD_NPS")"     "$(pct "$NEW_NPS" "$OLD_NPS")"
printf "\n"

# ── Persist a JSON summary the dashboard reads ──────────────────────────
# `last-bench.json` is the single source of truth for the "Last A/B bench"
# panel. Overwritten by every run so the dashboard always shows the most
# recent comparison. Best-effort: don't fail the whole script if write fails.
OUT="$REPO/last-bench.json"
DELTA_MS_PCT=$(awk -v a="$NEW_MIN" -v b="$OLD_MIN" 'BEGIN { if (b == 0) print "null"; else printf "%.2f", (a-b)*100.0/b }')
DELTA_NODES=$(awk -v a="$NEW_NODES" -v b="$OLD_NODES" 'BEGIN { printf "%d", a-b }')
TS=$(date -u +"%Y-%m-%dT%H:%M:%SZ")
TREE_CHANGED=$([ "$OLD_NODES" != "$NEW_NODES" ] && echo "true" || echo "false")
if command -v git >/dev/null 2>&1; then
    CUR_SHA=$(cd "$REPO" && git rev-parse --short HEAD 2>/dev/null || echo "unknown")
else
    CUR_SHA="unknown"
fi
cat > "$OUT" <<EOF
{
  "depth":          $DEPTH,
  "runs":           $RUNS,
  "delta_ms_pct":   $DELTA_MS_PCT,
  "delta_nodes":    $DELTA_NODES,
  "tree_changed":   $TREE_CHANGED,
  "baseline_ms":    $OLD_MIN,
  "current_ms":     $NEW_MIN,
  "baseline_nodes": $OLD_NODES,
  "current_nodes":  $NEW_NODES,
  "current_sha":    "$CUR_SHA",
  "timestamp":      "$TS"
}
EOF
echo "  → wrote $OUT" >&2
