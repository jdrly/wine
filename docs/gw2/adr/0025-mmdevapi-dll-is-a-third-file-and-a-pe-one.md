# mmdevapi.dll is a third file of mine, and a PE one

Decided 2026-11-27. Status: accepted.

Blish HUD follows the game's volume through NAudio 2.0.0. NAudio registers an
`IAudioSessionNotification` when it makes its session manager and unregisters it from the
manager's finalizer, and throws on any result but `S_OK` both times. Wine's two calls are stubs
that answer `E_NOTIMPL`. Blish HUD catches the first throw. The second comes on the finalizer
thread, where nothing catches it, and ends the process: Blish HUD's test 1 on 0.2 died within 5 s
of attaching to a program (jdrly/gw2-on-mac#387). The fix is in `dlls/mmdevapi`, which builds into
`mmdevapi.dll`, a PE file under `lib/wine/aarch64-windows`. My two files so far are unix
libraries under `lib/wine/aarch64-unix`.

So the fork builds a third file, my `mmdevapi.dll`: the engine's commit plus the fix, an ARM64X
file like the engine's, with an ARM64 half for ARM64 code and an ARM64EC half for x64 code under
FEX. The fix has no switch (ADR 0006). It only turns two failures into successes.

## Consequences

- `wine.zsh build` makes `dlls/mmdevapi/aarch64-windows/mmdevapi.dll` with the two unix files. The
  workflow strips it as Mach9 strips every PE file of the engine (`llvm-strip --strip-all`,
  athei/mach9 at v0.4.0, `bin/bundle-wine.sh`, Step 4e).
- `compare.py` reads Mach-O only, so `.github/gw2/pe.zsh` checks the file. My build of the engine's
  commit has the engine file's machine, sections, exports and imports, and this commit's has the
  same. The bytes are a report: the link writes the time and a hash of the contents into every
  build. Of the PE objects, only those whose source under `dlls/mmdevapi` the commit changes may
  differ from my base build's, in each half of the file. That is what "the engine's file plus my
  change" means for a PE file.
- Each green push publishes `mmdevapi-arm64-<commit>.tar.xz` to `arm64-builds`, with
  `mmdevapi.dll`, its `VERSION` and the licences, like the other two kinds. `VERSION` has the
  file's sha256 and both compares.
- `tests/gw2/sessions.c` registers and unregisters a notification as NAudio does. It has to pass
  under the engine with my file and fail on `E_NOTIMPL` under the engine's own. A runner may have
  no audio device, so it writes an endpoint into the prefix's registry first. The test lives
  under `tests/gw2`, not `dlls/mmdevapi/tests`: the fork configures with `--disable-tests`, so
  Wine's own tests never build here.
- My app installs no PE file of mine today. Installing this one is a ticket of its own in the app.
  It needs a place for the file, a download by commit and a check of its sha256, and the
  engine's own file kept next to it, as the app does for `ntdll.so`. The `wine` setting has to put
  that file back, since it is the fix's off switch (ADR 0006).
- Only the ARM64X file. The engine's i386 `mmdevapi.dll` stays, and the workflow doesn't build
  mine: Blish HUD is an x64 program (`Blish HUD.csproj` at `v1.3.0`).
- 0.1.x needs the same change against CrossOver 26.3's `mmdevapi.dll`, built in my workspace, not
  here.

## Alternatives I rejected

- **Patching NAudio, or running the Linux players' fork of Blish HUD that does.** That fork is a
  different program from a different source, and the app runs official releases only.
- **Sending session events.** Blish HUD needs the calls not to fail. A real `OnSessionCreated`
  would need the session list to tell the managers about each new session.
- **A switch.** The fix only changes a result that is already wrong, as ADR 0006 has it for every
  fix to Wine's own behaviour.

## Evidence

- jdrly/gw2-on-mac#387.
- GW2 on Mac workspace: `advisor-plans/2026-11-27-blish-hud/STUDY.md` (row 19 and patch 4) and
  `TEST1.md` (the crash on 0.2).
- NAudio `NAudio.Wasapi/CoreAudioApi/AudioSessionManager.cs:90-101,119-137` at `v2.0.0`. Blish HUD
  `GameServices/GameIntegration/AudioIntegration.cs:203` at `v1.3.0`.
- WineHQ's master at `ecca8e75524` (2026-11-26) still has both stubs.
