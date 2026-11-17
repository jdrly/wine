# On 0.2 the app carries the heat defaults: job wakes and the event pool on, fast locks off

Decided 2026-11-11 and 2026-11-12. Status: accepted. Two defaults still wait for a session of mine.

On 0.1.x the heat patch has its defaults built in: the event pool on, the wake policy at `one,skip`,
the workers' QoS move off. On this engine every switch is off in the fork (ADR 0004), so the
defaults a player gets are the app's Settings rows. Each row writes its variable into the game's
rule. Two rows start on, as on 0.1.x. In the first arm64 session, with neither, the game thread
spent 6.7% of its time waking job workers and 6.3% on event creates and closes. Two rows start off
until I've played them on this engine.

## Consequences

- *Job wakes* on, `WINE_HEAT_WAKE=one,skip`. On 0.1.x a release without a waiter makes no wake call
  and one with waiters wakes one worker. That cut the game thread's kernel time from 6.0 to 2.9% of
  a core.
- *Event pool* on, `WINE_HEAT_EVPOOL=1`. On 0.1.x it took the game thread's server calls from 86 to
  4 ms a second and was worth about 4 FPS at the busy spot.
- *Fast locks* off, `WINE_HEAT_SPIN`. The spin had never run on the game thread when the row was
  made. jdrly/gw2-on-mac #105 turns it on after a session of mine shows more hits than misses and no
  lost frame. That session hasn't happened.
- *Launcher frames* off, `WINE_MACDRV_SURFACE=copy,skip`, until I've looked at the launcher on three
  starts (#105).
- Existing 0.2 installs had `fastlocks=on` and `coolworkers=3` written while those rows were grey. A
  `settings_version` step reads both as off once.
- A dev build turns all of them on anyway.
- Cool workers has no row on 0.2 at all. That is the app's ADR 0006.

## Alternatives I rejected

- **Carrying the 0.1.x app's defaults over as they were.** The 0.1.x app ships fast locks on and
  three cool workers. The step 3 specification would have turned the spins on for every install, and
  its check caught that: the spin had never run on the game thread.
- **Every row off until played.** The record doesn't argue it. The two rows that start on are the
  two the first arm64 session showed missing.

## Evidence

- jdrly/gw2-on-mac #41 (the *Job wakes* row), #77 (the *Event pool* and *Fast locks* rows and the
  reset), #58 (*Launcher frames*), #105 (open: the two defaults that wait).
- GW2 on Mac workspace: `AGENTS.md` (the heat findings and the 0.1.x defaults);
  `plans/night/2026-11-11-plan.md`, where the plan's opening notes have the first session's sample,
  2026-11-11 19:20 and 19:31 (the defaults the tickets use until I say otherwise), 2026-11-12 00:27,
  02:08, 02:30 and 04:43.
