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
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import urlparse, parse_qs
from urllib.request import urlopen, Request


# ── TTL cache ────────────────────────────────────────────────────────
# Wrap expensive fetches (journalctl, lichess API) so we don't pay for them
# on every /api/status poll. Different TTLs for different freshness needs.
_cache_lock = threading.Lock()
_cache: dict[str, tuple[float, object]] = {}


def cached(key: str, ttl_s: float, producer):
    """Return cached value for `key` if fresher than ttl_s, else call
    `producer()`, cache its result, return it. Thread-safe."""
    now = time.monotonic()
    with _cache_lock:
        hit = _cache.get(key)
        if hit and (now - hit[0]) < ttl_s:
            return hit[1]
    # Compute outside the lock — producer may itself be slow / make network
    # calls, and holding the lock during that would serialize unrelated
    # cache entries.
    val = producer()
    with _cache_lock:
        _cache[key] = (time.monotonic(), val)
    return val

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
    out = _journal_iso(service)
    if not out:
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
# `pv` is parsed separately because it's an unbounded string trailing the line.
PV_PATTERN = re.compile(r"\bpv ([a-h1-8 a-zA-Z0-9]+?)(?:\s*$)")
BUILD_PATTERN = re.compile(r"info string BUILD (\S+)")

BUILD_INFO_PATH    = Path("/home/bertrand/chess-c/build-info.json")
BENCH_RESULT_PATH  = Path("/home/bertrand/chess-c/last-bench.json")
GAUNTLET_LOG_PATH  = Path("/home/bertrand/chess-c/ladder_match.log")


def _journal_cat(service: str) -> str:
    """Most recent 4000 journal lines for `service` in compact format,
    cached 3 s. Shared across move_history / engine_stats / latest_build_id
    so a single /api/status request makes one journalctl invocation instead
    of three. 4000 is the deepest consumer's window — others slice this."""
    def fetch():
        try:
            return subprocess.check_output(
                ["journalctl", "-u", service, "-n", "4000", "--no-pager", "-o", "cat"],
                stderr=subprocess.DEVNULL, timeout=3, text=True,
            )
        except Exception:
            return ""
    return cached(f"journal_cat:{service}", 3.0, fetch)


def _journal_iso(service: str) -> str:
    """500 recent journal lines with iso-timestamp prefix, cached 3 s.
    Used by activity_log only — different format from _journal_cat."""
    def fetch():
        try:
            return subprocess.check_output(
                ["journalctl", "-u", service, "-n", "500", "--no-pager", "-o", "short-iso"],
                stderr=subprocess.DEVNULL, timeout=3, text=True,
            )
        except Exception:
            return ""
    return cached(f"journal_iso:{service}", 3.0, fetch)


# ── System stats ──────────────────────────────────────────────────────
# CPU usage needs two /proc/stat samples to derive a percent. We keep the
# previous sample at module scope; the difference between consecutive
# requests (every 5 s from the dashboard) is the reported CPU%.
_cpu_lock = threading.Lock()
_cpu_prev = {"total": 0, "idle": 0}


def _read_cpu_counters() -> tuple[int, int]:
    """Return (total_jiffies, idle_jiffies) from /proc/stat aggregate row."""
    with open("/proc/stat") as f:
        parts = f.readline().split()
    vals = list(map(int, parts[1:]))
    # user, nice, system, idle, iowait, irq, softirq, steal, guest, guest_nice
    idle = vals[3] + (vals[4] if len(vals) > 4 else 0)
    total = sum(vals)
    return total, idle


def cpu_percent() -> float | None:
    """Sampled CPU% across all cores. Returns None on the very first call
    (no baseline yet). Thread-safe."""
    total, idle = _read_cpu_counters()
    with _cpu_lock:
        prev_total = _cpu_prev["total"]
        prev_idle  = _cpu_prev["idle"]
        _cpu_prev["total"] = total
        _cpu_prev["idle"]  = idle
    if prev_total == 0:
        return None
    dt = total - prev_total
    di = idle  - prev_idle
    if dt <= 0:
        return 0.0
    return round(100.0 * (dt - di) / dt, 1)


