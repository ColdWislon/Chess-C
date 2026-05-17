#!/usr/bin/env bash
# Nightly Stockfish gauntlet for chess-c.
#
# Flow:
#   1. Wait up to IDLE_WAIT_MAX_S for rpiBot73 to be idle (no live game).
#   2. Stop lichess-bot-c so the gauntlet has all 4 Pi cores.
#   3. Run python3 ladder_match.py under a hard wall-time cap.
#   4. Archive log + PGN with a date suffix (kept forever — see CLAUDE.md).
#   5. Always restart lichess-bot-c, even if the gauntlet failed.
#   6. Emit a single one-line summary so `journalctl -u chess-c-gauntlet`
#      tells the story without scrolling.
#
# Designed to be invoked by chess-c-gauntlet.service (Type=oneshot, root).

set -u  # not -e: we want explicit error handling so the bot ALWAYS comes back

REPO="/home/bertrand/chess-c"
LADDER="${REPO}/ladder_match.py"
LOG="${REPO}/ladder_match.log"
PGN="${REPO}/ladder_match.pgn"
ARCHIVE_DIR="${REPO}/gauntlets"
SERVICE="lichess-bot-c"
DASH_URL="http://127.0.0.1:8080/api/status?bot=rpibot73"

IDLE_POLL_S="${IDLE_POLL_S:-30}"
IDLE_WAIT_MAX_S="${IDLE_WAIT_MAX_S:-1800}"   # 30 min cap on idle wait
GAUNTLET_TIMEOUT="${GAUNTLET_TIMEOUT:-6h}"   # hard wall-time, kills hangs

ts() { date '+%Y-%m-%d %H:%M:%S'; }
say() { echo "[$(ts)] $*"; }

# ── 1. Wait for idle ──────────────────────────────────────────────────
say "starting nightly gauntlet; waiting for rpiBot73 to be idle"
waited=0
while [[ $waited -lt $IDLE_WAIT_MAX_S ]]; do
    game=$(curl -fsS --max-time 5 "$DASH_URL" 2>/dev/null \
        | python3 -c "import sys,json; g=json.load(sys.stdin).get('current_game'); print((g or {}).get('id',''))" 2>/dev/null || true)
    if [[ -z "$game" ]]; then
        say "bot is idle"
        break
    fi
    say "bot is in game $game; sleeping ${IDLE_POLL_S}s"
    sleep "$IDLE_POLL_S"
    waited=$((waited + IDLE_POLL_S))
done
if [[ -n "${game:-}" ]]; then
    say "GAVE UP: bot still in game $game after ${IDLE_WAIT_MAX_S}s — skipping gauntlet"
    exit 1
fi

# ── 2. Stop the bot so the gauntlet has the Pi to itself ──────────────
say "stopping ${SERVICE}"
if ! systemctl stop "$SERVICE"; then
    say "FAILED to stop ${SERVICE}; aborting (bot left untouched)"
    exit 1
fi

# ── 3. Run the gauntlet ───────────────────────────────────────────────
mkdir -p "$ARCHIVE_DIR"
start_ts=$(date +%s)
say "running gauntlet (timeout=${GAUNTLET_TIMEOUT})"
gauntlet_rc=0
timeout "$GAUNTLET_TIMEOUT" python3 "$LADDER" || gauntlet_rc=$?
elapsed_min=$(( ($(date +%s) - start_ts) / 60 ))
say "gauntlet finished rc=${gauntlet_rc} in ~${elapsed_min} min"

# ── 4. Archive the outputs even if the run errored out (partial data is
#       still useful) ──────────────────────────────────────────────────
stamp=$(date +%Y-%m-%d)
if [[ -f "$LOG" ]]; then
    cp -p "$LOG" "${ARCHIVE_DIR}/ladder_match-${stamp}.log"
    say "archived log → ${ARCHIVE_DIR}/ladder_match-${stamp}.log"
fi
if [[ -f "$PGN" ]]; then
    cp -p "$PGN" "${ARCHIVE_DIR}/ladder_match-${stamp}.pgn"
    say "archived pgn → ${ARCHIVE_DIR}/ladder_match-${stamp}.pgn"
fi

# ── 5. Always restart the bot ─────────────────────────────────────────
say "starting ${SERVICE}"
if ! systemctl start "$SERVICE"; then
    say "WARNING: failed to start ${SERVICE} — manual intervention required"
fi

# ── 6. One-line summary so the journal is grep-able ───────────────────
# Pull the FINAL LADDER block from the just-finished log.
summary="(no FINAL LADDER block in log)"
if [[ -f "$LOG" ]]; then
    summary=$(awk '/== FINAL LADDER ==/,0' "$LOG" \
        | grep -E "SF skill" \
        | tr '\n' '; ' | sed 's/; $//')
fi
say "SUMMARY rc=${gauntlet_rc} duration=${elapsed_min}min ${summary}"

exit "$gauntlet_rc"
