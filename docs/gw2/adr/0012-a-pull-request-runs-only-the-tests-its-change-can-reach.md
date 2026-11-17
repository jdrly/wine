# A pull request runs only the tests its change can reach

Decided 2026-11-16. Status: accepted.

Every pull request ran the whole tests job, about 22 of its 28 runner minutes, 17 of them in the
ntdll matrix, whatever it changed. One that changed only the Mac driver spent 1,045 s in the matrix,
and one that changed only a CI script 1,009 s. GitHub has disabled this fork's workflows once for
wasted compute. So a pull request now runs the tests of the files it can have changed.

## Consequences

- The build job's Compare step writes `selection.env` with `NTDLL`, `WINEMAC`, `WIN32U` and `TESTS`.
  A file's flag is 1 when `compare.py fork` lists a moved, changed, new or gone function of it, or
  when the diff touches a path that reaches it: `dlls/ntdll`, `server` and `tools` for `ntdll.so`,
  `dlls/winemac.drv` or `dlls/win32u` for the other two, `include` for all three. The paths are
  there because a change to data alone shows no changed function.
- A change under `tests/gw2` or `.github` sets `TESTS` and runs everything. Narrowed on 2026-11-17
  by ADR 0015: only a file there that can reach a test counts.
- The matrix runs blocks 1 to 6 and 8 only when `ntdll.so` is selected, and block 7 when `ntdll.so`
  or `win32u.so` is. Smoke runs whenever the ntdll blocks don't. Changed on 2026-11-17 by ADR 0016:
  block 7 runs in the window lane, and smoke runs only when no other lane is owed.
- A push has no fork point. What it runs is ADR 0013's question.
- A test can be skipped for a change that reaches its file through a path the lists don't name. The
  fork report still catches any changed function, so only a data-only change through such a path
  slips by.
- A push can skip the matrix because a pull request ran on the same tree, and that pull request may
  have skipped the ntdll blocks because its `ntdll.so` matched its fork point. The chain trusts that
  the fork point was tested with the same engine, toolchain and tests.

## Alternatives I rejected

- **Running everything on every pull request.** About 28 macOS minutes each, most of them for files
  the change can't reach.
- **Stopping the matrix at its first red block.** In the 8 runs that went red there, the first red
  block was at or near the end in 5. With one push per pull request, a red run that shows every
  failure is worth more than the minutes.
- **Caching the baseline build between runs (WINE-11).** It takes 22 to 40 s, and a cached base
  breaks the compare's "one run, one toolchain".

## Evidence

- jdrly/gw2-on-mac #217, jdrly/wine PR #34 (`02e36e1b0d4`), run 43882227376.
- GW2 on Mac workspace: `advisor-plans/2026-11-16-forks/vetted-B-wine-ci-docs.md` ("TEST-02,
  corrected: take the selection, drop the early stop"),
  `plans/027-wine-ci-run-only-reachable-tests.md` and `plans/README.md` ("Not planned, and why");
  `plans/night/2026-11-11-plan.md`, 2026-11-16 15:05 to 15:43.
