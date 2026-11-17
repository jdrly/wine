# A wait-all that puts an object back wakes its waiters

Decided 2026-11-16. Status: accepted.

msync's wait-all takes its objects one by one. When a later one is gone, it puts back what it took.
athei's code puts a semaphore's count, a mutex's owner or an auto-reset event's state back with a
plain store and no wake. A thread that looked at the object while the wait-all had it then sleeps on
with the object free, until something else releases it. The audit of 2026-11-16 found this while
looking at my own code: with `WINE_HEAT_WAKE=one` my pass-on of a wake fired from a stale status,
and that stray wake was the only one such a sleeper got. Removing the stray wake alone would have
uncovered the lost one. So the put-back is fixed first, and for good: each put-back wakes as a
release or a set does. Like my other fixes to athei's msync, it has no switch (ADR 0006).

## Consequences

- In `tooslow:` each put-back calls `signal_all` after its store: a mutex this pass took, a
  semaphore's count, an auto-reset event.
- With `one`, a wait-all passes a wake on for a semaphore only when the pass slept on that semaphore
  and was woken. The put-back's wake comes first in the history for that reason.
- With `skip`, the semaphore's put-back is never skipped. No release is in progress there.
  Changed on 2026-11-17 by ADR 0018: the put-back passes 0 to `signal_all`, so it wakes every
  waiter in every mode, `one` included.
- The fix is always on and acts in every process. It is a row in `.github/gw2/UPSTREAM.md`'s second
  table, "Fixes without a switch", checked against athei's branch on 2026-11-16: he doesn't have it.
- `tests/gw2/putback.c` shows it on the runner. Under the engine's own `ntdll.so` the test has to
  fail 3 of 3 on the lost wake, and it does ("the waiter slept on with the token put back", after
  about 500 ms). Under mine it passes in off, one, skip and one,skip.
- Each put-back costs one more wake call, on a path that only runs when a wait-all loses a race.
  Whether GW2 does a wait-all on a semaphore, mutex or auto event at all is unmeasured. My first
  session on the dev build that carries it is the check that nothing got worse.
- The plan that makes the wake skip decide by msync's reference count alone (plan 034) comes after
  this one, so each change to the wake path gets its own run of the matrix. On 2026-11-17 I let it
  go ahead with its verifier's conditions. ADR 0018 records it.

## Alternatives I rejected

- **Fixing only my stale-status wake.** That was the audit's first sketch. It removes the one wake a
  sleeper gets after a put-back in that mode.
- **Leaving athei's put-back as it is.** The lost wake is there with or without my fork, and my code
  only hid part of it by accident.

## Evidence

- jdrly/gw2-on-mac #219, jdrly/wine PR #36 (`30c65fd5a6d`), runs 43901133242, 43908623198 and
  43914959674.
- GW2 on Mac workspace: `advisor-plans/2026-11-16-forks/vetted-B-wine-ci-docs.md` ("WINE-05,
  corrected: fix the put-back first"), `plans/035-wine-wake-after-wait-all-putback.md` and
  `plans/README.md`; `plans/night/2026-11-11-plan.md`, 2026-11-16 16:50 to 18:46.
