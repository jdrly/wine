# Every build is compared with the engine's file, then object by object with my own baseline

Decided 2026-11-11. Status: accepted. The byte gate is narrowed by ADR 0003.

My files replace the engine's own inside an engine I didn't build (ADR 0001). So "the engine's file
plus my change" has to be shown, not assumed. The workflow does it in two steps. First it rebuilds
the engine's commit and holds that baseline to the engine's shipped file. Then it compares a patched
build with that baseline per object file, through relocations: an object none of whose inputs
changed must equal the baseline's, and a touched object is listed function by function. The pull
request accounts for every listed function by cause.

## Consequences

- On the engine's own image (`macos-26-arm64` 20261102.0473.1, Xcode 26.6, clang-2100.1.1.101) my
  rebuild is the engine's `ntdll.so` and `winemac.so` byte for byte in the header, every section of
  every loaded segment, the load commands and the link-edit data. Only the debug map's source
  directories differ, because Mach9 builds with ccache.
- An object counts as touched when any input of makedep's rule for it is in the diff. The first rule
  looked at the `.c` file alone and failed on a hook in `unix_private.h`'s static inline, which
  changes `serial.o` without changing `serial.c`.
- The compare has a self-test with fixtures for each gap its review found, and a broken build that
  must fail and name its function.
- A string literal is named by its text and an anonymous constant by its own bytes, because clang
  renumbers their labels. A rule that forgave tail calls was dropped after its review showed it
  could hide a changed target.
- Every build flag stays the engine's. An -O2 `ntdll.so` broke Steam's login on 0.1.x.
- Each green push publishes the files to the pre-release `arm64-builds`, and the app's dev builds
  fetch them by commit.

## Alternatives I rejected

- **Comparing linked files with address operands masked by pattern.** This was the first compare.
  Its review kept finding the same kind of hole, a hand-made layout where a changed instruction
  compares equal: five in two rounds, then one more. I stopped patching it. It stays as a
  diagnostic.

## Evidence

- jdrly/wine PR #1 (`2fcc713`, the build, the gate and the compare), PR #5 and PR #6 (the touched
  path and the inputs rule), PR #10 (`winemac.so`), PR #20 and PR #26 (literals and constants).
- jdrly/gw2-on-mac #40.
- GW2 on Mac workspace: `plans/night/2026-11-11-plan.md`, 2026-11-11 22:12, 22:50, 22:56 ("the
  compare moves to a design without guessing"), 23:22 and 23:55 (the inputs rule), 2026-11-12 01:12
  (`winemac.so`), 2026-11-13 05:50 (the tail-call rule dropped); `AGENTS.md` ("My own Wine builds
  for Mach9 come from GitHub's runners"); `docs/research/engine-o2-2026-10-28.md`.
