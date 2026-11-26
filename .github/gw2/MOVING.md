# Moving to another Mach9 release

My three files replace the engine's, so they have to be built from the commit the engine was
built from, with the engine's configuration. This is the list of what moves when the engine does,
in the order I'd do it. Line numbers drift, so it names what to search for instead.

## 1. The new engine's Wine commit

Mach9 pins athei's Wine by branch name (`pins/wine-src.ref` says `arm64-patched`), so the
release's own `ntdll.so` is the only place that names the commit. Unpack the release tarball and
read its version string, as the workflow's "Stage" step does with my build:

```zsh
strings -a wine/lib/wine/aarch64-unix/ntdll.so | grep -oE 'wine-[0-9]+\.[0-9]+-[0-9]+-g[0-9a-f]+'
```

`wine-11.21-54-g501796b0ed` means athei/wine `501796b0ed1`.

Then read `SERVER_PROTOCOL_VERSION` in that commit's `include/wine/server_protocol.h`. A release on
the same Wine version can still change it: 0.4.0 is on 11.21 like 0.3.1, and went from 972 to 973
with two requests of athei's. My files from the old base can't talk to the new wineserver then, so
the app has to move the engine and my files in one step.

## 2. What the new base already has

Go through the second table of `.github/gw2/UPSTREAM.md`, "Fixes without a switch", and check
each fix against the new commit. Drop the commits it already has before the rebase, and update
the "Upstream has it" column for the rest.

## 3. Rebase, then every value tied to the engine

Rebase my commits onto the new commit (which ones, since the move to 0.4.0: section 3b), with the
first table of `UPSTREAM.md` as the list of hooks to carry over. Then each of these, with where its new value comes from:

- `BASE_COMMIT` in `.github/workflows/gw2.yml`'s `env:`: the commit of step 1, and its comment's
  `wine-<version>-<n>-g<sha>`.
- `ENGINE_RUNNER` and `ENGINE_XCODE_VERSION` in the same block, with the comment above them: the
  image and Xcode of Mach9's release run for the new tag (its run log names both).
- `RUNNER` and `XCODE_VERSION`: the engine's two if GitHub assigns that runner, for the strict
  gate, otherwise the newest Xcode image my app builds with. See the last section.
- `LLVM_MINGW_VERSION` and `LLVM_MINGW_SHA256`: Mach9's `pins/redist.env` at the new tag.
- `MACH9_VERSION` and `MACH9_SHA256`: the new tag, and the sha256 of its
  `wine-<tag>-macos-arm64.tar.xz`, computed from the download.
- `MACOSX_DEPLOYMENT_TARGET: "26.4"` in the build job's `env:`: `MACOS_FLOOR` in Mach9's
  `bin/lib/profile.sh`.
