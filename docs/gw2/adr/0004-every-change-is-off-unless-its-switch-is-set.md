# Every change of behaviour is off unless its switch is set

Decided 2026-11-11. Status: accepted.

My three files replace the engine's for every process it starts, Steam and the launcher included. So
each change of behaviour sits behind an environment variable that is off unless set and is read
once. The `WINE_HEAT_*` ones act only in the process that `WINE_HEAT_EXE` names, which is
`Gw2-64.exe` unless set otherwise. Unset, each file behaves as athei's does. The app turns a switch
on by writing its variable into the game's compatdb rule.

## Consequences

- `.github/gw2/SWITCHES.md` lists every switch with its values and what unset means. Lint fails when
  a `getenv` of a `WINE_HEAT_*`, `WINE_MACDRV_*` or `WINE_WIN32U_*` name has no row, and when a
  row's name is read nowhere.
- A value a switch doesn't know counts as unset. The Mac driver and win32u log it.
- A build's `VERSION` has a `switches:` line that names the variables each file reads. The app greys
  a row whose switch the installed file doesn't have (#80).
- The heat switches' mode bits are one enum of bit numbers. Two tickets had each taken 0x10 for a
  new switch, and git merged them without a conflict.
- Which switches a player gets is the app's decision, not the fork's (ADR 0005).
- A dev build sets every switch from `dev-switches.conf`, whatever the app's rows say.
- A fix to athei's or Wine's own behaviour is the exception and has no switch (ADR 0006).

## Alternatives I rejected

- **Defaults built into the files, as 0.1.x's heat patch has them.** The record has the rule and
  doesn't argue this option. What it shows is where the files load: in every process of the bottle,
  where 0.1.x's patch also acted only in `Gw2-64.exe`.

## Evidence

- jdrly/gw2-on-mac #40 (the wake policy, off unless a `WINE_HEAT_*` variable is set and the image is
  `WINE_HEAT_EXE`), #45 (the enum), #80 (the `switches:` line).
- This fork's `.github/gw2/SWITCHES.md` (jdrly/wine PR #40).
- GW2 on Mac workspace `plans/night/2026-11-11-plan.md`, 2026-11-11 19:20 (every behaviour change
  has a switch and a rule for its default) and 2026-11-12 00:05, 01:12 and 09:52.
