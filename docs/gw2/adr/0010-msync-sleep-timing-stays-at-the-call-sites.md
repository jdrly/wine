# msync's sleep timing stays at the call sites

Decided 2026-11-16. Status: accepted.

Every line of mine in athei's `msync.c` is a possible conflict at the next rebase, so the audit
looked for hooks to remove. One candidate (WINE-04) was the timing of msync's sleeps, which my
counters take at the callers of `ulock_wait`. Moving it into `ulock_wait` would turn four small
hunks into two. I left it where it is. `heat_sleep_on()` and `heat_woke_on()` take the object's
index, and `ulock_wait` only gets its address. With the timing in the wrapper, `WINE_HEAT_WAKE=skip`
with counting on would call `heat_wait_slept()` before `heat_woke_on()`. The waiter would stay
registered longer and could get an extra wake call, which is the call `skip` exists to remove.

## Consequences

- `msync.c` is byte for byte the base's apart from its `heat.h` include, so the order of the heat
  calls around a sleep is unchanged in every mode.
- The rest of that change went in. Six upstream files no longer include `heat.h` themselves, because
  `unix_private.h` does. The counters and timers each come from one list in `heat.h`, so an enum
  entry and its log name can't drift apart.
- The compare lists 20 changed functions for it. They differ only in the line number an `assert`
  would print, shifted by the deleted include line.
- The timed hunks stay as rows in `.github/gw2/UPSTREAM.md`, to be re-applied at an engine move.

## Alternatives I rejected

- **Timing inside `ulock_wait`.** It changes the wake order under `skip`, as above. It couldn't lose
  a wake, only add one, but that one is the wake `skip` is for.
- **Passing the object's index through athei's four `ulock_wait` call lines.** It keeps the order
  and puts more of my lines into his functions, not fewer.
- **A second, untimed wrapper for the skip branch.** Same result: it takes none of my lines out of
  his functions.

## Evidence

- jdrly/gw2-on-mac #221, jdrly/wine PR #38 (`08ecd6aad55`), with the reviewer's round 1 note on the
  order of `heat_wait_slept()` and `heat_woke_on()`.
- GW2 on Mac workspace: `advisor-plans/2026-11-16-forks/vetted-B-wine-ci-docs.md` ("WINE-04,
  corrected"), `plans/033-wine-ntdll-hook-cleanups.md`; `plans/night/2026-11-11-plan.md`, 2026-11-16
  20:46 ("WINE-04 timing dropped to keep skip's order").
