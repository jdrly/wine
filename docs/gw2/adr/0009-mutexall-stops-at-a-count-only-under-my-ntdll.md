# `mutexall.exe` stops at a count only under my `ntdll.so`

Decided 2026-11-16. Status: accepted.

`mutexall.exe` tests my fix to the wait-all's mutex rollback. It runs under the engine's own
`ntdll.so`, where it has to fail on athei's bug, and under mine, where it has to pass. Each run took
a fixed 20 seconds. To save runner time the plan bounded every run at 20,000 wait-alls. The first
run showed why that is wrong for one side: two of the engine's three runs went past 20,000 without
losing the race, so the engine's side no longer proved the bug. The two sides ask different
questions. The engine's runs have to show a bug, and losing a race takes time. Mine show that the
fix holds, which a count does. So only my side is counted.

## Consequences

- `mutexall.exe` stops after `GW2_TEST_WAIT_ALLS` wait-alls when that is set, and otherwise runs its
  20 seconds. `matrix.zsh` sets 20,000 for `mutexall-mine` in block 3, and never for a run under
  the engine's file.
- Under the engine the bug took 742 to 51,542 wait-alls on that day's runners, not the "17 to 749"
  the plan had from one earlier run.
- Under mine a run takes about 2 s for 20,000 wait-alls, where it took 20 s.
- Block 2's `mutexall` runs under my file still took 20 s each. Since jdrly/wine PR #41
  (2026-11-17, jdrly/gw2-on-mac #225) block 2 stops them at 20,000 wait-alls too.
- In the same change the matrix dropped three leftover `unreg.exe` runs with
  `WINE_HEAT_MSYNC_UNREG=1`, a variable my file no longer reads, and `collect` frees each block's
  prefixes once its runs have ended. A runner had run out of disk the same day.
- Since jdrly/gw2-on-mac #324 (2026-11-19) the engine's `mutexall` runs, and `putback`'s, run
  only when `selection.env`'s `ENGINE_BUGS` is 1: a new `MACH9_VERSION`, or a change to
  `dlls/ntdll/unix/msync.c`, `mutexall.c`, `putback.c`, `matrix.zsh` or `run.zsh`, against the fork
  point or, on a push, the push's parent. They prove that the test still finds athei's bug. A new
  engine or a changed test can change that, and so can a change of mine to `msync.c`, where both
  bugs are. The two scripts decide what a run under the engine means. Each run also has to lose a
  race, and in run 43923114537 the engine lost only 1 of 3. Their failures print `expected`, not
  `FAIL`, and a `FAIL` that run.zsh prints in such a run, for the prefix boot or processes left
  behind, fails the lane.
- Since jdrly/gw2-on-mac #323 (2026-11-20) a push compares `ENGINE_BUGS`'s files with the
  branch's tip before the push (`github.event.before`), not with the pushed commit's parent, so a
  push of several commits counts all of them. Without that tip in the clone the flag is 1.
- Since #323 block 1 runs `wakes.exe` once in each mode, the run with the minute of producing. The
  second and third runs of each mode went, with their 20 seconds each: about 115 s of matrix-a,
  the lane that set the run's length. The minute this record keeps is the first run's, and it
  stays. Block 2's runs of `wakes.exe` keep their 20 seconds.

## Alternatives I rejected

- **A count on both sides.** It makes the engine's runs pass when they should fail, as the first run
  showed.
- **Keeping 20 s on both sides.** The run under mine proves nothing more after 20,000 wait-alls.
- **Cutting `wakes.exe`'s minute the same way.** That minute is deliberate. A stall that needs more
  than 20 seconds of producing on one semaphore would escape every run otherwise.

## Evidence

- jdrly/gw2-on-mac #220, jdrly/wine PR #37 (`15172b2c0a8`), runs 43923114537 (red) and 43929333559.
- GW2 on Mac workspace: `advisor-plans/2026-11-16-forks/vetted-B-wine-ci-docs.md` ("TEST-03,
  corrected"), `plans/028-wine-tests-counted-mutexall-drop-leftover-unreg.md` and `plans/README.md`
  ("Follow-ups found while executing", from 028); `plans/night/2026-11-11-plan.md`, 2026-11-16 18:52
  to 20:01.
