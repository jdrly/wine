# The heat dump is written to a temp file and renamed over the log

Decided 2026-11-16. Status: accepted.

Changed on 2026-11-27 by ADR 0024: the clip step and block 7 are gone.

With `WINE_HEAT_COUNT=1` a dumper thread writes the counters to `heat-<pid>.log` every 5 s. It
opened the log with `O_TRUNC` and then wrote, so a read in between found an empty or short file.
That showed when the tests got a shorter dump period: `wakes.exe` read the log between the two calls
and found no row for its thread (run 43943879791). I had two ways out, and chose the second: the
dumper writes `heat-<pid>.log.tmp` in the same folder and renames it over the log. The gap is a real
defect and not only the tests' problem. It is there at the game's 5 s too, and I press Collect Logs
and run the session report while the game runs.

## Consequences

- A reader sees the last whole dump or the new one, never a cut one.
- If the write, the close or the rename fails, the dumper removes the temp file and keeps counting.
  The last whole log stays.
- `heat_dump()` runs only under `heat_dump_mutex`, so the dumper and the exit dump can't race on the
  temp name.
- Nothing that reads the log matches the `.tmp` by its pattern. The app's `collect-logs.sh` copies
  the whole folder, so a `.tmp` that exists at that moment comes along as an extra file. It is never
  taken as the counters.
- The dump period itself became a switch for the tests only, `WINE_HEAT_DUMP_MS` (100 to 60000,
  default 5000). The matrix sets 500 for the four counter tests. Since jdrly/wine PR #41
  (2026-11-17) the clip step sets 500 too, and the requests runs of block 6 and every run of
  block 7 set 1000. My app never sets it. It is the
  fork's twentieth switch, and my own vetted note had preferred reading the dump a process writes at
  exit. Planning showed that way rewrites eight test programs that only the runner can compile.

## Alternatives I rejected

- **Option (a).** The log says I chose (b) over it and doesn't say what (a) was. The pull request
  points at a second read in `wakes.exe`, as `spin.exe` has. That would fix the test and leave the
  short read for Collect Logs and the session report.

## Evidence

- jdrly/gw2-on-mac #222, jdrly/wine PR #39 (`d839d05b1f6`), runs 43943879791 (red) and 43950293002.
- GW2 on Mac workspace: `plans/night/2026-11-11-plan.md`, 2026-11-16 21:30 ("Chose (b)") and 22:04;
  `advisor-plans/2026-11-16-forks/plans/029-wine-tests-short-dump-period.md` and `plans/README.md`
  ("Decisions the plans made that I may want to overrule", 029).
