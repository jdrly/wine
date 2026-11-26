# The fork builds on the Xcode 27 runner, with a structural gate against the engine's files

Decided 2026-11-12. Status: accepted. It narrows ADR 0002's byte gate until the engine's toolchain
is available again.

GitHub's `macos-26` runners stopped being assigned on 2026-11-12. Four attempts of the build job sat
in the queue for 15 minutes each and were cancelled, while `xcode-27` runners started in seconds for
the app and my DXMT fork. The engine was built with Xcode 26.6, and Xcode 27's clang isn't that
compiler, so my rebuild of the engine's commit can't be the engine's file byte for byte there. I
decided to go on with what we have and compile on 27. The gate changes from "my rebuild is the
engine's file" to "my rebuild has the engine's file's shape, and my change touches exactly these
functions".

## Consequences

- The structural gate holds my build of the engine's commit to the engine's file in its exports and
  imports, linked libraries and install names, `minos`, segments and sections, and the set of
  functions by name. A difference there fails. How many functions differ in their bytes is printed
  and doesn't fail: `ntdll.so` 14 of 979, `winemac.so` 118 of 970, `win32u.so` 65 of 3,240.
- The patched compare is unchanged. It runs against my baseline from the same run and toolchain and
  still names every function a change touches.
- The tests on the engine are the functional proof for files from another compiler, so none may be
  skipped for that reason.
- `MACOSX_DEPLOYMENT_TARGET` stays the engine's 26.4, and the gate checks `minos`.
- The record of a build names its toolchain and which gate held.
- The runner label and the Xcode version are two values at the top of the workflow. With the
  engine's toolchain the byte gate applies again, so going back is one edit.
- The first dev build from this toolchain needs my first-session check again: Steam's login, the
  launcher and the game with every row off. jdrly/gw2-on-mac #124 stays open for it.
- My FEX fork builds on the same runner. Its two PE files come from the pinned llvm-mingw and keep
  their byte gate.

## Alternatives I rejected

- **Waiting for `macos-26` capacity.** Four cancelled attempts in one day, with a queue of approved
  work behind them.
- **GitHub's macOS 27 image with Xcode 26.6.** That image carries only Xcode 27.0 to 27.2.
- **A larger runner.** `macos-26-xlarge` costs $0.102 a minute and is offered only to organizations
  on a Team or Enterprise plan.
- **A rented Mac mini as my own runner.** Out of stock, or 300 EUR and more.

## Evidence

- jdrly/gw2-on-mac #124 and its comment of 2026-11-12, jdrly/wine PR #19 (`b496e499a64`), PR #26.
- GW2 on Mac workspace `plans/night/2026-11-11-plan.md`, 2026-11-12 17:55 (the images), 20:20 (my
  decision to go on with what we have and compile on Xcode 27), 21:05 and 21:30 (the first runs),
  2026-11-13 05:50 (the counts after the report's fix).
- The dispatch project file `gw2-on-mac.md`, "Runner for the Wine and FEX forks".
