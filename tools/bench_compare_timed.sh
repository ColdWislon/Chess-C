#!/usr/bin/env bash
# Timed A/B-compare: each binary gets a fixed time budget per position. Reports
# total depth reached and budget utilization. Use for changes affecting time
# management or stopping behavior, where the deterministic depth-N bench can't
# detect the difference.
#
# Usage:
#   make bench-baseline             # before your change
#   <edit code>
#   make bench-compare-timed BENCH_MS=1000   # after the change
#
# Or directly: ./tools/bench_compare_timed.sh [MS_PER_POSITION]
#
# Headline metric is total_depth (sum of depths reached across 8 positions).
# Higher = better. Budget utilization shows what fraction of the allotted time
# was actually consumed — a low number can indicate an aborted-too-early bug.

set -euo pipefail

MS="${1:-1000}"
RUNS="${BENCH_RUNS:-3}"
REPO="$(cd "$(dirname "$0")/.." && pwd)"
NEW="$REPO/chess-engine-c"
OLD="$REPO/chess-engine-c.baseline"

if [ ! -x "$NEW" ]; then echo "error: $NEW not built" >&2; exit 1; fi
if [ ! -x "$OLD" ]; then echo "error: no baseline at $OLD (run 'make bench-baseline')" >&2; exit 1; fi

if command -v vcgencmd >/dev/null 2>&1; then
    THR="$(vcgencmd get_throttled 2>/dev/null | awk -F= '{print $2}')"
    if [ -n "$THR" ] && [ "$THR" != "0x0" ]; then
        echo "⚠ Pi reports throttled=$THR. Min-of-N still meaningful." >&2
    fi
fi

run_once() {
    local bin="$1"
    printf 'bench_timed %s\nquit\n' "$MS" | "$bin" 2>/dev/null \
        | grep -E "^(Total elapsed|Total depth|Total nodes|Budget utilization)" \
        | awk -F: '{print $2}' | tr -d ' %'
}

OLD_DEPTHS=(); OLD_ELAPSED=(); OLD_UTILS=(); OLD_NODES=""
NEW_DEPTHS=(); NEW_ELAPSED=(); NEW_UTILS=(); NEW_NODES=""

echo "── $RUNS interleaved iteration(s), $MS ms/position (baseline ↔ current)..." >&2
for i in $(seq 1 "$RUNS"); do
    mapfile -t o < <(run_once "$OLD")
    mapfile -t n < <(run_once "$NEW")
    # grep order: Total elapsed, Total budget(?no — Budget utilization comes after Total budget which we don't grep), Budget utilization, Total depth, Total nodes
    # Actually re-checking: bench.c prints in this order:
    #   Total elapsed (ms) : X
    #   Total budget (ms)  : X      (not grepped)
    #   Budget utilization : X%
    #   Total depth        : X
    #   Total nodes        : X
    # So our grep -E filter gets: elapsed, utilization, depth, nodes (4 values).
    OLD_ELAPSED+=("${o[0]}")
    OLD_UTILS+=("${o[1]}")
    OLD_DEPTHS+=("${o[2]}")
    OLD_NODES="${o[3]}"

    NEW_ELAPSED+=("${n[0]}")
    NEW_UTILS+=("${n[1]}")
    NEW_DEPTHS+=("${n[2]}")
    NEW_NODES="${n[3]}"

    printf "  iter %d/%d: baseline depth=%s util=%s%%  ↔  current depth=%s util=%s%%\n" \
        "$i" "$RUNS" "${o[2]}" "${o[1]}" "${n[2]}" "${n[1]}" >&2
done

max()    { printf '%s\n' "$@" | sort -nr | head -1; }
avg()    { printf '%s\n' "$@" | awk '{ s+=$1; n++ } END { if (n) printf "%.1f", s/n }'; }
median() { printf '%s\n' "$@" | sort -n | awk 'BEGIN{c=0} {a[c++]=$1} END{ if (c%2) print a[int(c/2)]; else printf "%.1f", (a[c/2-1]+a[c/2])/2 }'; }

OLD_MAX_DEPTH=$(max "${OLD_DEPTHS[@]}");    NEW_MAX_DEPTH=$(max "${NEW_DEPTHS[@]}")
OLD_AVG_DEPTH=$(avg "${OLD_DEPTHS[@]}");    NEW_AVG_DEPTH=$(avg "${NEW_DEPTHS[@]}")
OLD_MED_DEPTH=$(median "${OLD_DEPTHS[@]}"); NEW_MED_DEPTH=$(median "${NEW_DEPTHS[@]}")
OLD_AVG_UTIL=$(avg "${OLD_UTILS[@]}");      NEW_AVG_UTIL=$(avg "${NEW_UTILS[@]}")

delta()  { awk -v a="$1" -v b="$2" 'BEGIN { printf "%+.1f", a-b }'; }
pct()    { awk -v a="$1" -v b="$2" 'BEGIN { if (b+0 == 0) print "n/a"; else printf "%+.2f%%", (a-b)*100.0/b }'; }

printf "\n"
printf "  (median is the headline — robust to single lucky/unlucky CPU bursts.\n"
printf "   max is best-case, avg is point estimate. higher depth = better.)\n\n"
printf "                       %15s   %15s   %s\n" "BEFORE" "AFTER" "Δ"
printf "  Median depth       : %15s   %15s   %s (%s)\n" \
    "$OLD_MED_DEPTH" "$NEW_MED_DEPTH" "$(delta "$NEW_MED_DEPTH" "$OLD_MED_DEPTH")" "$(pct "$NEW_MED_DEPTH" "$OLD_MED_DEPTH")"
printf "  Avg depth          : %15s   %15s   %s (%s)\n" \
    "$OLD_AVG_DEPTH" "$NEW_AVG_DEPTH" "$(delta "$NEW_AVG_DEPTH" "$OLD_AVG_DEPTH")" "$(pct "$NEW_AVG_DEPTH" "$OLD_AVG_DEPTH")"
printf "  Max depth          : %15s   %15s   %s\n" \
    "$OLD_MAX_DEPTH" "$NEW_MAX_DEPTH" "$(delta "$NEW_MAX_DEPTH" "$OLD_MAX_DEPTH")"
printf "  Avg budget util %% : %15s   %15s   %s pp\n" \
    "$OLD_AVG_UTIL" "$NEW_AVG_UTIL" "$(delta "$NEW_AVG_UTIL" "$OLD_AVG_UTIL")"
printf "\n"
