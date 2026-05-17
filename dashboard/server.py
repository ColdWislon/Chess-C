#!/usr/bin/env python3
"""
Lightweight dashboard server for rpiBot73.
Serves the HTML page and exposes /api/status?bot=, /api/log?bot=,
and /api/engine-stream?bot= (SSE).

Tokens are read from environment variables to keep them out of source control.
Each bot entry's `token_env` names the env var; the systemd unit / .env file
provides the actual value at runtime.
"""
import json
import os
import subprocess
import re
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import urlparse, parse_qs
from urllib.request import urlopen, Request

BOTS = {
    "rpibot73": {
        "name":      "rpiBot73",
        "engine":    "C",
        "token_env": "LICHESS_TOKEN_RPIBOT73",
        "service":   "lichess-bot-c",
    },
}
DEFAULT_BOT = "rpibot73"


def _resolve_token(cfg: dict) -> str | None:
    return os.environ.get(cfg.get("token_env", ""))

PORT = 8080
HTML = Path(__file__).parent / "index.html"


def _bot_cfg(key: str) -> dict:
    return BOTS.get((key or "").lower(), BOTS[DEFAULT_BOT])


def lichess_get(path: str, token: str, ndjson: bool = False) -> dict | list | None:
    url = f"https://lichess.org{path}"
    headers = {"Authorization": f"Bearer {token}"}
    if ndjson:
        headers["Accept"] = "application/x-ndjson"
    req = Request(url, headers=headers)
    try:
        with urlopen(req, timeout=5) as r:
            body = r.read().decode()
        lines = [l for l in body.splitlines() if l.strip()]
        if ndjson:
            return [json.loads(l) for l in lines]
        return json.loads(lines[0]) if lines else None
    except Exception:
        return None


def _service_active(service: str) -> bool:
    try:
        out = subprocess.check_output(
            ["systemctl", "is-active", service],
            stderr=subprocess.DEVNULL, timeout=2, text=True
        )
        return out.strip() == "active"
    except Exception:
        return False


def activity_log(service: str) -> list:
    """Return recent meaningful events from the given bot's journal."""
    try:
        out = subprocess.check_output(
            ["journalctl", "-u", service, "-n", "500", "--no-pager", "-o", "short-iso"],
            stderr=subprocess.DEVNULL, timeout=3, text=True,
        )
    except Exception:
        return []

    events = []
    patterns = [
        (r"Will challenge (\S+) for",        "challenge", lambda m: f"Challenging {m[1]}"),
        (r"BOT (\S+) .* declined .+?: (.+)", "declined",  lambda m: f"{m[1]} declined: {m[2].strip()}"),
        (r"BOT (\S+) .* declined",           "declined",  lambda m: f"{m[1]} declined"),
        (r"Game ID:\s*(\S+)",                "game",      lambda m: f"Game started: {m[1]}"),
        (r"Game (\S+) is over",              "finished",  lambda m: f"Game over: {m[1]}"),
        (r"Challenge id is (\S+)\.",         None,        None),
        (r"Welcome (\S+)!",                  "connected", lambda m: f"Connected as {m[1]}"),
        (r"You're now connected",            "connected", lambda m: "Awaiting challenges"),
        (r"Next challenge will be created",  None,        None),
        (r"No suitable bots found",          "error",     lambda m: "No suitable bots found"),
    ]

    for line in out.splitlines():
        ts_match = re.match(r"(\S+)\s+\S+\s+\S+\[\d+\]:\s*(.*)", line)
        if not ts_match:
            continue
        ts_raw, msg = ts_match[1], ts_match[2].strip()
        msg = re.sub(r"\x1b\[[0-9;]*m", "", msg).strip()
        if not msg:
            continue
        for pattern, kind, fmt in patterns:
            m = re.search(pattern, msg)
            if m:
                if kind is None:
                    break
                events.append({"ts": ts_raw, "kind": kind, "msg": fmt(m)})
                break

    return list(reversed(events[-60:]))


INFO_FIELDS = [
    ("depth",    r"depth (\d+)"),
    ("seldepth", r"seldepth (\d+)"),
    ("score_cp", r"score cp (-?\d+)"),
    ("nodes",    r"nodes (\d+)"),
    ("nps",      r"nps (\d+)"),
    ("time",     r"time (\d+)"),
    ("hashfull", r"hashfull (\d+)"),
    ("hashmb",   r"hashmb (\d+)"),
    ("tthits",   r"tthits (\d+)"),
    ("ttprobes", r"ttprobes (\d+)"),
    ("qnodes",   r"qnodes (\d+)"),
    ("cutoffs",  r"cutoffs (\d+)"),
    ("cutoffs1", r"cutoffs1 (\d+)"),
    ("nullcuts", r"nullcuts (\d+)"),
]

BUILD_INFO_PATH = Path("/home/bertrand/chess-c/build-info.json")