def mem_stats() -> dict:
    """Memory usage from /proc/meminfo. Uses MemAvailable (kernel's own
    estimate of free-for-allocation memory, includes reclaimable cache)."""
    info = {}
    try:
        with open("/proc/meminfo") as f:
            for line in f:
                parts = line.split()
                if parts[0] in ("MemTotal:", "MemAvailable:"):
                    info[parts[0][:-1]] = int(parts[1])  # kB
    except Exception:
        return {}
    total = info.get("MemTotal", 0)
    avail = info.get("MemAvailable", 0)
    if not total:
        return {}
    used = total - avail
    return {
        "total_mb": total // 1024,
        "used_mb":  used  // 1024,
        "pct":      round(used * 100 / total, 1),
    }


def cpu_temp_c() -> float | None:
    """Pi CPU temperature in °C, read from the thermal sysfs (no subprocess)."""
    try:
        with open("/sys/class/thermal/thermal_zone0/temp") as f:
            return round(int(f.read().strip()) / 1000.0, 1)
    except Exception:
        return None


# `vcgencmd get_throttled` returns a bitmask. Bits 0-3 are "now"; bits 16-19
# are "ever since boot". See https://www.raspberrypi.com/documentation/computers/os.html#vcgencmd
_THROTTLE_BITS = {
    "undervolt":   (0x1,     0x10000),
    "freq_cap":    (0x2,     0x20000),
    "throttled":   (0x4,     0x40000),
    "soft_temp":   (0x8,     0x80000),
}


def power_status() -> dict:
    """Pi power/throttle state from vcgencmd. Returns active flags + history
    flags + a single overall status: 'ok', 'past' (throttled-before only), or
    'active' (currently throttled / under-voltage)."""
    try:
        out = subprocess.check_output(
            ["vcgencmd", "get_throttled"], stderr=subprocess.DEVNULL,
            timeout=2, text=True,
        ).strip()
    except Exception:
        return {"status": "unknown"}
    m = re.search(r"throttled=0x([0-9a-fA-F]+)", out)
    if not m:
        return {"status": "unknown"}
    bits = int(m.group(1), 16)

    now, ever = {}, {}
    any_now, any_ever = False, False
    for name, (now_bit, ever_bit) in _THROTTLE_BITS.items():
        n = bool(bits & now_bit)
        e = bool(bits & ever_bit)
        now[name]  = n
        ever[name] = e
        any_now  = any_now  or n
        any_ever = any_ever or e

    status = "active" if any_now else ("past" if any_ever else "ok")
    return {
        "status": status,
        "raw":    f"0x{bits:x}",
        "now":    now,
        "ever":   ever,
    }


def engine_proc_stats(service: str) -> dict:
    """Find the chess-engine-c subprocess inside the given service's cgroup
    and report its PID, RSS (MB), and uptime (s). The engine is a grandchild
    via python multiprocessing, so PPID-based lookup doesn't work; we walk
    the cgroup's process list instead — that's the authoritative service
    membership under systemd."""
    cgroup_paths = [
        f"/sys/fs/cgroup/system.slice/{service}.service/cgroup.procs",
        f"/sys/fs/cgroup/systemd/system.slice/{service}.service/cgroup.procs",
    ]
    pids: list[int] = []
    for p in cgroup_paths:
        try:
            with open(p) as f:
                pids = [int(x) for x in f.read().split() if x.strip()]
            break
        except FileNotFoundError:
            continue
        except Exception:
            return {}
    engine_pid = 0
    for pid in pids:
        try:
            with open(f"/proc/{pid}/comm") as f:
                if f.read().strip() == "chess-engine-c":
                    engine_pid = pid
                    break
        except Exception:
            pass
    if not engine_pid:
        return {}

    try:
        with open(f"/proc/{engine_pid}/status") as f:
            status = f.read()
        rss_kb_m = re.search(r"VmRSS:\s+(\d+)\s+kB", status)
        rss_mb = int(rss_kb_m.group(1)) // 1024 if rss_kb_m else None

        # uptime: now - process start time (start_time is in clock ticks since boot)
        with open(f"/proc/{engine_pid}/stat") as f:
            stat_fields = f.read().split()
        # field 22 (1-indexed): starttime in clock ticks
        starttime_ticks = int(stat_fields[21])
        clk_tck = os.sysconf("SC_CLK_TCK")
        with open("/proc/uptime") as f:
            sys_uptime = float(f.read().split()[0])
        proc_uptime_s = sys_uptime - (starttime_ticks / clk_tck)
        return {
            "pid":       engine_pid,
            "rss_mb":    rss_mb,
            "uptime_s":  int(proc_uptime_s),
        }
    except Exception:
        return {"pid": engine_pid}


