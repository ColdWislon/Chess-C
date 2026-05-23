---
description: Run the autonomous engine improvement loop — propose → review → gauntlet → push to feature branch. Reverts src/ on any failure. Production never touched.
allowed-tools: Agent, Bash, Read, Edit, Write
---

# /improve — autonomous chess-c improvement loop

You are orchestrating an autonomous engine improvement loop. Run these stages in sequence, handing off between four specialist agents. **The entire pipeline must leave `src/` clean on exit — revert any applied diff if any stage fails or refuses.**

# Hard preconditions

Before starting:

1. **Working tree must be clean on main**. Refuse and stop if `git status --short` shows uncommitted changes — running the loop would conflict with the user's work-in-progress.
2. **We must be on `main`**. `git checkout main` if needed, but refuse if branch switch is impossible (uncommitted changes, etc.).
3. **rpiBot73 should not be in a critical game.** The improver runs PROF (1 core, ~5 s) and the gauntler will require idle. If currently in a game, warn the user — they can choose to wait or abort.

# Stages

## Stage 1 — Propose

Spawn `engine-improver` with a brief like:

> Analyze chess-c per your system prompt. Propose ONE small targeted improvement as a unified diff. Read CLAUDE.md first.

Expect output: a `## Proposal:` block with title + diff, **or** `## No proposal` (in which case stop here and report to the user).

## Stage 2 — Apply

If a proposal returned, apply the diff to `src/`:

```bash
echo "<diff text>" > /tmp/improve-proposal.patch
cd /home/bertrand/chess-c
git apply --check /tmp/improve-proposal.patch || { echo "diff does not apply cleanly"; exit 1; }
git apply /tmp/improve-proposal.patch
```

If the diff fails to apply (the improver hallucinated context lines, or main moved): output a failure summary, revert any partial application via `git checkout src/`, stop.

## Stage 3 — Review

Spawn `engine-reviewer` with:

> A diff has been applied to src/. Review per your system prompt. The proposal title and rationale are: <paste improver's Proposal block>.

Expect output: `## Verdict: APPROVE` or `## Verdict: REJECT`.

- **REJECT** → revert (`git checkout src/`), report the rejection reason + the original proposal, stop.
- **APPROVE** → continue to stage 4.

## Stage 4 — Gauntlet

Spawn `engine-gauntler` with:

> The reviewer approved the change currently in src/. Run a 40-game Pi self-play gauntlet per your system prompt. Title for context: <proposal title>.

Expect output: `## Gauntlet result` block with Elo / LOS / W-D-L and a `Recommendation: DEPLOY|DO NOT DEPLOY`.

- **Gauntlet refused** (bot in game, etc.) → revert src/, report, stop.
- **DO NOT DEPLOY** → revert src/, report the gauntlet numbers (still informative), stop.
- **DEPLOY** → continue to stage 5.

## Stage 5 — Deploy to feature branch

Spawn `engine-deployer` with:

> Reviewer approved, gauntler recommended DEPLOY. The change is applied to src/. The proposal was: <paste full Proposal block>. The gauntlet result was: <paste Gauntlet result block>. Push per your system prompt.

Expect output: `## Deployed to feature branch` or `## Deploy refused`.

After deployer runs (success or refuse), it should leave us back on `main` with `src/` clean. Verify:

```bash
git checkout main 2>/dev/null
git status --short    # should be empty (or only untracked binaries)
```

If src/ is still modified (deployer crashed mid-flight), `git checkout src/` to revert.

## Final report

Output a single end-to-end summary in this format:

```
# /improve run summary

**Proposal:** <title>
**Reviewer:** APPROVE | REJECT (<reason if reject>)
**Gauntlet:** Elo <+/-N> ± <err>, LOS <N>%  (40 games Pi)
**Outcome:** <pushed to auto-improve/<branch> | reverted (reason)>

<one paragraph: what to do next — usually "git fetch on WSL and run wsl-ab-gauntlet.sh against the branch, then merge if it confirms">
```

# Safety invariants (apply at all times)

- **Never push to main from this loop.** Only `auto-improve/<topic>` branches.
- **Never deploy to lichess.** The `safe-restart-bot.sh` is the user's responsibility — we only stage branches for review.
- **Always restore src/ to clean state on exit**, success or failure.
- **Always restart `lichess-bot-c` if you stopped it.** The gauntler handles this internally but verify after stage 4.
- **Halt and report** if any stage takes an action you can't reverse. The user can always start a new `/improve` run.

# When the user asks to "run /improve again" or chains runs

Each invocation is independent. Do not assume state from previous runs. If a previous loop left a branch on origin, the improver should still propose something new (the deployer's branch was for the user to evaluate independently).
