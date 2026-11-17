# athei's Mach9 Wine is the base, and my files replace the engine's one by one

Decided 2026-11-11. Status: accepted.

The 0.2 line of GW2 on Mac runs on Mach9, athei's arm64 Wine with FEX. My Wine changes for 0.1.x
were patches against CrossOver 26.3's source and don't carry over. So this fork exists: a fork of
athei/wine whose branch `gw2-on-mac` starts at `53def89293a` (`wine-11.21-37`), the commit Mach9
0.3.1's engine was built from. It builds three files, `ntdll.so`, `winemac.so` and `win32u.so`, and
my app drops them into Mach9's engine in place of its own. I don't build an engine. His tarball
stays the base. I chose Mach9 because it is open, pinned and documented, and by the author of the
engine 0.1.x uses.

## Consequences

- The fork moves to a new athei commit only when the app moves to the Mach9 release built from it.
  Mach9 pins his Wine by branch name, so a release's version string is the only place to read that
  commit from.
- The app keeps the engine's own file next to mine (`ntdll.so.engine`), so the engine's files are
  one setting away.
- Only Mach9's `wine.app` is signed by athei. It carries the cross-architecture entitlement through
  his provisioning profile. Everything else in the engine is ad hoc signed and replaceable under it,
  which is why single files can be swapped. A loader signed by my own team is jdrly/gw2-on-mac #33
  and waits for me.
- `arm64-patched` mirrors athei's branch and gets nothing of mine.
- Nothing goes to athei's repositories or to WineHQ, not a pull request, an issue or a comment. The
  changes here are AI-written. Issues are off in this fork, and the tracker is jdrly/gw2-on-mac with
  the labels `area:wine`, `area:mac-driver` and `area:build`.
- A commit of athei's that I take ahead of an engine move stays his commit, and drops out when the
  base has it.
- Because the files are drop-ins, each one has to be the engine's file plus my change and nothing
  else. ADR 0002 is how that is checked.

## Alternatives I rejected

- **Building my own engine from athei's repositories.** Asked and answered on 2026-11-11: not yet.
  The reason is not recorded.
- **CrossOver Preview's arm64 Wine.** On 2026-11-03 I ruled out arm64 Wine until CrossOver's arm64
  Wine sources are public. The dispatcher's note adds that I don't want CrossOver installed and that
  closed sources would block my own Wine fixes. athei's open arm64 Wine ended that wait.

## Evidence

- jdrly/gw2-on-mac #21 (the 0.2 line), #40 (the first `ntdll.so`), #33 (the loader).
- GW2 on Mac workspace: `plans/night/2026-11-11-plan.md`, 2026-11-11 19:08 (the fork created at
  19:10) and 23:00 (no engine of my own from athei's repositories yet);
  `docs/research/mach9-arm64-wine-2026-11-11.md`; `AGENTS.md` ("My own Wine builds for Mach9 come
  from GitHub's runners").
- The dispatch project file `gw2-on-mac.md` and the dispatcher's notes `arm64-0-2-line-and-ci` and
  `arm64-wine-waits-for-crossover-sources`.
