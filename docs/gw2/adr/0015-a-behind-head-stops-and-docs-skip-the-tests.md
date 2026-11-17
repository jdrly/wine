# A pull request whose head is behind stops, and one that changes only docs skips the tests

Decided 2026-11-17. Status: accepted.

ADR 0012 made a pull request run only the tests its change can reach, but any byte under
`tests/gw2` or `.github` set `TESTS=1` and ran everything. In the seven pull requests after it, six
touched one of the two, and none skipped a matrix block. Pull request #40 changed three Markdown
files under `.github/gw2`, `AGENTS.md`, and in `gw2.yml` only comment lines and the `lint` job. Its
run 43957907232 still spent 942 s in the matrix and 24.5 minutes in the tests job, 31.6 minutes in
all, for files none of its lines could change. The second problem is jdrly/gw2-on-mac#149. A pull
request's run takes the workflow and `.github/gw2` from GitHub's merge commit, and builds and tests
the head. When the head is behind `gw2-on-mac`, new workflow steps run against old tests. Run
43370004985 went red after 13 minutes that way, at a step for `leftover.exe` that the head didn't
have. Even when such a run is green, it's wasted: the merge has another tree than the head, so ADR
0013's guard finds no green run of that tree and the push runs every test again.

So I made two changes to how a pull request's run decides what to do:

- `TESTS` counts only what can reach a test: a file under `tests/gw2` or `.github` that isn't
  Markdown, and of `gw2.yml` what is left without its comment lines and its `lint` and `publish`
  jobs.
- The build job's step "The head has gw2-on-mac's tip" compares the merge commit's tree with the
  head's. If they differ, the run fails in its first minute and asks for a rebase.

## Consequences

- A pull request that changes only Markdown, comments or the `lint` and `publish` jobs gets
  `NTDLL=0 WINEMAC=0 WIN32U=0 TESTS=0`, and its tests job runs the smoke test. #40's run would
  have taken about 9 minutes, not 31.6. A pull request that changes code or tests runs what it ran
  before.
- A comment line is one whose first character after the indentation is `#`. That holds because the
  workflow has no here-document. A here-document, or a multi-line string with a line that starts
  with `#`, would make the rule skip a change that matters, so the rule has to change with it.
- None of the Markdown files under `tests/gw2` or `.github` is read by the build or the tests.
  `SWITCHES.md` is read by the lint job only. A Markdown file that a test reads would need its own
  exception.
- A new job that runs no test has to be added to the `awk` pattern. Until it is, a change to it
  runs everything, which costs minutes and misses nothing.
- A branch that falls behind `gw2-on-mac` while it waits for its push has to be rebased first. A
  push after the rebase is a second push and needs its own go. The run that stops costs about a
  minute, not 13 to 31.
- A re-run of an old run keeps its old merge commit, so the step passes there even if
  `gw2-on-mac` has moved since. ADR 0013's guard still catches that at the push.
- "A process left behind" and "rundll32 and an addon's d3d11.dll" still run on a pull request that
  owes no test, since their condition is `FULL` alone.
- This changes ADR 0012's line "a change under `tests/gw2` or `.github` sets `TESTS`", and settles
  #149, which ADR 0013 lists as open, as "rebase first".

## Alternatives I rejected

- **The merge commit's tests against the head's files.** A test that came to `gw2-on-mac` with a
  fix would run against a head without the fix, and fail.
- **A warning instead of a failure for a head that is behind.** The run would go on testing a
  mixed tree, and the push would run every test again anyway. It is a one-line change back
  (`::warning::` and no `exit 1`) if I ever want it.
- **A finer rule for single test programs.** It needs a table of which program runs in which step,
  and a wrong row skips a test without a sound.

## Evidence

- jdrly/gw2-on-mac #228 and #149, jdrly/wine PR #40 and its run 43957907232, run 43370004985.
- GW2 on Mac workspace: `advisor-plans/2026-11-17-ci/REVIEW.md` (finding 2 and "The head against
  the merge ref") and `advisor-plans/2026-11-17-ci/007-wine-ci-selection-and-behind-head.md`.
- The rule applied to the fork's history: `TESTS=0` for #38 and #40, `TESTS=1` for #33, #35, #36,
  #37 and #41.
