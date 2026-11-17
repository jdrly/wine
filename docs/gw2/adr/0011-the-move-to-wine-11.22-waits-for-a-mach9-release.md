# The move to Wine 11.22 waits for athei's release on it

Decided 2026-11-13, planned 2026-11-16. Status: accepted. The move itself is blocked: no engine on
Wine 11.22 exists.

My three files are built from athei's Wine at `53def89293a`, Wine 11.21, server protocol 972. Wine
11.22 is tagged upstream with protocol 979, so files from an 11.21 base can't talk to an 11.22
wineserver. When the engine moves, all three move with it in one step. 11.22 also rewrites the Mac
driver's window surface function that my launcher fix lives in, and athei's own series overlaps
upstream in 33 files outside tests. My survey calls it the costly move. I don't make it myself. I
take athei's rebase as the base and wait for his releases on it. The move happens on his second
Mach9 release on 11.22, as jdrly/gw2-on-mac #166 says. His first is for the trial: the rebase of my
commits, the gates, and upstream's IOSurface window surfaces (#167). Moving on the first needs my
explicit go.

## Consequences

- The trigger is a Mach9 release whose engine reports `wine-11.22-…`. The upstream tag alone is not
  one, and neither is athei's branch moving without a release.
- If his next release is still on 11.21, jdrly/gw2-on-mac #166 takes it as an ordinary engine move.
  My commits rebased onto his 11.21 tip without a conflict in two trials.
  Taken on 2026-11-17 with Mach9 0.4.0: athei/wine `501796b0ed1` (`wine-11.21-54`), still Wine
  11.21 but server protocol 973, not 972. His FEX commit gained two requests,
  `set_cross_process_work_section` and `get_cross_process_work_section`, so my files from the old
  base can't run on 0.4.0's wineserver, and the app moves the engine and my files in one step. The
  rebase had one conflict, twice: his msync now unregisters on the contention bailout itself, the
  fix of #44 and #104, so his code stays and only my count is left there.
- I don't take his Mac driver changes ahead of the engine. He rewrote that commit twice in four
  days, and I know no GW2 symptom they fix.
- On the first 11.22 engine I try upstream's IOSurface window surfaces before porting my frame copy
  (#167). Until then the copy stays inside `macdrv_surface_flush`, and moving it into a file of mine
  is not planned (WINE-09).
- The fork was made ready ahead of the move: `heatreq.h`'s list is generated (#218), fewer hooks sit
  in upstream files (#221), the put-back fix is in (ADR 0007), and `.github/gw2/MOVING.md` and
  `UPSTREAM.md` say how a move goes and which lines are mine (jdrly/wine PR #40, merged 2026-11-17).
- With 11.22's extended-state commits the shared data page says AVX whatever the emulator reports.
  My FEX fork's `avx-hidden` test will then fail, and this fork needs a part that makes the page
  follow the emulator.
- The move gets one forced update of `gw2-on-mac`, with my go on the day.

## Alternatives I rejected

- **An engine of my own on 11.22.** The plan lists it as an alternative and leaves it out. I replace
  files in athei's engine and don't build one (ADR 0001).
- **Rebasing onto the upstream tag before athei does.** The plan names the tag alone as no trigger.
  My hooks sit in athei's msync, which upstream Wine doesn't have.
- **Moving on his first 11.22 release.** My survey expects it to be rougher than 0.3.1, and a new
  engine under the game changes the server protocol, the Mac driver's surface path and ntdll's
  memory code at once. So the first release gets the trial and the second the move.

## Evidence

- jdrly/gw2-on-mac #166 and its comments of 2026-11-15 ("For 11.22 I wait for his second release on
  that base"), #167.
- GW2 on Mac workspace: `docs/research/wine-upstream-survey-2026-11-13.md` ("Should I move the 0.2
  engine?"), `docs/research/upstream-watch-triage-2026-11-15.md`;
  `advisor-plans/2026-11-16-forks/plans/039-wine-move-to-11.22.md` ("Trigger", "Which base") and
  `plans/README.md` (039 BLOCKED, WINE-09); `plans/night/2026-11-11-plan.md`, 2026-11-16 13:20 (the
  move as package P11) and 13:58.
