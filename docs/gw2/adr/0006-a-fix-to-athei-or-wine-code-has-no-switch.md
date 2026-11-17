# A fix to athei's or Wine's own behaviour has no switch

Decided 2026-11-11, widened 2026-11-13. Status: accepted.

Every change of mine has a switch (ADR 0004), with one exception: a fix. The first one was the
wait-all's mutex rollback, which my 0.1.x build has and athei's tree lacks: it only changes a state
that is already wrong, and 0.1.x has had it unconditionally since heat round 7b. So it got no switch
and acts in every process, Steam included. The way to turn it off is the app's `wine` setting, which
puts the engine's own file back with every other change of mine gone.

## Consequences

- The fixes without a switch today, all in `msync.c` unless named: the wait-all's rollback releases
  only the mutexes this pass took (#44), the contention bailout unregisters from the server (#44,
  #104), a relative timeout's deadline is computed before the first spin or sleep (#101), a wait-all
  that puts an object back wakes its waiters (#219, ADR 0007), two win32u fixes of athei's that
  I took ahead of an engine move (#159), athei's fix that gives a remote break-in on ARM64 the
  target's own `DbgUiRemoteBreakin` (#174), and WineHQ's fix that makes a restored window the
  foreground window before it is activated (#160).
- The unregister started behind `WINE_HEAT_MSYNC_UNREG=1`, set by the app for the game only. So its
  off path ran only where nobody chose it, in Steam, the launcher's helpers and every other process
  of the bottle, which are the processes that wait on several objects alertably. On 2026-11-13 it
  became plain code (#104, jdrly/wine PR #22). #104 is still open.
- `.github/gw2/UPSTREAM.md` has a second table, "Fixes without a switch", with a column for whether
  upstream has each one. A rebase starts by checking every row again.
- The mutex, unregister and put-back fixes each have a test that must fail under the engine's own
  `ntdll.so` and pass under mine.
- A fix goes off if a normal session shows a hang in Steam, the launcher or the game that the build
  before didn't have.
- Nothing goes to athei's repositories. If one of these is worth reporting, that is mine to do.

## Alternatives I rejected

- **A switch for each fix.** The unregister had one for two days. It left the wrong behaviour on in
  exactly the processes that hit it.
- **Fixes only in `Gw2-64.exe`.** That was 0.1.x's split for the unregister. The same reasoning
  ended it.

## Evidence

- jdrly/gw2-on-mac #44 (the mutex fix has no switch of its own, and the rule for the default), #104
  (the unregister in every process and without a switch), #101, #219, #159.
- jdrly/wine PR #3 (`f609772`), PR #22 (`41ca59151dc`), PR #36 (`30c65fd5a6d`).
- This fork's `.github/gw2/UPSTREAM.md` (jdrly/wine PR #40).
- GW2 on Mac workspace `plans/night/2026-11-11-plan.md`, 2026-11-11 19:31 and 2026-11-12 01:08.
