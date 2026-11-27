# My Wine fork as a patch series

- Fork: jdrly/wine, branch `gw2-on-mac`.
- Base: athei/wine `501796b0ed1` (wine-11.21-54), the commit Mach9 0.4.0's engine was built from,
  with the lightweight tag `engine-0.4.0`.
- Head the topic table was written against: `c8d41f337ee` (#55), 2026-11-26. #56 to #59 came
  after it and have their lines in `series-topics`, and two topics were dropped on 2026-11-27
  (below).
- Series branch: `series/2026-11-26`.

The workspace's ADR 0015 makes each of my forks a curated patch series. This file is the series of
this one: every commit in `501796b0ed1..gw2-on-mac` with its topic, and the rules that cut those
commits into one or a few commits per topic on a `series/…` branch. That branch's tree equals
`gw2-on-mac`'s. It is the same code cut differently, and nothing on it is dropped.

148 commits are in `501796b0ed1..c8d41f337ee`. 59 are what I carry on the engine's commit. The
other 89 are the 0.3.1 line, which the move to 0.4.0 kept reachable under a merge, and the two
merges of that move. The counts below are those 148: the four commits after them and the drop's
commit aren't in the table.

## Topics

How to read the columns:

- **Fork default** is what the code does with the variable unset.
- **0.2.1** is what my app's 0.2.1 does in a release build. *Set* means the app writes the
  variable, in the game's compatdb rule or the session's environment. *Shipped, unset* means the
  code is in the file 0.2.1 installs and the fork default applies. *Not in use* means nothing a
  player runs reads it. 0.2.1 installs my `ntdll.so` at `43e22bd` and my `winemac.so` and
  `win32u.so` at `5e7bde7`.
- **App** is where my app's `arm64` branch names the variable. *Release* is a row or a line a
  release build writes when its setting asks for it, for the files that read it. *Dev list* is a list only a development build reads. Dropping a switch the release
  or the dev list writes takes away what that row does. The app leaves out a switch the file
  doesn't have and goes on.
- **Verdict.** A drop is a proposal, and I decide each one. Everything that never became a default
  and has no note against it is "decide", with the open case named.

| Topic | Commits | What it does | Switches (fork default) | 0.2.1 | App | Evidence | Verdict |
|---|---|---|---|---|---|---|---|
| `ci` | 18 | The `GW2` workflow: builds my two files on the engine's commit, compares each object with the engine's file, publishes the assets, and decides which tests a change runs. Since #323 a pull request picks blocks 4 to 6 and the two broken builds by the paths it changes (ADR 0012), one with only Markdown builds nothing, and every push runs every test | none | not in use | no | n/a | **keep.** CI |
| `tests` | 11 | Commits made for `tests/gw2`, most of them with CI or doc lines as well: the leftover-process check, arcdps's crash window through `rundll32`, the mode-set and debugger-attach tests (gone in #323: both fixes are the engine's own since 0.4.0), and how the matrix runs | none | not in use | no | n/a | **keep.** Tests |
| `docs` | 4 | `AGENTS.md`, `SWITCHES.md`, `UPSTREAM.md`, `MOVING.md`, this file and the ADRs. The backfill brought 0001 to 0014 and 0017. The other ADRs came with their pull requests, and the rules put them here | none | not in use | no | n/a | **keep.** Docs |
| `engine-pins` | 1 | `f709c4cc012`: the values tied to Mach9 0.4.0 (`BASE_COMMIT`, the FEX pin of the sampler, the request list, the tests that must fail under the engine's file) | none | shipped | no | n/a | **keep.** Each move writes it again, per `MOVING.md` section 3 |
| `heat-core` | 2 | `heat.c`, `heat.h` and `heat_private.h`: the switch parser, the process filter and the hooks' shared state. Its two commits are the split into one file per switch family (#27) and the smaller hooks (#38) | `WINE_HEAT_EXE` (`Gw2-64.exe`), `WINE_HEAT_LOG` (`/tmp`) | `WINE_HEAT_LOG` set with Debug logging | Release: `WINE_HEAT_LOG` | Every `WINE_HEAT_*` topic needs it | **keep.** Always there |
| `counters` | 4 | Per-thread counters and the game thread's server requests by name, written to `heat-<pid>.log` and `heat-requests-<pid>.log` | `WINE_HEAT_COUNT` (off), `WINE_HEAT_DUMP_MS` (5000, tests only) | `WINE_HEAT_COUNT=1` set with Debug logging, which is off in a release | Release: `WINE_HEAT_COUNT`. The session report reads the file | The triage of 2026-11-25 took its yield and native-exit numbers from it | **keep.** Diagnostics the app reads |
| `job-wakes` | 2 | A semaphore release of count n makes at most n wake-one calls, and a release of a semaphore only this process uses, with no thread of it asleep on it, can make none (#2, #48) | `WINE_HEAT_WAKE` (off) | set, `one,skip` (the *Job wakes* row, on) | Release | On 0.1.x the game thread's kernel time went from 6.0 to 2.9% of a core and lock waits from 75 to 25 ms a second (`heat-locks-2026-10-29.md`). The first arm64 session spent 6.7% of the game thread on waking workers (ADR 0005) | **keep.** A release default |
| `event-pool` | 2 | Reuses the events GW2 creates and closes 2,500 times a second (#4) | `WINE_HEAT_EVPOOL` (off) | set, `1` (the *Event pool* row, on) | Release | On 0.1.x server calls fell from 86 to 4 ms a second, about 4 FPS at the busy spot (`heat-workers-2026-10-28.md`) | **keep.** A release default |
| `msync-fixes` | 3 | Three fixes to athei's msync without a switch: the wait-all's rollback, the timeout's deadline, and the wake after a put-back (#3, #22, #36) | none | shipped, always on | no | `UPSTREAM.md`'s second table. athei took four of my fixes with 0.4.0 | **keep.** Always on |
| `spins` | 1 | Spins before a lock sleep, an event sleep and a worker's semaphore sleep (#12) | `WINE_HEAT_SPIN` (off), `WINE_HEAT_SPIN_PAUSE` (32) | shipped, unset (the *Faster locks* row, off) | Release row, off. Dev list: the row on, and a line with `sem=5:game` | Natively a hot lock gains 39 to 45% at 12 to 15% more CPU (`heat-r7-2026-11-04.md`). On 0.1.x the `game` scope never spun on the game thread (`perf-review-2026-11-10.html`). On 0.2 the row waits for a session with more hits than misses, which hasn't happened (ADR 0005, #105) | **decide.** Never a default. The open case is #105's session |
| `sampler` | 3 | Samples the game thread by x86 address under FEX and reads FEX's counters (#13, #24, #28) | `WINE_HEAT_PROF` (off), `WINE_HEAT_PROF_HZ` (1000), `WINE_HEAT_PROF_THREADS` (game thread), `WINE_HEAT_PROF_FEX` (the engine's FEX) | shipped, unset. `WINE_HEAT_PROF_FEX` set while my FEX is in place | Release: `WINE_HEAT_PROF_FEX`. The app also looks for `WINE_HEAT_PROF` and `WINE_HEAT_CONTROL` in my own rules | The game thread's split by address (`game-thread-breakdown-2026-10-31.md`, on 0.1.x) and the FEX time question (`fex-jit-time-2026-11-13.md`). #175 and #176 want it again | **decide.** A diagnostic, never a default. It is the costliest one to carry: its FEX pin and layouts follow every engine (`MOVING.md`), and my FEX fork's `GW2Contract.h` exists for it. I'd keep it while #175 and #176 are open |
| `dlopen-fix` | 1 | ntdll's `dlopen`, `dlsym` and `dlclose` run with the server's signals blocked, so no thread is suspended inside dyld's lock, and `virtual_mutex` isn't held across the `dlopen` of a builtin's Unix library (#52) | none | shipped, always on | no | It fixed Steam's web helper hanging at its first start (#304, 0.2.0's notes) | **keep.** Always on |
| `upstream-picks` | 1 | Four upstream changes ahead of athei's next base (#55): athei's `5466044` turns AutoFill off for the input context, WineHQ's `dedead4` compiles the Rosetta workaround for x86_64 only, `53ba24d` yields with `swtch()`, and `48d67d3` makes a finite, nonzero sleep that isn't alertable a `nanosleep`, without the yield and `select()` before it. My yield timer moves into the `swtch()` branch with them (ADR 0022) | none | not in 0.2.1 | no | ADR 0021: the game thread yields 336 times a second at 0.31 µs, so `swtch()` can't save more than 0.1 ms a second. My trace of 2026-11-20 has a `com.apple.SafariPlatformSupport.Helper` next to the game, which AutoFill starts | **keep.** Always on. A pick the new base has goes empty at the next move. The timer's move doesn't, it stays with my hook |
| `macdrv-surface` | 1 | Cocoa gets a copy of each GDI window frame and no frame equal to the last (#14) | `WINE_MACDRV_SURFACE` (off) | shipped, unset (the *Launcher frames* row, off) | Release row, off. Dev list: the row on | On 0.1.x the same fix ended the launcher's blank frames and took `Gw2-64.exe` from 117 to 36% of a core (`launcher-fix-2026-10-28.md`). On 0.2 the row waits until I've looked at the launcher on three starts (#105) | **decide.** Never a default on 0.2. The open case is #105 |
| `macdrv-mouse-scale` | 1 | A factor on the GameController mouse's counts (#18) | `WINE_MACDRV_MOUSE_SCALE` (1) | set while *Mouse-look speed* isn't 1 and *Raw mouse-look* is on | Release | A player's setting | **keep.** A release row writes it |
| `macdrv-restore` | 1 | A restored window becomes the foreground window before it is activated (#31) | none | shipped, always on | no | #160. WineHQ has it as `ede017e2b24`, so it goes empty on a base that has that commit | **keep.** Always on |
| `mmdevapi-sessions` | 1, after the table's head | `IAudioSessionManager2`'s session notifications register and unregister with `S_OK` instead of `E_NOTIMPL`, so NAudio doesn't throw from its finalizer and end Blish HUD (jdrly/gw2-on-mac#387). In `mmdevapi.dll`, my one PE file (ADR 0025) | none | not in 0.2.1 | not yet: the app installs no PE file of mine | Blish HUD's test 1 on 0.2 died within 5 s on the finalizer's `NotImplementedException` (workspace `advisor-plans/2026-11-27-blish-hud/TEST1.md`) | **keep.** Always on, a fix without a switch (ADR 0006) |
| `kept-0.3.1-history` | 87 | athei's 34 commits of the 0.3.1 base and the 53 on them (50 mine, 3 of his fixes picked early), reachable under the merges | n/a | n/a | n/a | n/a | **keep as history.** Not replayed. Its content is in the 59 |
| `engine-move-merges` | 2 | `9bcdcdd3048` and `5e7bde79c2a`, both with `d377654f9d1`'s tree | n/a | n/a | n/a | n/a | **keep as history.** No tree effect |

Sum: 18 + 11 + 4 + 1 + 2 + 4 + 2 + 2 + 3 + 1 + 3 + 1 + 1 + 1 + 1 + 87 + 2 = 145, and the 3 commits
of the two dropped topics below make 148.

### Dropped

A dropped topic's commits stay in `gw2-on-mac`'s history. The commit that drops it takes the
topic's name in `series-topics`, so the topic's series commit adds and removes the same lines and
comes out empty, and a replay has nothing of it to pick. Its CI, test and doc lines go the same way
through the rules.

| Topic | Commits | Dropped | What it did | Why | The drop's commit |
|---|---|---|---|---|---|
| `macdrv-calls` | 2 | 2026-11-27, jdrly/gw2-on-mac#374 (ADR 0024) | Nothing went to the Cocoa main thread for an unchanged view or cursor clip, and a synchronous call woke its caller without a byte in the event pipe (#23, jdrly/gw2-on-mac#62). `WINE_MACDRV_VIEW`, `_CLIP`, `_CALL`, `_DRAIN` | With the switches off, the view update is 0.6 ms of the game thread a second, and `macdrv_ClipCursor` has no sample. The stall it was written for was never measured | takes `macdrv-calls`. Its line goes into `series-topics` when the series is next rebuilt |
| `win32u` | 1 | 2026-11-27, jdrly/gw2-on-mac#374 (ADR 0024) | My build of `win32u.so`: `SetCursor` answered an unchanged cursor from shared memory, and an empty `PeekMessage` could skip its yield (#20, jdrly/gw2-on-mac#61). `WINE_WIN32U_SETCURSOR`, `WINE_WIN32U_PEEK_YIELD` | With the switch off, the cursor requests show no sample in 20.5 s. It was a third file in every process, and the one the next base changes first | the same commit. The rule `dlls/win32u/* win32u` puts its `dlls/win32u` lines in this topic |

Left out together, the two replay clean onto `501796b0ed1`: 19 series commits clean and 2 left out,
tree `ffa1a39f6d22`, which is `94c8b311781` with the two topics' series commits reverted. The
drop's commit is that tree's source, plus the switch lines in `macdrv_main.c` and the tests, CI
and docs of both.

The research notes named in the table are in my workspace's `docs/research/`.

### For me to decide

No Wine topic is a proposed drop. Three are open: `spins` and `macdrv-surface` (both wait for
#105's session) and `sampler` (I'd keep it while #175 and #176 are open). `macdrv-calls` and
`win32u` were dropped on 2026-11-27.

### What a drop costs

Each line is the series replayed onto `501796b0ed1` with one topic's commits left out, on
2026-11-26:

| Topic left out | Result |
|---|---|
| `macdrv-surface`, `macdrv-calls`, `win32u`, `macdrv-mouse-scale` | clean, each alone |
| `upstream-picks` | clean. Case 6 of `tests/gw2/timers.c` stays in `tests` and expects the new sleep, so without the topic it fails: the old sleep path yields once more per sleep of 1 ms |
| `sampler` | one conflict hunk, in `thread.c`, in `heat-core` part 2 |
| `spins` | conflicts in `sampler` (`thread.c`), `heat-core` part 2 (`msync.c`, `sync.c`, `thread.c`) and `msync-fixes` part 2 (`msync.c`). The prototype prints 7 conflict blocks over the three, and some of them are an earlier topic's conflict seen again in a file it left marked |

A clean drop of a topic with a switch still leaves the switch's lines in `macdrv-options` or `heat-core`, which read a
variable nothing acts on. They go in a small commit after the drop. Whether the rest builds
without the topic is the first run's answer after the drop.

## The series branch

The commits that aren't merges and aren't in a skipped topic are cut by the rules below: a commit's
files under a rule's paths go to that rule's topic, whatever the commit's own topic, so the CI,
test and doc lines of a feature commit land in `ci`, `tests` and `docs`. Then the slices are
picked onto the base topic by topic, in the order below (greedy): per round and topic, a topic
takes its slices until one doesn't apply yet, and the rest wait for the next round. A topic that
needs another topic's lines first gets a second commit further down.

| # | Series commit | Commits it takes slices from |
|---|---|---|
| 1 | `ci` | 39 |
| 2 | `heat-core` | 9 |
| 3 | `job-wakes` | 2 |
| 4 | `msync-fixes` | 1 |
| 5 | `event-pool` | 3 |
| 6 | `spins` | 2 |
| 7 | `sampler` | 5 |
| 8 | `counters` | 6 |
| 9 | `upstream-picks` | 1 |
| 10 | `macdrv-options` | 3 |
| 11 | `macdrv-surface` | 1 |
| 12 | `macdrv-mouse-scale` | 1 |
| 13 | `macdrv-calls` | 2 |
| 14 | `macdrv-restore` | 1 |
| 15 | `win32u` | 1 |
| 16 | `tests` | 39 |
| 17 | `docs` | 15 |
| 18 | `heat-core` (part 2) | 5 |
| 19 | `msync-fixes` (part 2) | 2 |
| 20 | `dlopen-fix` | 1 |
| 21 | `job-wakes` (part 2) | 1 |

That is 18 topics: 17 of the table's, and `macdrv-options`, which holds `macdrv_main.c` so a Mac
driver topic can be left out without a conflict in the file that reads every `WINE_MACDRV_*`
switch. `engine-pins` has no commit of its own, because the rules put its lines in `ci`, `tests`,
`docs`, `sampler` and `counters`. Rows 18 to 21 apply only after the rows above them: they edit
lines in `msync.c`, `sync.c`, `server.c` and `loader.c` that other topics wrote first. Each series
commit's message names its topic in `Series-Topic:` and lists the `gw2-on-mac` commits it is made
of.

Onto athei's `arm64-patched` at `5466044bde5`, the tip of 2026-11-25, all 21 apply clean.

## Machine blocks

My workspace's replay script reads these blocks by their fence label. Paths are globs where `*`
matches `/` too, and the first rule that matches wins.

```series-topics
ce7fd783e3bb29a0153188b31dac337ef1a9cc17 ci
ce040beff191d80d1577918a4a66f040a0e5d38f ci
e81da113fde21fe4b9e700ed57ee7f669729681e job-wakes
b84839b8477ec9b54f6efc13b93829fcf67a7683 ci
7852d5024506916bff8d81cfcfeb680fae107308 ci
ba52d6f2f804f1af56050cd3a0b0b549ea51f8c2 tests
911a658d7ee6758e8fb4790cda842419fef3256e ci
c6060f6028ed9f156a3f4b7150ddc19a21f11c1c msync-fixes
abfdb233b7b5d7e1f6c15593a706faae0ee4ecc4 event-pool
0868c214fd5a64b6a66e4ccf7b59a29824d6303e ci
e605aa72e0ca886b63001cac2d7b86121d04bd88 spins
cd781e42de04e7a64f2fb777e48cec69641db29a sampler
03f83f03340a06a6b03ea2ec967c15987bc3163f ci
d2c66daaf10553865861349e920113bcb48f66af macdrv-surface
e28f8ece5ec67e502032c3084188c5c3593aea9c counters
cb454608a28a4167aac2d8674f027cf90d2de377 ci
7b2d968412d56f5796ec9d210875af4ae99bf22b ci
1243c26fee1397a03e0c0c10cd6e751bf7391814 macdrv-mouse-scale
4b08126887d223156725b4146b1725fe9b426867 win32u
9d27908697a319b9fd48bcb834d2fa4b20a0a4a9 counters
29eae08c7884e3c40b106f23296b4feec160dd1f msync-fixes
d49586f7bc6fb8649f19a5ff9e97bd0dff0d9e32 sampler
6860c47cf7a804d1f083a97d5d3e1ec7de1ed0d8 macdrv-calls
3ce1c9ff6120bd6aee517ae774cb7ad18996353f tests
e990b74e8ca34b8b673d5db73eade9b330a3a96e ci
45a316f14cdc8ddf8826e27d8901739de268d13d heat-core
2d4b1172acd7308341de2d025a8f151fe6c14aa2 sampler
7b566fdae7d068795880827d1cda24a1cf696cd6 tests
53095008e2385588cc1d58a1109b2aad8c416c22 tests
f8165d5df4351f065708733c79eb58135ac7fbce tests
2d4007c5cd6a419e427084180a7f6ef66d5909b8 macdrv-restore
1c3a4c523756d92d5c7644a9c36a5b577de1b426 tests
833419664604196d5b75ea083248b7314382193f ci
2e880b4f36019f33c0e86de3dfa3b24636ab9da7 ci
ddd71529d531e2095c4180c0bf95f317275d7f52 counters
a986cf29d9186ca6f2ddb7997b9756dfa0c83391 msync-fixes
2622db762cced829038a8b8e22484e9d624f49df tests
832bad20698f9b255cec6a0b6992381e24e4b24c heat-core
6cb27a4e37f015bbca108521b0a01e1e53e58a3f counters
aab554f0c2eb4d2ab429cf772e6265ec1cccc045 docs
6ad945df4ac0b6685a9debacc949273c12dfb74e tests
41ce92970dc3374f77759e2031429644cde50af3 ci
056c2843382da3ca48b1bed24ee51b26733c332a ci
4c5b18099596e590dc701f2ddd6f2e5f853daad8 ci
915a55ea7bf5e1d91c21f94f82c0222122c76820 tests
3a891dc144c66b8bf0c30d919ccae69750f7fbbb ci
4e625a2756e7763ebf59b469d46411e2ad452d16 docs
af2deec54242e57516593f116f89be02839e6eb9 job-wakes
2539e95c96850e26cc61f601959e3d3499d53e68 ci
36a40c29fbd283acc74397ab7286abd441dcd138 docs
f709c4cc012aac39c068712811495794e1b5e665 engine-pins
1d7f76939208e0d7f6e7c4b19f5659f2f171bca0 macdrv-calls
f61569d1fa5fd946747a692aa1ce231c9a265713 event-pool
fee426fb73860a6d1661efeb58821dcbb1f80d31 ci
d377654f9d1fce4e14d8045bac641487e67e57dc docs
82619942fd5a16c4c171c0e2f8a857566443710f kept-0.3.1-history
c80227811cd7d066f2545d4c35e0dcec7c964a6b kept-0.3.1-history
99e5377924f2af7b0d8511f207a0ed108e7e0512 kept-0.3.1-history
684731797b1d92a1e635fe22ce76a3c73ccac6e4 kept-0.3.1-history
2801791df0ecdb84475ebeedd26ce049ebad2919 kept-0.3.1-history
6fc9a25f932551f415936d5b65ce0af7f1594163 kept-0.3.1-history
a9ffb3d78947d5f852e56275cac08f6d7b4a86ec kept-0.3.1-history
729b21e4f6a90a5b98928897749b5dbfc08b49dd kept-0.3.1-history
d77bb4c3c36ff8050886ab09e4dfed1169494eb4 kept-0.3.1-history
0ae2cd6ac966b363b4ec83ee1d3d8428845c8717 kept-0.3.1-history
f6a80b4920fb32b5623c3e188d49c1f43eb9f06a kept-0.3.1-history
82f5a15e1e3a3db54809a00fd1c828af91fdf641 kept-0.3.1-history
f1f66d9fc741f5bab57dab04ae7ec542f4db9bff kept-0.3.1-history
7e2f29a0abe23be24e9b15275335a4471450026f kept-0.3.1-history
105dd1b1197b08a97571444573bb385aa627ef57 kept-0.3.1-history
fb1d0214505382ea10c95be79b8d7317b1a095eb kept-0.3.1-history
da3e6ec92bc14825ab8092726226b0a5d13216cc kept-0.3.1-history
7ef93f79326f2caade6b16b0d0f20ef2abc4ab43 kept-0.3.1-history
1b8e611a25bb4d6b2186fb2a13346ae1de0374e5 kept-0.3.1-history
5ac13b02c170107bb26b1b11817b006be397223e kept-0.3.1-history
e75ab5b88b8aaf9422451932ec58c6a5104ab926 kept-0.3.1-history
e77f838d6e9a383281e132e055350afda9ddcc1b kept-0.3.1-history
4002a640a8809633cd12d0ca21a902795abd320d kept-0.3.1-history
f60b737750a82d1a21f7ad2ea1daa5981e6cc38a kept-0.3.1-history
f41d34f8021a5cfa3cf61b16fa0d0b05013160b0 kept-0.3.1-history
88912bf8fce7ddf5145852f1c9a24ca533fb2683 kept-0.3.1-history
8fcdfea7d0e675280a7d52b749ae85244ca2b300 kept-0.3.1-history
24bcb90aeaa25424323928d2a1e91c9a4240acb4 kept-0.3.1-history
2355e407038e798096474e146103543e5867ca6d kept-0.3.1-history
5e4fa0028da5305875ead2621651f6cd5d0b94ac kept-0.3.1-history
25aa72fde3d8a0f46c94a1579982bd6a97b4a240 kept-0.3.1-history
05740a91e3a32009a5975bf5864a123b5c197058 kept-0.3.1-history
df9a40146ec76bf682dbec33bc9495a5b9bca55c kept-0.3.1-history
53def89293adfe1a62c43ef46abe80a536e9ca49 kept-0.3.1-history
2fcc7139e340a4061bfd7aff9b02f4b2d5b0479b kept-0.3.1-history
3b0cfe652d00bf64608713ce6e5a4ad1714e0501 kept-0.3.1-history
21ec9b8a8805bc9c4e9243db512f10f54031e5b8 kept-0.3.1-history
e453c3afe8257c3658685532b91bd2f80e460d06 kept-0.3.1-history
8bfe1cee48e32b43d4cd2dd4a63c1164ba83045c kept-0.3.1-history
e3334049c55fb9fde7cc4c10c264d46e504b1d06 kept-0.3.1-history
3a201be7b1cd6e6fd6136278807e628b3341473c kept-0.3.1-history
f6097722b998ed3ffd130cd17132fc19835799ad kept-0.3.1-history
cba04c550cfe180acacca31ce7102f0e7e9e6e0c kept-0.3.1-history
683e2f123e4b36dcefeacb9bfd701865fbb0b3ef kept-0.3.1-history
361daafb1aaac427aaa058bfd20bd8a17e957bab kept-0.3.1-history
86f2dda8e282e59074bc2151bd5297e6bb831a48 kept-0.3.1-history
e53c4ea6385bb4122f42d2d1fdcc56e84a56be3e kept-0.3.1-history
1250bbe9817fbe6252d51368d7997be73522ae08 kept-0.3.1-history
3b95f7f2dc036a6fd965005d022c9f52c0311c0b kept-0.3.1-history
d831f808236c80a55da41ca1352d6b48e1af1eab kept-0.3.1-history
b496e499a640767d4c02dcd5f1758f312bd024b1 kept-0.3.1-history
bdf36d3cab07da0cd3ff2dc6284a8072dc79a05b kept-0.3.1-history
b7d314dffc333ea57d68214d2c2b36f2b6dd9eb0 kept-0.3.1-history
8752da04c30d76e489adee08fb36707632c7504b kept-0.3.1-history
41ca59151dc948966ed465519d7584c6fdb3d57d kept-0.3.1-history
29db621e1877b50d5c69fb6af205b0b42d14f9ab kept-0.3.1-history
472d77743682adff9cad62abee48d8b9154b3a04 kept-0.3.1-history
ebeb45098999e1c8a2492c5f5b4a2e7a1d609753 kept-0.3.1-history
e4b3c1e3cd69109999b800fb6eb3aec9cf3c4ef2 kept-0.3.1-history
d854fb294ae4e7ab130ffa4ba4e97f8d42b32004 kept-0.3.1-history
8e8931b4244b75379bab7c31122233147062448d kept-0.3.1-history
198d884e2f8509fab67db99a529c5cfacbc14018 kept-0.3.1-history
6d2623ab5f035f5955d01d41ad96d9b858c0cab6 kept-0.3.1-history
f4c7da17451dee254f5850330ce75efa8c1935cb kept-0.3.1-history
02f49072169eaac67deec749324156a446d33d24 kept-0.3.1-history
7d97ef7c0e50a486c9e096becc5c7787454d31a6 kept-0.3.1-history
8384433b85624e749f171665832d99dd0d569e48 kept-0.3.1-history
3fed5c1791bc05aa677a7fe9ee1edc0aeded87bc kept-0.3.1-history
c57a43e6869807ce07c825c0eac389765c7c3231 kept-0.3.1-history
f2d809d95a1992fe8a4162f6a03df063845e563c kept-0.3.1-history
02e36e1b0d47cfb780a81a2a7348bfdded713b5f kept-0.3.1-history
7c1ab3e1cb5ecd8a37609035bb788f764c796bf2 kept-0.3.1-history
30c65fd5a6dcb19e76f2c1ae73de70c3bd782938 kept-0.3.1-history
15172b2c0a8f91eef4a1859e7015b4d9dbf8e39d kept-0.3.1-history
08ecd6aad550c8160a01519ad76a88a90bd56ce5 kept-0.3.1-history
d839d05b1f68ccf6beb14b0ba436ae342a37d91a kept-0.3.1-history
64ad314cbc0aaf73b0b72148cf58f17c59ace4a2 kept-0.3.1-history
73344e9b1f6f306a35cd03752994650d30ced9be kept-0.3.1-history
c2b78d4953461cc0b625bfe8ce7108495c620758 kept-0.3.1-history
de45517d729811de0b9565eb7e5d55f5533c250c kept-0.3.1-history
82efd2c7e136b47677d03cc5fe9cf72c9b803653 kept-0.3.1-history
1f7ef153b2b884ef2844bdfce927f19bcf8c3ce0 kept-0.3.1-history
b094e4ac3b29cb175ee3c33eed2223d2ea7caf97 kept-0.3.1-history
cdfb47e84d852d6ebe4a4bb95ff63ace94da863a kept-0.3.1-history
cc8f1a453d1bca0147cc34e39d9e77b5c3b1afe1 kept-0.3.1-history
07a13e9418f6a881de21ffa7878bd0faf94a4f70 kept-0.3.1-history
4e55fb228c016cda5cd0c3bdd62d4aca8348a6d3 kept-0.3.1-history
9bcdcdd304850dd4a6ad06472d71769ac31c5275 engine-move-merges
5e7bde79c2a5eedd4e8bd14d7f5ce83d1863a324 engine-move-merges
43e22bdc25bb9bb00a16b9e5685e98836de080f3 dlopen-fix
b4937621f4e5a045955074d0d64e163569556a2c tests
63f5aa66d5d14b07e390f962b7089693749436ab tests
c8d41f337ee063ff063697d7c23d4e1a191292b0 upstream-picks
f774ec8f6fb111ec0150df18ca28f8a2625b2efe docs
4722e8f3981770d8c9d30f1dad55a1fb69211845 ci
75369d439255bd34628e6208bab5ec254e82b832 counters
94c8b311781a3f35aae33bc5574a018986a4bb2f spins
```

```series-rules
.github/gw2/*.md docs
AGENTS.md docs
docs/* docs
.github/* ci
tests/gw2/* tests
dlls/ntdll/unix/heat.c heat-core
dlls/ntdll/unix/heat.h heat-core
dlls/ntdll/unix/heat_private.h heat-core
dlls/ntdll/Makefile.in heat-core
dlls/ntdll/unix/heatwake.c job-wakes
dlls/ntdll/unix/heatpool.c event-pool
dlls/ntdll/unix/heatspin.c spins
dlls/ntdll/unix/heatprof.[ch] sampler
dlls/ntdll/unix/heatfex.h sampler
dlls/ntdll/unix/heatreq.[ch] counters
dlls/ntdll/unix/heatdl.h dlopen-fix
dlls/win32u/* win32u
dlls/winemac.drv/macdrv_main.c macdrv-options
dlls/mmdevapi/* mmdevapi-sessions
```

```series-order
mode greedy
ci
heat-core
job-wakes
msync-fixes
event-pool
spins
sampler
counters
dlopen-fix
upstream-picks
macdrv-options
macdrv-surface
macdrv-mouse-scale
macdrv-calls
macdrv-restore
win32u
mmdevapi-sessions
tests
docs
```

```series-skip
kept-0.3.1-history
engine-move-merges
```

```series-titles
ci CI: The GW2 workflow builds my two files on the engine's commit and checks them against the engine's
heat-core Heat core: The switch parser, the process filter and the hooks' shared state
job-wakes Job wakes: A semaphore release makes fewer wake calls, and can skip them without a waiter
msync-fixes msync: My fixes to athei's msync, without a switch
event-pool Event pool: Reuse the events GW2 creates and closes 2,500 times a second
spins Fast locks: Spin before lock, event and worker semaphore sleeps
sampler Sampler: The game thread by x86 address under FEX, with FEX's counters
counters Counts: Per-thread counters and the game thread's server requests by name
upstream-picks Upstream picks: athei's AutoFill fix and WineHQ's yield, sleep and Rosetta changes ahead of the next base
macdrv-options Mac driver: The WINE_MACDRV_* switches, read in one place
macdrv-surface Launcher: Cocoa gets a copy of each frame, and no frame equal to the last
macdrv-mouse-scale Mac driver: A speed factor for the GameController mouse's counts
macdrv-calls Mac driver: Nothing goes to the Cocoa main thread for an unchanged view or cursor clip
macdrv-restore Mac driver: A restored window becomes the foreground window before it is activated
win32u win32u: My build of win32u.so, with switches for SetCursor from shared memory and an empty PeekMessage without a yield
tests Tests: The programs and the matrix under tests/gw2
docs Docs: AGENTS.md, the switch and hook lists, MOVING.md, SERIES.md and the ADRs
dlopen-fix ntdll: No suspend inside dyld's lock, and no virtual_mutex across dlopen
mmdevapi-sessions mmdevapi: Session notifications register and unregister instead of failing
```

## Rules

- A commit needs no line in `series-topics` when the rules place every path it changes. That
  covers the commit that adds this file, and later commits that only touch CI, tests or docs.
- Any other commit gets its line when the series is next rebuilt, and the build refuses one
  without it.
- The series branch is rebuilt under a new date, never rewritten.
- `gw2-on-mac` is never rewritten.
