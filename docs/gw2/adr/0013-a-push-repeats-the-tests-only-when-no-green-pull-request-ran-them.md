# A push repeats the tests only when no green pull request ran them on its tree

Decided 2026-11-12, guarded 2026-11-16. Status: superseded on 2026-11-20 by jdrly/gw2-on-mac #323:
every push runs every test (the last consequence).

On 2026-11-12 GitHub disabled this fork's workflows for wasted compute, after a night of pushes that
cancelled runs in progress, three test jobs at once and a matrix of busy producers. One of my rules
from that morning: a push run doesn't repeat what the pull request's run of the same tree proved. A
push to `gw2-on-mac` then ran `smoke.exe` in place of the matrix, built, compared and published, in
4 minutes instead of about 13. That rule took something for granted: that the tree was the head of a
green, up-to-date pull request. Publish only said afterwards which run had tested it. A stale pull
request merged into a moved branch would have published files that only the smoke test saw. Since
2026-11-16 the skip is earned. The `tests` job looks for a green pull request run on the same tree
and runs every test when it finds none.

## Consequences

- The `tests` job starts with "Whether a green pull request tested this tree": the commit's merged
  pull requests, a head with the same tree, a successful `pull_request` run of `gw2.yml` on that
  head. Only a match writes `FULL=0`.
- A pull request, no match, and a failed lookup all run the full set. The step can't skip the tests
  unless it finds a green run on the same tree. A lookup that misses costs about 28 runner minutes
  and nothing worse.
- The first guarded push ran the smoke test only, said "pull request #33's green run tested this
  tree", and took 6 minutes.
- The files a dev build fetches from `arm64-builds` were therefore either tested as that tree by a
  pull request, or tested by the push itself.
- My DXMT fork got the same rule with the guard built in (its ADR 0013).
- A pull request's run still takes its workflow from the merge commit and its tests from the head
  (jdrly/gw2-on-mac #149, open). Its scripts under `.github/gw2` come from the workflow's commit
  since PR #9. Settled on 2026-11-17 by ADR 0017: a head that is behind stops at once and rebases
  first.
- Tightened on 2026-11-17 by jdrly/wine PR #46 (jdrly/gw2-on-mac #241). The lookup moved into the
  build job's step "Which tests this run owes", and a match also needs the pushed commit's parent
  to be an ancestor of the pull request's head. A pull request's run tests GitHub's merge of its
  head into `gw2-on-mac`, so a commit that came to `gw2-on-mac` and was reverted while the pull
  request waited could leave a squashed tree equal to the head's that no run tested. Publish's
  summary makes the same check.

- Since jdrly/gw2-on-mac #323 (2026-11-20) every push to `gw2-on-mac` runs every test, and the
  lookup is gone from the build job and from Publish. #323 lets a pull request that changes only
  Markdown skip its build and tests, so its green run would have vouched for a tree it never
  built, and the merge push of a change to the CI would have run only the smoke test. A push now
  costs a pull request's full run, about 12 minutes of wall clock, each time something merges.

## Alternatives I rejected

- **The full matrix on every push.** That was the state GitHub disabled the fork in.
- **The unguarded skip.** It held in 21 of 21 pushes, by habit and not by a check. Neither fork has
  branch protection that would make it hold.

## Evidence

- jdrly/wine PR #11 (`683e2f1`, the skip), PR #33 (`f2d809d`, the guard), jdrly/gw2-on-mac #214,
  push run 43879956359.
- GW2 on Mac workspace: `plans/night/2026-11-11-plan.md`, 2026-11-12 02:25 ("Rules from now on") and
  03:00, 2026-11-16 14:55 and 15:00; `advisor-plans/2026-11-16-forks/vetted-B-wine-ci-docs.md`
  (TEST-12) and `plans/025-wine-ci-enforce-push-guard.md`.
- The dispatch project file `gw2-on-mac.md`, "Runner time".