- The configure arguments in `configure()` of `.github/gw2/wine.zsh`: Mach9's `bin/build-wine.sh`
  and `bin/lib/profile.sh` at the new tag. `ac_cv_func_pipe2=no` follows the deployment target.
  `SRC` and `BUILD` (their defaults in `wine.zsh` and the build job's `env:`) are Mach9's CI paths,
  which reach the file through its debug map; check them against the release run.
- `core.abbrev 10` in `wine.zsh`: the number of hex digits after `-g` in the string of step 1.
- The comment on FreeType in the build job's step "Install Homebrew's bison, freetype, gnutls and
  SDL": the version in Mach9's new release run. The build takes whatever Homebrew has and names it
  in `VERSION`.
- `dlls/ntdll/unix/heatreq.h`, the server's request list: run
  `python3 -I .github/gw2/heatreq.py write include/wine/server_protocol.h dlls/ntdll/unix/heatreq.h`
  after the rebase. Lint fails until it matches.
- `HEAT_FEX_PIN_GUID`, `HEAT_FEX_PIN_AGE` and `HEAT_FEX_PIN_SIZE` in
  `dlls/ntdll/unix/heatprof.h`: the CodeView GUID, age and SizeOfImage of the new engine's
  `wine/lib/wine/aarch64-windows/libarm64ecfex.dll`, and the sha256 in the comment above them. If the
  new engine has another FEX, check the layouts in `heatprof.h` (`HEAT_FEX_STATE_*`,
  `HEAT_FEX_TAIL_*`) and `heatfex.h` (`HEAT_FEX_STATS_*`) against its source too. Until the pin
  moves, the sampler refuses the new FEX and stays off.
- The same GUID as hex in `tests/gw2/matrix.zsh`, on the line that names `HEAT_FEX_PIN_GUID`, for
  the sampler's test.
- The tests that have to fail under the engine's own file, on a bug my file fixes: `engine_bugs` in
  `tests/gw2/matrix.zsh`, and the workflow's steps that run a program under `$RUNNER_TEMP/engine`
  or the engine's `win32u.so` and expect it to fail. For every fix the new base has, that run
  passes now, so it goes. At 0.4.0 that was modeset.exe (#159) and attach.exe (#174).
- Hooks that applied without a conflict into code athei changed. Read his diff of every file in
  the first table of `UPSTREAM.md`. At 0.4.0 his remote layers became views a client surface can be
  ordered under, so `WINE_MACDRV_VIEW`'s serial had to change with them too.

## 3b. The pull request of a rebase

athei rewrites his branch, so my rebased commits share only upstream Wine with `gw2-on-mac`, and
GitHub can't merge them into it: the pull request gets no merge commit and no run. Its head is
therefore a merge of `gw2-on-mac` into the rebased commits that keeps the rebased tree (`git merge
-s ours origin/gw2-on-mac`). That head merges into `gw2-on-mac` without a conflict, its merge has the
head's tree, and the commits of the old base stay reachable, so the commit ids in this folder and in
the ADRs still resolve. The pull request lands as a merge commit or a fast-forward, never squashed:
a squash would leave athei's new commits out of the branch's history, and `BASE_COMMIT`, which
every build checks out, would then hang on the pull request's branch alone.

Because the old line stays reachable, a plain `git rebase` of `gw2-on-mac` at the next move would
replay it too: athei's old commits and mine on them. The next move takes only my commits since this
one, oldest first, and cherry-picks them onto the new commit:

```zsh
git rev-list --reverse --no-merges 501796b0ed1..gw2-on-mac ^4e55fb228c0
```

`501796b0ed1` is the base of 0.4.0 and `4e55fb228c0` the tip of the 0.3.1 line. A later move of
this kind adds its own old tip to the `^` list.

Since 2026-11-26 the series replaces this command: [`docs/gw2/SERIES.md`](../../docs/gw2/SERIES.md)
gives each of these commits a topic, and the next move replays the `series/…` branch it names onto
the new commit, topic by topic.

## 4. Outside this repository

My DXMT fork keeps the same list for its side, with my app's items too, in
[`docs/gw2/README.md`, "Moving to a new Mach9"](https://github.com/jdrly/dxmt/blob/gw2-on-mac/docs/gw2/README.md#moving-to-a-new-mach9)
on its `gw2-on-mac`. In short:

- My FEX fork, the `env:` of `.github/workflows/gw2.yml`: `BASE_COMMIT` and `BASE_VERSION`, the
  FEX the new engine has, and `LLVM_MINGW_VERSION`, `LLVM_MINGW_SHA256`, `MACH9_VERSION`,
  `MACH9_SHA256`, `MACH9_COMMIT` and `FEX_SMOKE_SHA256`, the sha256 of `smoke/fex.c` at that commit.
- My DXMT fork, the `env:` of `.github/workflows/gw2.yml`: `LLVM_MINGW_VERSION`,
  `LLVM_MINGW_SHA256`, `MACH9_VERSION` and `MACH9_SHA256`.
- My app, on its `arm64` branch: the `ENGINE_*` block of `src/gw2om.sh`, which its comment says
  to bump together. The name, title, URL, the asset's API URL (its id), size and sha256 come from
  the release; the Wine, FEX and DXMT commits from the release page's Components list; the
  loader's, `ntdll.so`'s, `winemac.so`'s, `win32u.so`'s, `d3d11.dll`'s, `winemetal.so`'s and FEX's
  hashes from the unpacked tarball. An old hash makes `engine_matches` refuse the new engine.
  `MACOS_MIN`, right after the block, follows the loader's `Info.plist`. Then the two `engine:`
  lines of `.github/workflows/checks.yml`; in `scripts/test-sandbox.sh`, `ENGINE_WINE`,
  `ENGINE_FEX` and every check that names the engine's tag, URL, size, sha256 or commits (search
  for `v0.3.1` and the three commits); and the engine paragraph of `README.md`'s "The pieces".

## 5. What the gates hold my files to then

The build job builds `BASE_COMMIT` and compares each file with the new engine's: byte for byte on
the engine's runner and Xcode, structurally otherwise. Then `compare.py patched` holds this
commit's objects to that base build, and the tests run my files in the new engine.

An engine that is not a Mach9 release, my own build of upstream Wine for example, has no engine
file to compare my base build with. The baseline gate then has nothing to hold to, and only the
patched compare and the tests remain. I haven't decided what replaces it. Such a move needs my
decision first.

## Why the build doesn't run on the engine's own runner

Until 2026-11-12 it ran on macos-26 with Xcode 26.6, and my build of the engine's commit was the
engine's ntdll.so and winemac.so byte for byte in the header, every section of every loaded
segment, the load commands and the link edit data (run 43109476509 of pull request #1 and run
43156131896 of #10). Since 2026-11-12 it runs on GitHub's Xcode 27 image: no macos-26 runner was
assigned all evening, and I have no Mac of my own for it. Xcode 27's clang isn't the engine's, so
my files can't be the engine's byte for byte. The patched compare is the same either way, my base
build against this commit's from one run and one toolchain, and the tests on the engine are the
proof that files from the other compiler work.
