# Moving to another Mach9 release

My three files replace the engine's, so they have to be built from the commit the engine was
built from, with the engine's configuration. This is the list of what moves when the engine does,
in the order I'd do it. Line numbers are as of this file's last change; the names are what to
search for.

## 1. The new engine's Wine commit

Mach9 pins athei's Wine by branch name (`pins/wine-src.ref` says `arm64-patched`), so the
release's own `ntdll.so` is the only place that names the commit. Unpack the release tarball and
read its version string, as the workflow's "Stage" step does with my build:

```zsh
strings -a wine/lib/wine/aarch64-unix/ntdll.so | grep -oE 'wine-[0-9]+\.[0-9]+-[0-9]+-g[0-9a-f]+'
```

`wine-11.21-37-g53def89293` means athei/wine `53def89293a`.

## 2. What the new base already has

Go through the second table of `.github/gw2/UPSTREAM.md`, "Fixes without a switch", and check
each fix against the new commit. Drop the commits it already has before the rebase, and update
the "Upstream has it" column for the rest.

## 3. Rebase, then every value tied to the engine

Rebase `gw2-on-mac` onto the new commit, with the first table of `UPSTREAM.md` as the list of
hooks to carry over. Then each of these, with where its new value comes from:

- `.github/workflows/gw2.yml:113`, `BASE_COMMIT`: the commit of step 1, and its comment's
  `wine-<version>-<n>-g<sha>`.
- `gw2.yml:124-125`, `ENGINE_RUNNER` and `ENGINE_XCODE_VERSION`, with the comment above them: the
  image and Xcode of Mach9's release run for the new tag (its run log names both).
- `gw2.yml:119-120`, `RUNNER` and `XCODE_VERSION`: the engine's two if GitHub assigns that runner,
  for the strict gate, otherwise the newest Xcode image my app builds with. See the last section.
- `gw2.yml:126-127`, `LLVM_MINGW_VERSION` and `LLVM_MINGW_SHA256`: Mach9's `pins/redist.env` at
  the new tag.
- `gw2.yml:128-129`, `MACH9_VERSION` and `MACH9_SHA256`: the new tag, and the sha256 of its
  `wine-<tag>-macos-arm64.tar.xz`, computed from the download.
- `gw2.yml:141`, `MACOSX_DEPLOYMENT_TARGET: "26.4"` in the build job: `MACOS_FLOOR` in Mach9's
  `bin/lib/profile.sh`.
- `.github/gw2/wine.zsh:54-74`, the configure arguments in `configure()`: Mach9's
  `bin/build-wine.sh` and `bin/lib/profile.sh` at the new tag. `ac_cv_func_pipe2=no` follows the
  deployment target. `SRC` and `BUILD` (`wine.zsh:26` and the build job's env) are Mach9's CI
  paths, which reach the file through its debug map; check them against the release run.
- `wine.zsh:44`, `core.abbrev 10`: the number of hex digits after `-g` in the string of step 1.
- `gw2.yml:251`, the comment on FreeType: the version in Mach9's new release run. The build takes
  whatever Homebrew has and names it in `VERSION`.
- `dlls/ntdll/unix/heatreq.h`, the server's request list: run
  `python3 -I .github/gw2/heatreq.py write include/wine/server_protocol.h dlls/ntdll/unix/heatreq.h`
  after the rebase. Lint fails until it matches.
- `dlls/ntdll/unix/heatprof.h:40-42`, `HEAT_FEX_PIN_GUID`, `HEAT_FEX_PIN_AGE` and
  `HEAT_FEX_PIN_SIZE`: the CodeView GUID, age and SizeOfImage of the new engine's
  `wine/lib/wine/aarch64-windows/libarm64ecfex.dll`, and the sha256 in the comment above them. If the
  new engine has another FEX, check the layouts in `heatprof.h` (`HEAT_FEX_STATE_*`,
  `HEAT_FEX_TAIL_*`) and `heatfex.h` (`HEAT_FEX_STATS_*`) against its source too. Until the pin
  moves, the sampler refuses the new FEX and stays off.
- `tests/gw2/matrix.zsh:258`, the same GUID as hex, for the sampler's test.

## 4. Outside this repository

- My FEX fork, `.github/workflows/gw2.yml`: `BASE_COMMIT` and `BASE_VERSION` (lines 117-118), the
  FEX the new engine has, and `LLVM_MINGW_VERSION`, `MACH9_VERSION` and `MACH9_COMMIT` (128-133).
- My DXMT fork, `.github/workflows/gw2.yml:63-66`: `LLVM_MINGW_VERSION`, `LLVM_MINGW_SHA256`,
  `MACH9_VERSION` and `MACH9_SHA256`.
- My app, on its `arm64` branch: `ENGINE_NAME`, `ENGINE_URL` and `ENGINE_SHA256` in
  `src/gw2om.sh:129-135`, the `engine:` lines of `.github/workflows/checks.yml:196,215`, the
  engine's name, commits and URL in `scripts/test-sandbox.sh:283,288`, and `README.md:398-399`.

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
