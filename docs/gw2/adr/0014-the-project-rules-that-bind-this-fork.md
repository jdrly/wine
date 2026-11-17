# The project's rules that bind this fork

Recorded 2026-11-17. Each rule has its own date in the workspace ADR it points to. Status: accepted.

This fork is one of four repositories of GW2 on Mac, and some of my rules hold across all of them.
Their full records are ADRs in the project's workspace, which is not public. So that a reader of
this fork finds them, each rule is stated here in a sentence, with the workspace ADR that holds the
reasons.

## Consequences

- **Nothing caps the game below 60 fps.** Nobody proposes a frame cap below 60, or anything that
  trades frame rate for heat or power, and no dev build turns one on. Heat and power come down by
  removing work from each frame. Workspace ADR `0001-no-frame-cap-below-60`.
- **No D3DMetal comparison runs.** A graphics bug belongs to my DXMT fork and is diagnosed there,
  not by a D3DMetal run. Workspace ADR `0002-no-d3dmetal-comparisons`.
- **Changes are validated in my normal play.** A switch is judged from the heat logs and the session
  report of sessions I play anyway, never from a protocol of timed minutes. Workspace ADR
  `0003-changes-are-validated-in-my-normal-play`.
- **A regression gets its root cause, not a workaround.** When 0.2 is worse than 0.1.x, the
  difference between the stacks is the bug. A Mac driver change that only hides it is not merged.
  Workspace ADR `0011-a-regression-gets-its-root-cause-not-a-workaround`.
- **Nothing goes upstream.** No pull request, issue, comment or mail goes to athei's repositories or
  to WineHQ. That one is in this fork's own ADR 0001.
- **Runner time.** One run at a time, one push per pull request after its review, and each push on
  the dispatcher's go. A test runs only as long as its assertions need. Workspace ADR
  `0010-one-run-at-a-time-and-each-push-needs-a-go`.
- **No reruns.** A job that failed a test or a build is never rerun. A new push starts a new run.
  One rerun is allowed for a job that a GitHub outage killed, that never got a runner, or that
  failed only in a GitHub-provided action's network call. Workspace ADRs `0006` to `0009`.
- The workspace has five more ADRs that touch this repository: everything is written in my first
  person (`0004`), checks run on GitHub's runners (`0005`), my builds are named arm64 (`0012`), the
  weekly upstream watch (`0013`), and design decisions are recorded as ADRs (`0014`).

## Alternatives I rejected

- **Keeping these rules only in the workspace.** The workspace has no remote, so a reader of this
  repository can't open it. The sentence here is the rule, and the workspace ADR is the full record
  with its reasons.
- **A full copy of each ADR in every repository.** I asked for a short ADR that states each rule in
  a sentence and points to the workspace.

## Evidence

- GW2 on Mac workspace `docs/adr/0001`, `0002`, `0003`, `0006` to `0011`.
- My answer of 2026-11-17 on the form of the ADR backfill, relayed by the dispatcher
  (`advisor-plans/2026-11-17-adr-backfill/README.md` in the workspace).
- This fork's ADRs 0001, 0012 and 0013.