def system_stats(service: str = "") -> dict:
    out = {
        "cpu_pct": cpu_percent(),
        "mem":     mem_stats(),
        "temp_c":  cpu_temp_c(),
        "power":   power_status(),
    }
    if service:
        out["engine_proc"] = engine_proc_stats(service)
    return out


def parse_info(line: str) -> dict:
    if "info depth" not in line:
        return {}
    out = {}
    for key, pat in INFO_FIELDS:
        m = re.search(pat, line)
        if m:
            out[key] = int(m.group(1))
    pvm = PV_PATTERN.search(line)
    if pvm:
        out["pv"] = pvm.group(1).strip()
    return out


def latest_build_id(service: str) -> str | None:
    """Most recent `info string BUILD <id>` line from the journal. Returns
    None until the engine has been restarted at least once with the new build."""
    out = _journal_cat(service)
    for line in reversed(out.splitlines()):
        m = BUILD_PATTERN.search(line)
        if m:
            return m.group(1)
    return None


def move_history(service: str, max_moves: int = 60) -> dict:
    """Walk recent journal lines and reconstruct per-move stats for the
    current game.

    Boundary used: lichess-bot's `Source: Engine` log line — fires once per
    move, AFTER engine.play() returns, so the journal interleaving between
    the engine subprocess and the python parent is already settled.

      - If info-depth lines preceded the `Source: Engine`: this was a
        searched move (we keep the highest-depth one).
      - If none preceded: this was a book move (the engine short-circuits
        in uci.c and emits no info lines).

    Returns:
      {
        "book_count":        int,   # contiguous book moves at game start
        "first_search_move": int,   # 1-indexed engine-move # where book ended
        "moves":             [ {move_num, depth, score_cp, time_ms, nodes, pv}, ... ]
      }

    `move_num` is the absolute engine-move index — 1 = engine's first move
    regardless of book — so the chart's x-axis matches the engine's actual
    move count, with book moves accounted for via book_count."""
    empty = {"book_count": 0, "first_search_move": 1, "moves": []}
    out = _journal_cat(service)
    if not out:
        return empty

    history: list[dict] = []
    last_info: dict = {}
    move_num = 0
    book_count = 0
    saw_search_yet = False

    for line in out.splitlines():
        # Game / engine boundary: reset everything.
        if "info string BUILD" in line or "Game over" in line or "Game won by" in line:
            history.clear()
            last_info = {}
            move_num = 0
            book_count = 0
            saw_search_yet = False
            continue
        info = parse_info(line)
        if info:
            last_info = info
            continue
        if "Source: Engine" in line and "engine_wrapper" in line:
            # One engine move just completed.
            move_num += 1
            if last_info:
                saw_search_yet = True
                history.append({
                    "move_num": move_num,
                    "depth":    last_info.get("depth"),
                    "score_cp": last_info.get("score_cp"),
                    "time_ms":  last_info.get("time"),
                    "nodes":    last_info.get("nodes"),
                    "pv":       last_info.get("pv"),
                })
            else:
                # No info-depth → book move. Count only the contiguous
                # opening-book prefix; once a real search has run we
                # don't reclassify later silent moves as book.
                if not saw_search_yet:
                    book_count += 1
            last_info = {}

    return {
        "book_count":        book_count,
        "first_search_move": book_count + 1,
        "moves":             history[-max_moves:],
    }


