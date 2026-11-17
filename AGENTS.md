# jdrly/wine agent guidance

This fork belongs to GW2 on Mac, my app for Guild Wars 2 on Apple Silicon. Its branch `gw2-on-mac`
(the default) carries my changes to three files of athei's arm64 Wine, the engine of Mach9:
`ntdll.so`, `winemac.so` and `win32u.so`. My app drops them into Mach9's engine in place of its
own. The branch starts at athei/wine `53def89293a` (wine-11.21-37), the commit Mach9 0.3.1's
engine was built from, which `.github/workflows/gw2.yml` names as `BASE_COMMIT`.

- Nothing goes to athei's repositories or to WineHQ: no pull request, issue, comment or mail from
  this fork's work. The changes here are AI-written. A finding upstream should hear about goes to
  me, and I decide.
- Every AI-written commit says so in its body.
- Issues are disabled here. The tracker is jdrly/gw2-on-mac, with the labels `area:wine` (ntdll,
  wineserver, the heat counters and switches), `area:mac-driver` (the Mac driver and win32u) and
  `area:build` (the build and CI).
- Work branches off `gw2-on-mac` as `<type>/<issue>-<slug>` (`feat`, `fix`, `refactor`, `docs`)
  and comes back through a squashed pull request. Nobody pushes to `gw2-on-mac` directly. Commit
  subjects read `Area: What it does`, and the body names the issue as `Refs jdrly/gw2-on-mac#<issue>`.

## Builds and tests run on GitHub's runner only

A pull request's run costs about 28 macOS minutes, and GitHub disabled this fork's workflows once
for wasted compute. So: one run at a time, one push per pull request, each push after my go, and
never a push to find out whether something builds. Locally you can run `actionlint`, `zsh -n` on
the scripts, `python3 -I -m py_compile` on the Python and `python3 -I .github/gw2/heatreq.py check`.
Don't run `tests/gw2/run.zsh` anywhere but the runner. At its end it kills every process it takes
for the run's leftovers, and on a shared machine that can be another session's.

## Where my code goes

My code lives in files of my own: `dlls/ntdll/unix/heat*.{c,h}`, `dlls/winemac.drv/cocoa_calls.h`,
`gcmouse_scale.h`, `surface_frame.h`, `tests/gw2/`, `.github/gw2/`, `docs/gw2/` and the workflow.
An upstream file gets a narrow hook, a call into a function of mine, with a comment that names the
issue.
Every line of mine in an upstream file is a possible conflict at the next rebase.
`.github/gw2/UPSTREAM.md` lists them, and a change that adds or removes one changes its row.

Every change in behaviour sits behind a switch, an environment variable that is off unless set and
read once. `.github/gw2/SWITCHES.md` lists them, and Lint fails when it misses one the sources read.
A fix to athei's or Wine's code without a switch goes into `UPSTREAM.md`'s second table.

## Read on

- the header of `.github/workflows/gw2.yml`: what each job does and what the two gates mean
- the header of `tests/gw2/matrix.zsh`: the test matrix
- `.github/gw2/MOVING.md`: moving to another Mach9 release
- the GW2 on Mac workspace's `AGENTS.md` and `docs/research/`: the project's rules and measurements

Comments, commit messages and documents here are in my first person, in plain words.
