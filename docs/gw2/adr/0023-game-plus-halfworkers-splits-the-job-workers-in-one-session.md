# game+halfworkers splits the job workers in one session

Decided 2026-11-26. Status: accepted.

My job-worker study of 2026-11-26 found GW2's job workers spending about 0.96 of a core in lock
sleeps and wakes at `ncpu=all`, and 43 to 59% of those sleeps end within 5 µs. The lock spin of
`WINE_HEAT_SPIN` would catch many of them, but its `:game` scope leaves the workers out. They are
the threads at ABOVE_NORMAL. Nobody has measured what a spin costs them in CPU.

Two sessions, one with the spin on the workers and one without, would compare two places, two
loads and two evenings. So I test it inside one session: half of the workers spin and the other
half don't, under the same load, and the heat log compares them thread by thread.

`lock=20:game+halfworkers` is `:game`, except that every other thread at priority 1 spins. A thread
at priority 1 takes a number at its first wait of a kind with that scope, and the threads numbered
0, 2, 4 and so on spin as if their priority were in scope. Every other thread is as with `:game`.
With counting, each thread's row in `heat-<pid>.log` has `spin 1` or `spin 0`: whether the lock
spin's scope took the thread at its last lock wait.

## Consequences

- The issue asked for the order in which the workers start. The number follows the order of their
  first waits, which splits them as evenly: of seven workers four spin and three don't. Numbering
  at the start would mean a hook in `heat_thread_priority()`, in `heat.h`. Every unix file of ntdll
  includes that header, so the compare would list every object as touched for a change that acts in
  one function (ADR 0022 turned down a line in `heat.h` for the same reason). The rows say which
  half a thread is in, so the order doesn't need to be known.
- The rule doesn't look at the start address. In GW2 the workers are the only threads at
  ABOVE_NORMAL, which is why `:game` means `:not=1`. Another thread of the game at that priority
  would take a number too.
- Without the word nothing behaves differently. The new test in `heat_spin_budget()` runs only for
  a thread whose priority the scope already leaves out, and it returns at once without the word.
  The row's field is written only with counting, and only when it changes.
- `spin` goes before `start` in a thread's line, so readers that take name and value pairs read it
  as one more pair. A heat log from before this change has no `spin`.
- `tests/gw2/spin.c` has a case for it, `halves`. Six threads at ABOVE_NORMAL, all started at one
  address, wait 3 ms on an alert that never comes, one after another, while no other thread is at
  that priority. Their rows' `spin` has to be 1, 0, 1, 0, 1, 0 in that order. A thread with
  `spin 1` has to have spun, and one with `spin 0` has to have counted `spin_scoped` and no spin.
  An alert would have let a wait skip the spin's decision when its thread was off its core, so
  the waits time out instead. In every run the `spin` of those six rows and of the two alerts
  threads' is checked against the scope.
  `matrix.zsh` runs it with the dev list's string, `lock=20:game+halfworkers,event=30:game`, beside
  the release row's string, which keeps checking that plain `:game` leaves ABOVE_NORMAL out.

## Alternatives I rejected

- **Two sessions, or two halves of one evening.** Different places and loads. A difference of a
  few percent of a core would be lost in that.
- **The start address of the worker loop.** It is `0x140dd5950` on the 2026-11-24 build and moves
  with game builds. My October notes had the loop at `0x140dd5200`.
- **Numbering by thread id.** The wineserver gives every process's threads their ids from one
  table (`alloc_ptid()`), and reuses freed ones once 256 are free. A thread of Steam or of the
  game's web helper can take the id between two workers, so the parity of an id says nothing about
  the order.

## Evidence

- jdrly/gw2-on-mac#368, and #366, whose dev list line this replaces.
- GW2 on Mac workspace: `advisor-plans/2026-11-26-job-workers/STUDY.md`, "Waits by type" and lever 1.
