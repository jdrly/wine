# The tests run as three lanes, and block 7 goes with the window steps

Decided 2026-11-17. Status: accepted.

Changed on 2026-11-27 by ADR 0024: block 7 is gone, so the window lane runs the window steps alone.

Until now a pull request's run was one line of work: the build, then one tests job that ran the
window steps and then the matrix. In run 44001124649 (pull request #42) the build took 4.0 minutes
and the tests job 20.6. Inside that job the window steps took 330 s and `matrix.zsh` 834 s, and
neither reads anything the other makes. The matrix can't run more side by side inside one job:
the runner has 3 CPUs, and its blocks already run three to five programs at once.

So the tests job is now a matrix of lanes, one job each, started together when the build is done.
My account runs at most 5 macOS jobs at once, and my four repositories share them, so a Wine run
gets three lanes, not the four plan 008 drew. The build job says which lanes a run owes, as a job
output, and builds the test programs once for all of them.

I cut the lanes from the per-block seconds of the two runs after 006's shorter dump periods,
44001124649 and 43995170118:

| Lane | What runs | Seconds, 44001124649 | Seconds, 43995170118 |
|---|---|---|---|
| `window` | every test step but the matrix, then block 7 | 330 + 45 = 374 | 320 + 40 = 359 |
| `matrix-a` | blocks 1, 3b and 4 | 251 + 40 + 104 = 395 | 243 + 47 + 103 = 393 |
| `matrix-b` | blocks 1b, 2, 3, 5, 6 and 8 | 62 + 128 + 53 + 51 + 53 + 48 = 395 | 61 + 139 + 54 + 46 + 49 + 49 = 397 |
| `smoke` | the smoke test, when nothing else is owed | | |

A block's seconds run from the end of the block before it to its last `== ` line. Each lane adds
about 50 s of setup.

Block 7 is the one I moved into the window lane. It's the only block a pull request that leaves my
ntdll.so alone can owe, so `matrix-a` and `matrix-b` now exist only when `RUN_NTDLL` is 1, and a
pull request that changes only my win32u.so or my winemac.so needs one runner. Block 1 (about
250 s) can't shrink, since commit `e3334049c55` decided its producer's minute. Block 6 has to go
with block 5, whose clone of my tree it runs under, and `matrix.zsh` refuses 6 without 5.

## Consequences

- A pull request that reaches my ntdll.so runs three macOS jobs after the build, each of about
  7.5 minutes. From 44001124649's job times, its wall clock goes from about 25 minutes to about
  12.5, and its macOS minutes go up by about 2, from the two extra setups.
- A pull request that reaches only my winemac.so or my win32u.so gets the window lane alone. One
  that owes no test, and a push whose tree a green pull request run tested, get the smoke lane
  alone.
- "A process left behind" and "rundll32 and an addon's d3d11.dll" are window steps now, so they no
  longer run on a pull request that owes no test. That changes ADR 0015's consequence that they
  still do. They test `run.zsh` and the app's compatdb rule, which such a pull request can't have
  changed.
- `GW2_MATRIX_BLOCKS` takes the blocks' own names, and `matrix.zsh` exits 2 on a name that isn't a
  block. A new block has to go into one lane's list in "Run them", or no lane runs it, and nothing
  goes red for it.
- A new test step names its lane in its `if:`, besides its `RUN_*`.
- The lanes are named in two places that must agree: the `lanes+=` lines of the build job's "Which
  tests this run owes" and the `case $LANE` of "Run them".
- The seconds are from two runs. If one lane runs much longer than the others on the runner, "Run
  them" is the one place to move a block.
- Since jdrly/gw2-on-mac #323 (2026-11-20) block 1 runs once in each mode, about 115 s shorter, so
  the sentence above that it can't shrink no longer holds: ADR 0009 keeps the first run's minute,
  and the other two runs went. Blocks 4, 5 and 6 run only with `HEAT` (ADR 0012). From
  run 44623457706's blocks I expect about 325 s of matrix-a's steps and 345 s of matrix-b's when
  every block runs, so the lanes stay as they are. The pull request's run has the measured
  seconds.

## Alternatives I rejected

- **Four lanes, the window steps alone and three matrix lanes.** About 1 minute less wall clock,
  but four macOS jobs at once leave one of my 5 for the other three repositories.
- **The window steps alone, and the whole matrix in two lanes.** The best split, blocks 1, 2 and 3b
  against the rest, comes to 419 and 416 s in 44001124649 and 428 and 402 s in 43995170118: its
  longest lane is 25 to 30 s longer than mine, and a win32u-only pull request would need a matrix
  lane as well.
- **Plan 008's own three-lane variant, `matrix-c`'s blocks moved into `matrix-b`.** That lane would
  be blocks 1b, 2, 3, 4, 5, 6, 7 and 8, about 544 s, 2.5 minutes longer than any lane here.
- **More programs side by side in one job.** The runner has 3 CPUs. In run 43957907232 evpool's
  storm took 3.1 to 4.3 s in a block of three chains and 7.2 to 7.7 s beside two busy chains.

## Evidence

- jdrly/gw2-on-mac #229. Runs 44001124649 and 43995170118, their tests jobs' logs.
- GW2 on Mac workspace: `advisor-plans/2026-11-17-ci/008-wine-ci-test-lanes.md` and
  `advisor-plans/2026-11-17-ci/REVIEW.md`.
