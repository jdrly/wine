# The yield timer wraps swtch() in sync.c

Decided 2026-11-26. Status: accepted.

With `WINE_HEAT_COUNT=1`, my counters time every `NtYieldExecution` (`yield`, its five `yield_`
buckets and `yield_ms`, #40). The timer sat around `sched_yield()`. WineHQ's `53ba24d` (ADR 0021)
gives macOS a branch of its own that calls `swtch()`, and the pick left my timer in the
`HAVE_SCHED_YIELD` branch, which macOS no longer compiles. The counters would have stayed at 0.

So the macOS branch now has my timer: with counting, it reads the clock, calls `swtch()`, hands the
start to `heat_yield_done()` and returns what upstream's line returns. Without counting it runs
upstream's line. The `HAVE_SCHED_YIELD` branch is upstream's text again.

## Consequences

- The hook stays in `sync.c` and uses `heat_yield_done()` as before. A new function of mine in
  `heat.c` would have needed a line in `heat.h`, which every unix file of `ntdll` includes. Then the
  per-object compare would have named every object's inputs as changed by #342, where the picks
  change three: `sync.c`, `virtual.c` and `cocoa_app.m`.
- `yield` no longer counts a sleep with a timeout above 0. Before `48d67d3` each such sleep that
  wasn't alertable yielded once, up to 336 times a second on the game thread. A heat log from
  before this change and one from after aren't comparable on `yield`.
- `tests/gw2/timers.c` case 6 makes 1,100 yields on a thread of its own and 500 sleeps of 1 ms. Each
  of the 1,100 calls has to return `STATUS_SUCCESS` or `STATUS_NO_YIELD_PERFORMED`. With counting,
  the thread's row has to show at least 1,100 yields and fewer than 500 more. matrix.zsh's block 6
  runs it under mine with counting, and block 2 without, under the engine's file and mine.

## Alternatives I rejected

- **`heat_yield()` in `heat.c`, one line in `sync.c`.** Fewer of my lines in an upstream file, but
  `heat.h` changes for every object, as above.
- **Leaving the timer where it was.** It would count nothing on macOS.

## Evidence

- jdrly/gw2-on-mac #342, #40.
- WineHQ `53ba24da06b`, `48d67d33670`.