def parse_info(line: str) -> dict:
    if "info depth" not in line:
        return {}
    out = {}
    for key, pat in INFO_FIELDS:
        m = re.search(pat, line)
        if m:
            out[key] = int(m.group(1))
    return out


def engine_stats(service: str) -> dict:
    """Parse the last 'info depth' line from the bot's journal."""
    try:
        out = subprocess.check_output(
            ["journalctl", "-u", service, "-n", "200", "--no-pager", "-o", "cat"],
            stderr=subprocess.DEVNULL, timeout=3, text=True,
        )
    except Exception:
        return {}

    for line in reversed(out.splitlines()):
        info = parse_info(line)
        if info and "depth" in info:
            return info
    return {}


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def _bot_param(self) -> dict:
        qs = parse_qs(urlparse(self.path).query)
        return _bot_cfg((qs.get("bot") or [DEFAULT_BOT])[0])

    def do_GET(self):
        path = urlparse(self.path).path
        if path == "/":
            self._serve_file(HTML, "text/html")
        elif path == "/api/bots":
            self._serve_json([
                {"key": k, "name": v["name"], "engine": v["engine"]}
                for k, v in BOTS.items()
            ])
        elif path == "/api/status":
            self._serve_json(self._build_status(self._bot_param()))
        elif path == "/api/log":
            self._serve_json(activity_log(self._bot_param()["service"]))
        elif path == "/api/build-info":
            try:
                self._serve_json(json.loads(BUILD_INFO_PATH.read_text()))
            except Exception:
                self._serve_json({})
        elif path == "/api/engine-stream":
            self._serve_engine_sse(self._bot_param()["service"])
        else:
            self.send_error(404)

    def _serve_file(self, path: Path, content_type: str):
        try:
            data = path.read_bytes()
            self.send_response(200)
            self.send_header("Content-Type", content_type)
            self.send_header("Content-Length", len(data))
            self.end_headers()
            self.wfile.write(data)
        except FileNotFoundError:
            self.send_error(404)

    def _serve_json(self, obj):
        data = json.dumps(obj).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Content-Length", len(data))
        self.end_headers()
        self.wfile.write(data)

    def _serve_engine_sse(self, service: str):
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Cache-Control", "no-cache")
        self.send_header("Access-Control-Allow-Origin", "*")
        self.end_headers()

        proc = subprocess.Popen(
            ["journalctl", "-u", service, "-f", "-o", "cat", "-n", "0"],
            stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
        )
        try:
            for raw in proc.stdout:
                line = raw.decode(errors="replace")
                info = parse_info(line)
                if info and "depth" in info:
                    payload = json.dumps(info)
                    self.wfile.write(f"data: {payload}\n\n".encode())
                    self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError, OSError):
            pass
        finally:
            proc.terminate()
            proc.wait()

    def _build_status(self, bot: dict) -> dict:
        name    = bot["name"]
        token   = _resolve_token(bot)
        service = bot["service"]
        if not token:
            return {"bot": name, "engine": bot["engine"], "online": False,
                    "error": f"Missing env var {bot.get('token_env')}",
                    "ratings": {}, "recent_games": [], "engine_stats": {}}

        user = lichess_get(f"/api/user/{name.lower()}", token) or {}

        current_game = None
        playing_url = user.get("playing")
        if playing_url:
            m = re.search(r"/([A-Za-z0-9]{8})", playing_url)
            if m:
                game_id = m.group(1)
                game = lichess_get(f"/api/game/{game_id}", token) or {}
                if game.get("id"):
                    current_game = game

        recent_raw = lichess_get(
            f"/api/games/user/{name.lower()}?max=10&moves=false&clocks=false&opening=true",
            token, ndjson=True,
        )
        recent = recent_raw or []

        is_online = playing_url is not None or _service_active(service)

        perfs = user.get("perfs") or {}
        count = user.get("count") or {}
        play_time = user.get("playTime") or {}

        ratings = {}
        for tc in ("bullet", "blitz", "rapid", "classical"):
            p = perfs.get(tc, {})
            if p.get("games", 0) > 0:
                ratings[tc] = {"rating": p.get("rating"), "games": p.get("games"), "prog": p.get("prog", 0)}

        return {
            "bot":               name,
            "engine":            bot["engine"],
            "online":            is_online,
            "ratings":           ratings,
            "rating":            perfs.get("rapid", {}).get("rating"),
            "games_played":      count.get("all", 0),
            "wins":              count.get("win", 0),
            "losses":            count.get("loss", 0),
            "draws":             count.get("draw", 0),
            "play_time_seconds": play_time.get("total", 0),
            "current_game":      current_game,
            "recent_games":      recent[:10],
            "engine_stats":      engine_stats(service),
        }


if __name__ == "__main__":
    print(f"Dashboard running at http://0.0.0.0:{PORT}")
    ThreadingHTTPServer(("0.0.0.0", PORT), Handler).serve_forever()
