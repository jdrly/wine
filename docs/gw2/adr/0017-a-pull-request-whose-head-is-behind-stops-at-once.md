# A pull request whose head is behind stops at once

Decided 2026-11-17. Status: accepted.

A pull request's run takes the workflow and `.github/gw2` from GitHub's merge commit, and builds
and tests the head. When the head is behind `gw2-on-mac`, new workflow steps run against old tests
(jdrly/gw2-on-mac #149). Run 43370004985 went red after 13 minutes that way, at a step for
`leftover.exe` that the head didn't have. Even when such a run is green, it's wasted: the merge has
another tree than the head, so ADR 0013's guard finds no green run of that tree and the push runs
every test again.

So the build job's step "The head has gw2-on-mac's tip" compares the merge commit's tree with the
head's. If they differ, the run fails in its first minute and asks for a rebase. The check is on
trees, not ancestry. A head that has the same changes as `gw2-on-mac`'s tip without being based on
it passes, and that's fine: its run tests the tree the merge will have. The same pull request
narrowed which changes owe tests, a second decision with its own record in ADR 0015.

## Consequences

- A branch that falls behind `gw2-on-mac` while it waits for its push has to be rebased first. A
  push after the rebase is a second push and needs its own go. The run that stops costs about a
  minute, not 13 to 31.
- A re-run of an old run keeps its old merge commit, so the step passes there even if
  `gw2-on-mac` has moved since. ADR 0013's guard still catches that at the push.
- This settles #149, which ADR 0013 lists as open, as "rebase first".

## Alternatives I rejected

- **The merge commit's tests against the head's files.** A test that came to `gw2-on-mac` with a
  fix would run against a head without the fix, and fail.
- **A warning instead of a failure for a head that is behind.** The run would go on testing a
  mixed tree, and the push would run every test again anyway. If I ever want it, the step needs
  `::warning::` for `::error::` and no `exit 1`.

## Evidence

- jdrly/gw2-on-mac #149 and #228, run 43370004985, jdrly/wine PR #42 (`c2b78d49534`).
- GW2 on Mac workspace: `advisor-plans/2026-11-17-ci/REVIEW.md`, "The head against the merge ref",
  and `advisor-plans/2026-11-17-ci/007-wine-ci-selection-and-behind-head.md`.
- This ADR was the second half of ADR 0015 when PR #42 landed it. I split the two when the ADRs
  0001 to 0014 landed, so each decision has its own record.
