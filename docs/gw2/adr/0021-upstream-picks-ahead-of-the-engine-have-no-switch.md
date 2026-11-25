# Upstream picks ahead of the engine have no switch

Decided 2026-11-26. Status: accepted.

My upstream triage of 2026-11-25 found four small changes that reach my `ntdll.so` and
`winemac.so` before athei's next base would bring them (#342):

- athei's `5466044`: the Mac driver registers `NSEnableTextInputContextBasedAutoFill` as `NO`, so
  AppKit starts no `com.apple.SafariPlatformSupport.Helper` for each process that shows a window.
  My trace of 2026-11-20 has one next to the game.
- WineHQ's `dedead4`: the workaround for Rosetta reporting a write fault as a read fault is
  compiled only for x86_64 macOS. My arm64 `ntdll.so` runs no Rosetta, and had it on the fault path.
- WineHQ's `53ba24d`: on macOS `NtYieldExecution` calls `swtch()` and returns
  `STATUS_NO_YIELD_PERFORMED` when `swtch()` returns false. xnu returns false when, once the caller
  runs again, no other thread waits for its core, so another thread may have run in between. With
  `sched_yield()` it always returned success here, since macOS has no `RUSAGE_THREAD`.
- WineHQ's `48d67d3`: a sleep that isn't alertable changes. With a zero timeout it is only the
  yield. A relative one is a `nanosleep`, repeated with the rest after `EINTR`, without the clock
  read and the yield before it.

I take them with `git cherry-pick -x`, unchanged, and without a switch. They are Wine's and
athei's own code, which the next base brings as it is, so they count as fixes in the sense of
ADR 0006, as WineHQ's `ede017e` did for #160. The night shift's rule that new behaviour gets a
switch is for my own changes. The dispatcher confirmed that on 2026-11-26.

What they are worth, from the triage: the game thread yields 336 times a second at 0.31 µs, 0.1 ms
of each second, so `swtch()` can't save more than that. What changes is the priority after a
yield that gave way: `sched_yield()` is `swtch_pri(0)`, which lowers the caller's priority until
it runs again, and `swtch()` doesn't. I read xnu's source and measured neither. I expect nothing I
can see in play and take them so the next base has four commits less to bring.

## Consequences

- Each is a row in `.github/gw2/UPSTREAM.md`'s "Fixes without a switch". At the next move, a row
  whose commit the new base has leaves the table, and its pick leaves the list of my commits to
  carry. WineHQ has the three `ntdll` picks after `wine-11.22`, so an 11.23 base has them.
- They act in every process of the bottle, Steam and the launcher included. The `wine` setting,
  which puts the engine's own files back, is their off switch.
- My yield timer had to move into the branch macOS compiles now (ADR 0022).
- A pick changes no server request, so the protocol stays at 973.
- On arm64, athei's W^X code right after the Rosetta block (`WX_EXCLUSIVE_HOST` in
  `virtual_handle_fault`) now sees a write fault only when the CPU reports one. The arm64 signal
  handler takes the fault type from the WnR bit of the fault's ESR, a MAP_JIT view opens its
  write window itself, and FEX's handler for self-modifying code takes read and write faults
  alike. So I know of nothing that relied on the rewrite. The test matrix runs x86_64 programs
  through FEX under my `ntdll.so`, which covers the paths those programs reach.

## Alternatives I rejected

- **A switch for each pick.** It would put my lines into upstream's code, where the next base
  brings the same code without them, so each one would be a conflict at the move. And ADR 0006's
  reason holds: off by default, the changes would run nowhere I don't choose them.
- **Waiting for athei's 11.23 base.** That costs nothing, but brings nothing earlier either. The
  AutoFill helper is a process per window-showing process today.

## Evidence

- jdrly/gw2-on-mac #342.
- GW2 on Mac workspace `docs/research/upstream-watch-triage-2026-11-25.md`, the Wine section.
- athei/wine `5466044bde5`, WineHQ `dedead43f26`, `53ba24da06b`, `48d67d33670`.
- The dispatcher's answer in the night shift of 2026-11-26.
