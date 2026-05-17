CC      = gcc
CFLAGS  = -std=c11 -Wall -Wextra -Isrc -pthread
LDFLAGS = -pthread

SRC = src/board.c src/tt.c src/eval.c src/search.c \
      src/opening.c src/perft.c src/chat.c src/bench.c src/uci.c src/main.c

release: $(SRC)
	$(CC) $(CFLAGS) -O3 -march=native $(SRC) -o chess-engine-c $(LDFLAGS)

debug: $(SRC)
	$(CC) $(CFLAGS) -O0 -g $(SRC) -o chess-engine-c-dbg $(LDFLAGS)

# src/poly_keys.h is checked in (the canonical 781 Polyglot constants).
# Regenerate with: python3 tools/gen_poly_keys.py > src/poly_keys.h

test: release
	@echo "=== Perft depth 4 (expect 197281) ==="
	@printf 'position startpos\nperft 4\nquit\n' | ./chess-engine-c
	@echo "=== Perft depth 5 (expect 4865609) ==="
	@printf 'position startpos\nperft 5\nquit\n' | ./chess-engine-c

# ── Bench infrastructure ──────────────────────────────────────────
# Deterministic mode (perf / pure-perf regressions):
#   `make bench`           — run the bench position set on current build
#   `make bench-baseline`  — build current code, stash binary as the "before"
#   `make bench-compare`   — build current code, run baseline + current, print delta
#   Override depth with: make bench BENCH_DEPTH=10
#
# Timed mode (changes affecting time management / stopping behavior):
#   `make bench-timed`           — runs each position with BENCH_MS time budget
#   `make bench-compare-timed`   — interleaved A/B on depth-reached + utilization
#   Override budget with: make bench-timed BENCH_MS=2000
BENCH_DEPTH ?= 8
BENCH_MS    ?= 1000

bench: release
	@printf 'bench $(BENCH_DEPTH)\nquit\n' | ./chess-engine-c

bench-timed: release
	@printf 'bench_timed $(BENCH_MS)\nquit\n' | ./chess-engine-c

bench-baseline: release
	@cp chess-engine-c chess-engine-c.baseline
	@echo "Baseline saved → chess-engine-c.baseline"
	@echo "Now make your changes, then run: make bench-compare (or bench-compare-timed)"

bench-compare: release
	@bash tools/bench_compare.sh $(BENCH_DEPTH)

bench-compare-timed: release
	@bash tools/bench_compare_timed.sh $(BENCH_MS)

clean:
	rm -f chess-engine-c chess-engine-c-dbg chess-engine-c.baseline

.PHONY: release debug test clean bench bench-timed bench-baseline bench-compare bench-compare-timed
