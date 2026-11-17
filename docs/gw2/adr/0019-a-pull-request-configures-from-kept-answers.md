# A pull request configures from kept answers, and a push checks them

Decided 2026-11-17. Status: accepted.

Configure is the longest step of the build job: 74 s in the median of 22 runs, 53 to 88. Most of
that is Wine's configure script asking the compiler about 450 questions, and on the same image the
answers don't change from run to run. Autoconf keeps those answers in a file of its own,
`config.cache`, and `configure -C` reads and writes it.

So `wine.zsh configure` runs with `-C`, and the build job keeps `config.cache` in GitHub's cache. A
pull request's configure starts from the kept file and skips the questions. Each run still writes
its own `config.h` and Makefiles and compiles every object. I expect Configure to drop from about
74 s to about 20 on a pull request. That number is inferred, not measured.

In ADR 0012 I rejected caching the base build, because the compare needs "one run, one toolchain".
This is not that: no object, no Makefile and no `config.h` leaves the run. Only the answers do. A
wrong answer from a stale file would still be worse than a slow configure, so two guards hold:

1. **The key names everything the answers depend on**: the image and its macOS build, Xcode, the
   SDK, clang, the versions of Homebrew's five packages, llvm-mingw, the deployment target, the
   engine's commit, and `wine.zsh` by its hash. If any of them changes, the key is new and
   configure asks everything again.
2. **A push never reads the kept answers.** Its configure asks everything, as before. Then its
   answers are compared with the kept ones, and the step fails if they differ. So what I publish
   is always configured in its own run, and a kept file that went stale without its key changing
   shows on the next push. A push is also what fills the cache: a pull request can read the caches
   of `gw2-on-mac`, not those of other pull requests.

## Consequences

- A stale hit costs one pull request tested on a `config.h` that differs from the published one
  in whatever the answer changed. That run still counts as the green run that lets the push skip
  the tests (ADR 0013). The push builds from fresh answers, and its `diff` fails, so it goes red
  rather than publishing.
- After such a red push, every later push with the same key fails the same way, because a cache
  entry can't be overwritten. The way out is to delete the entry under Actions, Caches, or to put
  the missed input into the key, which changes it.
- Anything new that configure's answers depend on goes into the key: a new Homebrew package in the
  install step, a new variable that `wine.zsh` passes to configure from the environment. Arguments
  inside `wine.zsh` are covered by its hash.
- A new runner image changes the key. The first pull request after it configures from nothing, and
  the first push after it fills the cache again. The same holds when GitHub drops a cache that
  nothing read for 7 days, and at the move to Wine 11.22 (ADR 0011), which changes the engine's
  commit.
- If a pull request's Configure takes more than 45 s with kept answers, the saving isn't worth the
  three steps, and I revert this.

## Alternatives I rejected

- **Caching the base build (WINE-11).** Rejected in ADR 0012 for the compare's sake, and nothing
  here changes that.
- **A push that also reads the kept answers.** It would save the same seconds on a push, but then
  nothing would ever check the kept file against fresh answers, and a stale answer could reach a
  published file.
- **A cache that pull requests fill.** GitHub doesn't let one pull request read another's caches,
  so only the branch's entry, written by a push, helps the next pull request.

## Evidence

- jdrly/gw2-on-mac #253.
- GW2 on Mac workspace: `advisor-plans/2026-11-17-ci/011-wine-ci-configure-answers.md`, with the
  step times of run 43957907232 (Configure 88 s: 4 s `wine.zsh prepare`, 10 s `config.status` and
  makedep, the rest the compile checks).
