#!/usr/bin/env bash
# Regenerate build-info.json from the live git log.
#
# Run manually or via `make build-info`. The dashboard's "Engine build" card
# reads this file via /api/build-info — auto-generating from git history
# means the panel can't drift out of sync with the running code.

set -euo pipefail
cd "$(dirname "$0")/.."
exec python3 tools/gen_build_info.py
