# A pull request that changes only docs skips the tests

Decided 2026-11-17. Status: accepted, changed on 2026-11-20 by jdrly/gw2-on-mac #323 (the last
consequence).

Changed on 2026-11-27 by ADR 0024: `selection.env` has no `WIN32U`, and `TESTS=1` runs the tests
of my two files.

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
- Since jdrly/gw2-on-mac #323 (2026-11-20) a pull request that changes only Markdown files doesn't
  build either, which saves the build job's 5 macOS minutes and the smoke lane. The `changes` job,
  on Linux, reads GitHub's compare of the base with the head: when every file, and a renamed
  file's old name, ends in `.md`, the build job is skipped, and the tests and Publish with it.
  Lint still runs, since it reads `SWITCHES.md`. A failed lookup or a list GitHub cut at 300
  files builds. A push always builds.
- In the same change the rest of this record went. `TESTS` no longer reads the workflow file: a
  change to any file under `tests/gw2` or `.github`, a comment line or a Markdown file beside code
  included, runs the tests of all three files, blocks 4 to 6 as ADR 0012's `HEAT` says. Review of
  #323 found that a key with an inline comment can hide a section from such a reading, and I
  would rather pay minutes than miss a test. A pull request that changes only Markdown builds and
  tests nothing by the line above, and runs Lint.

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
