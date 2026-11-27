# My win32u.so and the Mac driver's call switches are dropped

Decided 2026-11-27. Status: accepted.

Two topics of the series never became a default and have no result in play: `win32u`, my build of
`win32u.so` with `WINE_WIN32U_SETCURSOR` and `WINE_WIN32U_PEEK_YIELD` (#61), and `macdrv-calls`,
the four switches `WINE_MACDRV_VIEW`, `_CLIP`, `_CALL` and `_DRAIN` (#62). GW2's own numbers say
what they could save:

- With `SETCURSOR` off, the game thread sends 10 to 12.5 cursor requests a second, and 92 to 95%
  of them repeat the cursor. At 8.6 µs a request that is 0.1 ms a second. The 0.2.1 trace has no
  sample in `NtUserSetCursor` in 20.5 s. The yields `PEEK_YIELD` would leave out are in 2 of the
  game thread's 13,629 samples.
- With the four driver switches off, the view update GW2 causes each frame is in 12 of those
  samples, 0.6 ms a second. `macdrv_ClipCursor` has none. The stall in a synchronous call that
  `CALL` was written for was never measured in GW2.

Against that, `win32u.so` was a third file of mine in every process, Steam's included, and 0.2.1
players ran it with both switches unset: all of the risk and none of the effect. It is the file
the next Wine base reaches first: it reads `window_shm_t` and fills `monitor_info`, whose layouts
change four times past 11.21. `macdrv-calls` had to be fixed by hand at the move to 0.4.0, when
athei's remote layers became views, and WineHQ's focus-event rewrite past 11.22 runs through
`macdrv_handle_event`, where it has two hunks.

So both go: their code, their switches, their tests and their rows. `dlls/win32u` is athei's again,
byte for byte at `501796b0ed1`. Of the Mac driver's upstream files only `macdrv_main.c`,
`surface.c`, `cocoa_gcmouse.m`, `window.c` and the AutoFill pick's `cocoa_app.m` still differ from
it.

## Consequences

- The `macdrv` archive has `winemac.so` alone. The build still makes a `win32u.so`, since
  `winemac.so` links against it, but nothing compares, stages or publishes it. The baseline,
  patched and fork compares, `selection.env`'s `WIN32U` and the job output `run_win32u` are gone,
  and so is the `WINE_WIN32U_*` part of the switch index's Lint.
- Gone from the tests: `setcursor.c`, `peekyield.c`, `clip.c`, `calls.c`, `native/pipe.c`, matrix
  block 7 and the steps that ran them, the engine tree with both of my files that only `clip.exe`
  used, and `requests.c`'s `GW2_TEST_SETCURSOR_SKIPS`. The window lane is shorter by block 7
  (ADR 0016).
- `ratio.c` (#159) stays. It tests `make_ratio()` in `dlls/win32u/sysparams.c`, which is athei's
  code now, so it runs with every window lane: a new base that brings the abort back shows there.
  A change under `dlls/win32u` sets `WINEMAC`, so a pull request that touches `sysparams.c` gets
  that lane. `winemac.so` links against `win32u.so` anyway.
- `compare.py` keeps its support for a library whose objects sit in subdirectories, unused, and its
  test of the debug map's names for them. Taking it out would touch the code every compare runs.
- My app needs its `DEV_GONE` rows for the six variables before this merges (#373). A driver build
  without `win32u.so` makes the app ask for the engine's own, so an update from 0.2.1 ends with the
  engine's file. That is my reading of `gw2om.sh`, not a run.
- In `SERIES.md` both topics move to the dropped list. This change's commit takes the topic
  `macdrv-calls`, and the rule `dlls/win32u/* win32u` puts its `win32u` lines in that topic, so
  each topic's series commit adds and removes the same lines and comes out empty. A replay of the
  series then has nothing of either to pick.

## Alternatives I rejected

- **Keep them behind their switches.** That is what 0.2.1 does, and it gives players a third file
  with no effect. The cost is at every base move, not in play.
- **Make them defaults.** The best case is about 2 µs a frame for the cursor and 13 µs for the view
  update, against a file of mine in every process.
- **Drop `win32u` and keep `macdrv-calls`.** The view update is the larger number, but it is under a
  thousandth of the game thread, and the topic is the one that broke silently at 0.4.0.
- **Leave the topics' code in and only stop shipping `win32u.so`.** The code would still meet every
  rebase.

## Evidence

- jdrly/gw2-on-mac#374, #61, #62.
- GW2 on Mac workspace: `advisor-plans/2026-11-26-topic-analysis/ANALYSIS-wine-mesa.md`, the
  sections `macdrv-calls` and `win32u` and "What the replays say", and `DECISIONS.md`, section 1.
- The replay of 2026-11-27: the series built from `SERIES.md` at `94c8b311781`, with the lines for
  #56 to #59, picked onto `501796b0ed1` without both topics: 19 clean, 2 left out, tree
  `ffa1a39f6d22`. That tree is `94c8b311781` with the two topics' series commits reverted.