def last_bench_result() -> dict | None:
    """`bench_compare.sh` can be wired to write this file after each run.
    Until that's done, returns whatever's there (or None). Dashboard tolerates
    a missing file gracefully."""
    try:
        return json.loads(BENCH_RESULT_PATH.read_text())
    except Exception:
        return None


def last_gauntlet_result() -> dict | None:
    """Parse ladder_match.log. Format produced by ladder_match.py:

        == SF Skill 5 ==
          Game  1/20  (W)  W  153 plies  running 1.0/1
          Game  2/20  (B)  L  ...
          ...
          Final 18.5/20  (92%)  ≈ +436 elo vs SF skill 5

        == SF Skill 8 ==
          ...

        == FINAL LADDER ==
          SF skill  5: 18.5/20  (92%)  ≈ +436 elo
          SF skill  8: 12.5/20  (62%)  ≈ +89 elo
          ...

    We count W/D/L per skill from the per-game lines and pick the score/elo
    from the matching "Final" line. The "FINAL LADDER" block is also parsed
    as a cross-check / fallback when per-game lines are absent."""
    try:
        text = GAUNTLET_LOG_PATH.read_text()
    except Exception:
        return None

    skill_re   = re.compile(r"==\s*SF\s+Skill\s+(\d+)\s*==", re.IGNORECASE)
    game_re    = re.compile(r"\bGame\s+\d+/\d+\s+\(([WB])\)\s+([WLD])\b")
    final_re   = re.compile(r"Final\s+([\d.]+)\s*/\s*(\d+).*?([+-]?\d+)\s*elo", re.IGNORECASE)
    ladder_re  = re.compile(r"SF\s+skill\s+(\d+):\s*([\d.]+)\s*/\s*(\d+).*?([+-]?\d+)\s*elo", re.IGNORECASE)
    total_re   = re.compile(r"Total wall time:\s*([\d.]+)\s*min", re.IGNORECASE)

    results: dict[int, dict] = {}
    current = None
    wall_min = None

    for line in text.splitlines():
        m = skill_re.search(line)
        if m:
            current = int(m.group(1))
            results.setdefault(current, {"wins": 0, "draws": 0, "losses": 0,
                                        "score": 0.0, "total": 0,
                                        "elo": None})
            continue

        if current is not None:
            gm = game_re.search(line)
            if gm:
                bucket = results[current]
                outcome = gm.group(2)
                if   outcome == "W": bucket["wins"]   += 1
                elif outcome == "L": bucket["losses"] += 1
                elif outcome == "D": bucket["draws"]  += 1
                continue

            fm = final_re.search(line)
            if fm:
                bucket = results[current]
                bucket["score"] = float(fm.group(1))
                bucket["total"] = int(fm.group(2))
                bucket["elo"]   = int(fm.group(3))
                current = None  # leave block on Final
                continue

        # FINAL LADDER fallback / cross-check
        lm = ladder_re.search(line)
        if lm:
            skill = int(lm.group(1))
            b = results.setdefault(skill, {"wins": 0, "draws": 0, "losses": 0,
                                          "score": 0.0, "total": 0, "elo": None})
            b["score"] = float(lm.group(2))
            b["total"] = int(lm.group(3))
            b["elo"]   = int(lm.group(4))

        tm = total_re.search(line)
        if tm:
            wall_min = float(tm.group(1))

    if not results:
        return None

    total_score = sum(r["score"] for r in results.values())
    total_games = sum(r["total"] for r in results.values())
    return {
        "per_skill": [{"skill": k, **v} for k, v in sorted(results.items())],
        "final":     {"points": total_score, "out_of": total_games} if total_games else None,
        "wall_min":  wall_min,
        "mtime":     int(GAUNTLET_LOG_PATH.stat().st_mtime),
    }


