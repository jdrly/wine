# A pull request that changes only docs skips the tests

Decided 2026-11-17. Status: accepted.

ADR 0012 made a pull request run only the tests its change can reach, but any byte under
`tests/gw2` or `.github` set `TESTS=1` and ran everything. In the seven pull requests after it, six
touched one of the two, and none skipped a matrix block. Pull request #40 changed three Markdown
files under `.github/gw2`, `AGENTS.md`, and in `gw2.yml` only comment lines and the `lint` job. Its
run 43957907232 still spent 942 s in the matrix and 24.5 minutes in the tests job, 31.6 minutes in
all, for files none of its lines could change.

So `TESTS` now counts only what can reach a test: a file under `tests/gw2` or `.github` that isn't
Markdown, and of `gw2.yml` what is left without its comment lines and its `lint` and `publish`
jobs. The same pull request brought the behind-head stop, a second decision with its own record in
ADR 0017.

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
- "A process left behind" and "rundll32 and an addon's d3d11.dll" still run on a pull request that
  owes no test, since their condition is `FULL` alone. Changed the same day by ADR 0016: they are
  window steps now, and such a pull request no longer runs them.
- This changes ADR 0012's line "a change under `tests/gw2` or `.github` sets `TESTS`".

## Alternatives I rejected

- **A finer rule for single test programs.** It needs a table of which program runs in which step,
  and a wrong row skips a test without a sound.

## Evidence

- jdrly/gw2-on-mac #228, jdrly/wine PR #40 and its run 43957907232, jdrly/wine PR #42
  (`c2b78d49534`).
- GW2 on Mac workspace: `advisor-plans/2026-11-17-ci/REVIEW.md`, finding 2, and
  `advisor-plans/2026-11-17-ci/007-wine-ci-selection-and-behind-head.md`.
- The rule applied to the fork's history: `TESTS=0` for #38 and #40, `TESTS=1` for #33, #35, #36,
  #37 and #41.
