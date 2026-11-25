# My decisions for this fork

Each file records one decision of mine about this fork: what I decided, why, what follows from it,
what I rejected and where the record is. Issue numbers are in jdrly/gw2-on-mac unless a line names
another repository. Evidence lines that name my GW2 on Mac workspace are pointers to files that
aren't public.

The order is by number, not by date. 0015 and 0016 were decided on 2026-11-17 and landed first.
0001 to 0014 record older decisions and landed after them, backfilled from my notes, the night logs
and the issue threads. 0017 was the second half of 0015, split out when the backfill landed. A new
decision takes the next free number.

| ADR | Decided | Decision |
|---|---|---|
| [0001](0001-three-drop-in-files-on-athei-mach9-wine.md) | 2026-11-11 | The fork starts at Mach9 0.3.1's commit and builds three drop-in files. I don't build an engine. Nothing goes to athei or WineHQ. |
| [0002](0002-every-build-is-compared-with-the-engine-file-object-by-object.md) | 2026-11-11 | The baseline is held to the engine's file, and a patched build to the baseline per object through relocations. |
| [0003](0003-the-fork-builds-on-xcode-27-with-a-structural-gate.md) | 2026-11-12 | `macos-26` runners stopped coming, so the build moved to Xcode 27 and the byte gate became a structural one. Narrows 0002. |
| [0004](0004-every-change-is-off-unless-its-switch-is-set.md) | 2026-11-11 | Each change of behaviour is off unless its variable is set, and the heat switches act in `Gw2-64.exe` only. |
| [0005](0005-the-app-carries-the-heat-defaults-on-0.2.md) | 2026-11-11 and 2026-11-12 | The app's rows hold the defaults on 0.2: job wakes and the event pool on, fast locks and launcher frames off until I've played them. |
| [0006](0006-a-fix-to-athei-or-wine-code-has-no-switch.md) | 2026-11-11, widened 2026-11-13 | A fix to athei's or Wine's own behaviour is always on and acts in every process. The `wine` setting is its off switch. |
| [0007](0007-a-wait-all-that-puts-an-object-back-wakes-its-waiters.md) | 2026-11-16 | athei's put-back made no wake. Mine does, always on. |
| [0008](0008-the-heat-dump-is-written-to-a-temp-file-and-renamed.md) | 2026-11-16 | The dumper writes `heat-<pid>.log.tmp` and renames it, so no reader finds a cut log. |
| [0009](0009-mutexall-stops-at-a-count-only-under-my-ntdll.md) | 2026-11-16 | The engine's runs keep 20 s to show the bug. Mine stop at 20,000 wait-alls. |
| [0010](0010-msync-sleep-timing-stays-at-the-call-sites.md) | 2026-11-16 | Moving the timing into `ulock_wait` would change the wake order under `skip`, so it stays. |
| [0011](0011-the-move-to-wine-11.22-waits-for-a-mach9-release.md) | 2026-11-13, planned 2026-11-16 | I take athei's rebase as the base. The trial runs on his first Mach9 release on 11.22, the move on his second. |
| [0012](0012-a-pull-request-runs-only-the-tests-its-change-can-reach.md) | 2026-11-16 | Tests are selected by the files a change can reach. No early stop at the first red block. Narrowed by 0015 and 0016. |
| [0013](0013-a-push-repeats-the-tests-only-when-no-green-pull-request-ran-them.md) | 2026-11-12, guarded 2026-11-16 | A push skips the matrix only when a green pull request ran on the same tree. |
| [0014](0014-the-project-rules-that-bind-this-fork.md) | recorded 2026-11-17 | The project-wide rules in a sentence each, pointing to the workspace ADRs. |
| [0015](0015-a-pull-request-that-changes-only-docs-skips-the-tests.md) | 2026-11-17 | Only a change that can reach a test sets `TESTS`, so a pull request that changes only docs runs the smoke test. |
| [0016](0016-the-tests-run-as-three-lanes.md) | 2026-11-17 | The tests run as three lanes side by side, and block 7 goes with the window steps. |
| [0017](0017-a-pull-request-whose-head-is-behind-stops-at-once.md) | 2026-11-17 | A pull request whose merge tree differs from its head's fails in its first minute and asks for a rebase. |
| [0018](0018-the-wake-skip-decides-by-msync-reference-count-alone.md) | 2026-11-17 | The wake skip decides by msync's reference count and my own sleepers, without the private mark. The put-back never skips. |
| [0019](0019-a-pull-request-configures-from-kept-answers.md) | 2026-11-17 | A pull request's configure starts from `config.cache` kept under a key that names its inputs. A push asks everything again, fails if its answers differ, and fills the cache. |
| [0020](0020-no-suspend-inside-dyld-and-no-virtual-mutex-across-dlopen.md) | 2026-11-19 | ntdll's own `dlopen`, `dlsym` and `dlclose` of unix libs run with the server's signals blocked, and `load_builtin_unixlib` drops `virtual_mutex` for `dlopen`. Steam's web helper deadlocked on both. |
| [0021](0021-upstream-picks-ahead-of-the-engine-have-no-switch.md) | 2026-11-26 | athei's AutoFill fix and WineHQ's yield, sleep and Rosetta changes come in as unchanged picks, always on, like the fixes of 0006. |
| [0022](0022-the-yield-timer-wraps-swtch-in-sync-c.md) | 2026-11-26 | My yield timer moves into the macOS branch around `swtch()`, in `sync.c`, and `yield` stops counting sleeps above 0. |
