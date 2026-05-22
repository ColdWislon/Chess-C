CC      = gcc
CFLAGS  = -std=c11 -Wall -Wextra -Isrc -Iexternal -pthread
LDFLAGS = -pthread -lm

SRC = src/board.c src/tt.c src/eval.c src/search.c \
      src/opening.c src/perft.c src/chat.c src/bench.c \
      src/syzygy.c src/texel.c src/uci.c src/main.c

# Fathom Syzygy probe (external/tbprobe.c) — single TU, includes tbchess.c.
# Compiled separately with -w because Fathom's source emits a few -Wunused
# warnings on the Pi 4 toolchain that aren't ours to fix.
FATHOM_SRC = external/tbprobe.c
FATHOM_OBJ = external/tbprobe.o

$(FATHOM_OBJ): $(FATHOM_SRC) external/tbprobe.h external/tbconfig.h external/tbchess.c
	$(CC) $(CFLAGS) -O3 -w -DNDEBUG -c $(FATHOM_SRC) -o $@

# Build-time identity: short git SHA + "-dirty" if the working tree is dirty.
# Regenerated on every build (src/build_id.h is gitignored). Surfaces to the
# dashboard via `info string BUILD <id>` at startup.
GIT_SHA   := $(shell git rev-parse --short HEAD 2>/dev/null || echo unknown)
GIT_DIRTY := $(shell git diff --quiet 2>/dev/null || echo -dirty)
BUILD_ID  := $(GIT_SHA)$(GIT_DIRTY)

src/build_id.h: FORCE
	@echo '#pragma once' > $@.tmp
	@echo '#define BUILD_GIT_SHA "$(BUILD_ID)"' >> $@.tmp
	@cmp -s $@.tmp $@ 2>/dev/null || mv $@.tmp $@
	@rm -f $@.tmp

FORCE:

release: src/build_id.h $(SRC) $(FATHOM_OBJ)
	$(CC) $(CFLAGS) -O3 -march=native $(SRC) $(FATHOM_OBJ) -o chess-engine-c $(LDFLAGS)

debug: src/build_id.h $(SRC) $(FATHOM_OBJ)
	$(CC) $(CFLAGS) -O0 -g $(SRC) $(FATHOM_OBJ) -o chess-engine-c-dbg $(LDFLAGS)

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
	rm -f chess-engine-c chess-engine-c-dbg chess-engine-c.baseline src/build_id.h $(FATHOM_OBJ)

# ── Opening book ──────────────────────────────────────────────────
# book.bin is gitignored (re-downloadable, ~5 MB). `make book` fetches it
# from the upstream Donna GM-2600 book if absent. main.c looks for the
# file at /home/bertrand/chess-c/book.bin then falls back to ./book.bin.
BOOK_URL = https://github.com/michaeldv/donna_opening_books/raw/master/gm2600.bin

book: book.bin
book.bin:
	@echo "downloading opening book from $(BOOK_URL)…"
	@wget --quiet --show-progress -O book.bin.tmp "$(BOOK_URL)"
	@mv book.bin.tmp book.bin
	@echo "book.bin: $$(du -h book.bin | cut -f1)"

# ── Dashboard build-info ──────────────────────────────────────────
# Regenerate build-info.json from `git log` so the dashboard's
# "Engine build" card reflects what's actually committed. Safe to
# rerun any time; it just overwrites the file.
build-info:
	@python3 tools/gen_build_info.py

# ── Texel-tuning corpus ───────────────────────────────────────────
# Ethereal's quiet-labeled.epd: ~750k positions, each labeled with the game
# outcome, all already quiesced. Standard texel-tuning corpus. ~50 MB raw.
# `texel-snapshot.txt` is produced by the tuner each pass (gitignored).
CORPUS_URL = https://raw.githubusercontent.com/AndyGrant/Ethereal/master/resources/quiet-labeled.epd

corpus: quiet-labeled.epd
quiet-labeled.epd:
	@echo "downloading quiet-labeled corpus from $(CORPUS_URL)…"
	@wget --quiet --show-progress -O quiet-labeled.epd.tmp "$(CORPUS_URL)"
	@mv quiet-labeled.epd.tmp quiet-labeled.epd
	@echo "quiet-labeled.epd: $$(wc -l < quiet-labeled.epd) positions, $$(du -h quiet-labeled.epd | cut -f1)"

.PHONY: release debug test clean bench bench-timed bench-baseline bench-compare bench-compare-timed book build-info corpus FORCE
