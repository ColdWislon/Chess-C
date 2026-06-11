#!/usr/bin/env python3
"""Verify the ponder hold-loop: no bestmove until ponderhit/stop,
even when the ponder search self-completes (forced move)."""
import subprocess, sys, threading, time

ENGINE = sys.argv[1] if len(sys.argv) > 1 else "./chess-engine-c"
# 9 pieces (no syzygy/book path): black in check from Re8, only Kh7 is legal
FEN = "4R1k1/5pp1/7p/8/8/8/5PPP/6K1 b - - 0 1"

def run_case(name, release_cmd):
    p = subprocess.Popen([ENGINE], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                         stderr=subprocess.DEVNULL, text=True, bufsize=1)
    events = []
    ready = threading.Event()
    t0 = time.time()
    def reader():
        for line in p.stdout:
            events.append((time.time() - t0, line.rstrip()))
            if line.startswith("readyok"):
                ready.set()
    th = threading.Thread(target=reader, daemon=True)
    th.start()

    def send(cmd):
        p.stdin.write(cmd + "\n"); p.stdin.flush()

    send("uci"); send("ucinewgame")
    send(f"position fen {FEN}")
    send("isready")
    if not ready.wait(timeout=30):
        print(f"  FAIL [{name}]: engine never answered isready"); return False
    t_go = time.time() - t0
    send("go ponder wtime 60000 btime 60000")
    time.sleep(2.0)
    t_release = time.time() - t0
    send(release_cmd)
    time.sleep(1.0)
    send("quit")
    p.wait(timeout=10); th.join(timeout=5)

    bm = [(t, l) for t, l in events if l.startswith("bestmove")]
    ok = True
    if not bm:
        print(f"  FAIL [{name}]: no bestmove at all"); ok = False
    else:
        t, l = bm[0]
        if t < t_release - 0.05:
            print(f"  FAIL [{name}]: bestmove leaked {t - t_go:.2f}s after go ponder, "
                  f"before {release_cmd} at +{t_release - t_go:.2f}s: {l}")
            ok = False
        else:
            print(f"  PASS [{name}]: held {t_release - t_go:.2f}s, "
                  f"bestmove {t - t_release:+.3f}s after {release_cmd}: {l}")
    return ok

ok = run_case("ponderhit", "ponderhit")
ok &= run_case("stop", "stop")
sys.exit(0 if ok else 1)
