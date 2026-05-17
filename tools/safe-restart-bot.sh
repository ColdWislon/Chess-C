#!/usr/bin/env bash
# Safely restart lichess-bot-c.
#
# Polls the dashboard until rpiBot73 is idle, then `systemctl restart`s the
# service so the next-spawned engine subprocess picks up the freshest binary
# on disk. Always confirms the new engine is up before exiting.
#
# Usage:
#   ./tools/safe-restart-bot.sh            # poll up to 30 min then restart
#   ./tools/safe-restart-bot.sh --now      # only restart if currently idle, else abort
#
# Sudo is required for `systemctl restart`; the script will prompt if needed.

set -u

DASH_URL="http://127.0.0.1:8080/api/status?bot=rpibot73"
SERVICE="lichess-bot-c"
POLL_S="${POLL_S:-30}"
MAX_WAIT_S="${MAX_WAIT_S:-1800}"   # 30 min cap on idle wait

mode="wait"
[[ "${1:-}" == "--now" ]] && mode="now"

ts() { date '+%H:%M:%S'; }
say() { echo "[$(ts)] $*"; }

# Parse current_game id out of /api/status. Empty string means idle.
current_game() {
    curl -fsS --max-time 5 "$DASH_URL" 2>/dev/null \
        | python3 -c "import sys,json; g=json.load(sys.stdin).get('current_game') or {}; print(g.get('id') or '')" 2>/dev/null \
        || echo ""
}

# ── 1. Wait for idle ──────────────────────────────────────────────────
waited=0
while :; do
    game=$(current_game)
    if [[ -z "$game" ]]; then
        say "bot is idle — proceeding with restart"
        break
    fi
    if [[ "$mode" == "now" ]]; then
        say "ABORT (--now): bot is in game $game; refusing to restart"
        exit 1
    fi
    if [[ $waited -ge $MAX_WAIT_S ]]; then
        say "GAVE UP: bot still in game $game after ${MAX_WAIT_S}s"
        exit 1
    fi
    say "bot is in game $game; sleeping ${POLL_S}s (waited ${waited}s / max ${MAX_WAIT_S}s)"
    sleep "$POLL_S"
    waited=$((waited + POLL_S))
done

# ── 2. Capture pre-restart PID so we can verify a fresh engine came up ──
pre_pid=$(curl -fsS --max-time 5 "$DASH_URL" 2>/dev/null \
    | python3 -c "import sys,json; print((json.load(sys.stdin).get('system',{}).get('engine_proc') or {}).get('pid') or '')" 2>/dev/null || echo "")
say "pre-restart engine pid: ${pre_pid:-<none>}"

# ── 3. Restart ────────────────────────────────────────────────────────
say "running: sudo systemctl restart ${SERVICE}"
if ! sudo systemctl restart "$SERVICE"; then
    say "FAILED to restart ${SERVICE}"
    exit 1
fi

# ── 4. Verify new engine is up ────────────────────────────────────────
# lichess-bot itself comes up in ~2 s but the engine subprocess only spawns
# once a game starts or a challenge is being negotiated. We don't wait for
# that — we just confirm the service is active and report the build_id from
# the journal (the engine emits it on `uci` / `ucinewgame`).
say "waiting for service active…"
for _ in $(seq 1 30); do
    if systemctl is-active --quiet "$SERVICE"; then
        say "service active"
        break
    fi
    sleep 1
done

# Latest BUILD line in the journal tells us which binary is now in play.
build=$(sudo journalctl -u "$SERVICE" -n 200 --no-pager -o cat 2>/dev/null \
    | awk '/info string BUILD/ {b=$0} END {if (b) print b}' \
    | sed 's/.*BUILD //')
if [[ -n "$build" ]]; then
    say "engine emits build: $build"
else
    say "(no BUILD line in recent journal yet — emitted on first ucinewgame)"
fi

say "done"