def games_today_count(recent: list[dict], bot_name: str) -> dict:
    """From the existing recent_games list, count games started today (UTC)
    against other bots. Used as a proxy for the lichess 100/day bot-vs-bot
    quota (the exact API endpoint doesn't exist)."""
    from datetime import datetime, timezone
    now = datetime.now(timezone.utc)
    day_start_ms = int(now.replace(hour=0, minute=0, second=0, microsecond=0).timestamp() * 1000)
    bot_low = bot_name.lower()
    vs_bots = 0
    total   = 0
    for g in recent or []:
        ts = g.get("createdAt") or 0
        if ts < day_start_ms:
            continue
        total += 1
        for color in ("white", "black"):
            opp = g.get("players", {}).get(color, {}).get("user", {}) or {}
            if opp.get("name", "").lower() == bot_low:
                continue
            if opp.get("title") == "BOT":
                vs_bots += 1
                break
    return {"vs_bots_today": vs_bots, "total_today": total, "limit": 100}


def opening_repertoire(recent: list[dict], bot_name: str, top_n: int = 5) -> dict:
    """Top openings played as each color, from recent_games. Each entry:
    name, plays, wins, draws, losses."""
    bot_low = bot_name.lower()
    buckets = {"white": {}, "black": {}}
    for g in recent or []:
        white = (g.get("players", {}).get("white", {}).get("user", {}) or {}).get("name", "").lower()
        bot_color = "white" if white == bot_low else "black"
        opening = (g.get("opening") or {}).get("name")
        if not opening:
            continue
        name = opening.split(":")[0].strip()
        winner = g.get("winner")
        result = "draw" if not winner else ("win" if winner == bot_color else "loss")
        b = buckets[bot_color].setdefault(name, {"plays": 0, "win": 0, "draw": 0, "loss": 0})
        b["plays"] += 1
        b[result]  += 1

    def top(bucket: dict) -> list[dict]:
        items = [{"name": n, **v} for n, v in bucket.items()]
        items.sort(key=lambda x: -x["plays"])
        return items[:top_n]

    return {"white": top(buckets["white"]), "black": top(buckets["black"])}


def engine_stats(service: str) -> dict:
    """Parse the last 'info depth' line from the bot's journal."""
    out = _journal_cat(service)
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
        elif path == "/api/system":
            bot = self._bot_param()
            self._serve_json(system_stats(bot["service"]))
        elif path == "/api/move-history":
            self._serve_json(move_history(self._bot_param()["service"]))
        elif path == "/api/bench-result":
            self._serve_json(last_bench_result() or {})
        elif path == "/api/gauntlet-result":
            self._serve_json(last_gauntlet_result() or {})
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

        # Fetch wider than the 10-row display so games-today and the opening
        # repertoire have enough data to be useful. 50 covers a typical day of
        # bot play. Cached for 30 s — the dashboard polls every 5 s but the
        # recent-games list rarely changes that fast, and lichess will rate-
        # limit us into oblivion if we ask for 50 games every 5 s.
        recent = cached(
            f"lichess_recent:{name.lower()}",
            30.0,
            lambda: lichess_get(
                f"/api/games/user/{name.lower()}?max=50&moves=false&clocks=false&opening=true",
                token, ndjson=True,
            ) or [],
        )

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
            "system":            system_stats(service),
            "build_id":          latest_build_id(service),
            "games_today":       games_today_count(recent, name),
            "openings":          opening_repertoire(recent, name),
        }


if __name__ == "__main__":
    print(f"Dashboard running at http://0.0.0.0:{PORT}")
    ThreadingHTTPServer(("0.0.0.0", PORT), Handler).serve_forever()
