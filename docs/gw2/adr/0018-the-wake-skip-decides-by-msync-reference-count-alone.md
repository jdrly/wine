# The wake skip decides by msync's reference count alone

Decided 2026-11-17. Status: accepted.

With `WINE_HEAT_WAKE=skip` a semaphore release makes no wake call when nobody can be asleep on
the semaphore. Until now my code asked three questions after the count changed: is the releasing
handle marked private, is the object's reference count in msync's shared memory at most 2, and
does this process count a sleeper on the slot. The mark cost four hooks in upstream files, one of
them a rewrite of `NtReleaseSemaphore`, and a thread-local store and clear on every release.

The reference count already answers what the mark answered. A thread of mine that sleeps on a
semaphore is counted by slot in `heat_waiters`. A thread of another process reaches the slot only
through `get_inproc_sync_fd`, and the server raises the count before it replies. A process keeps
its reference while any of its threads uses the object, even after the handle was closed. So I
dropped the mark, and the skip decides by the count and this process's sleepers alone.

There is one state the count doesn't see, and I accept it. No handle to the semaphore is left in
any process, a thread of another process is still asleep on it, and a thread of mine is still
inside a release or a SignalObjectAndWait on it. The server's reference went with the last
handle, so the count reads 2, and my release makes no wake call. The lost wake hits the other
process (Steam, the overlay, `CefHost.exe`), never a thread of `Gw2-64.exe`, whose sleepers I
count. On Windows the same close against the same release already loses that wake when the close
wins, and a wait on a handle that was closed is undefined. The mark didn't close this state
either: it covered the semaphores it saw as shared, and for a handle another process duplicated
out of mine it never knew.

The wait-all's put-back (ADR 0007) has no Windows counterpart, and a wait-all holds its entries
for its whole wait, so it would be the widest way into that state. It stays out of the skip: it
passes 0 to `signal_all` and wakes every waiter, as msync does without my switches.

## Consequences

- `NtCreateSemaphore` and `NtReleaseSemaphore` have athei's lines again.
  `NtSetInformationObject` keeps only the event pool's hook, and `NtDuplicateObject` only the
  close count and the event pool's hook.
  `.github/gw2/UPSTREAM.md` has two rows fewer. `heat_handles` has one free bit, value 1.
- Every semaphore released in `Gw2-64.exe` can skip now, not only those `NtCreateSemaphore` made
  there without a name: Steam's, CEF's, DXMT's and an addon's too, when nothing else holds them.
  A release through `NtSignalAndWaitForSingleObject` can skip too. The mark never let it.
- A semaphore this process holds through two handles, both used, reads 3 and never skips. Before,
  the mark refused it.
- The put-back wakes every waiter in every `WINE_HEAT_WAKE` mode. With `one` it made one wake-one
  call until now. It runs only when a wait-all loses a race.
- A new counter, `wake_shared`, counts the releases the skip refused because the count was above
  2. It is the last counter on each line of `heat-<pid>.log`, written as `wake_shared <n>` like
  the others. A session with the overlay and the in-game browser tells me whether any semaphore
  GW2 releases has a second holder at all. `tests/gw2/wakes.c` case 16 checks it with a semaphore
  that a second handle of the test has looked up.
- If athei changes when the server takes or drops an msync reference, or the cache in `sync.c`
  stops holding a reference through a wait or a release, the skip is wrong. A rebase reads
  `release_inproc_sync`, `inproc_wait`, `inproc_release_semaphore` and `get_obj_inproc_sync`
  against `heatwake.c`'s header.

## Alternatives I rejected

- **Keeping the mark next to the count.** It narrows the state above for the semaphores it sees,
  at the price of four upstream hooks and a store on every release, and a release descheduled
  between its two loads still loses the same wake.
- **Letting the put-back skip.** It is the one way into the state that Windows doesn't have, and
  skipping there saves nothing: put-backs are rare.
- **Measuring first, with a second counter of the releases the mark refused and the count alone
  would skip.** That needs a session before the change. `wake_shared` gives the half that matters
  after it.

## Evidence

- jdrly/gw2-on-mac #249, the issue where I accepted the state above.
- GW2 on Mac workspace: `advisor-plans/2026-11-16-forks/plans/034-wine-drop-private-handle-mark.md`
  and its check, `advisor-plans/2026-11-17-wake-skip/VERDICT.md`, with the request logs of the
  dev20 session of 2026-11-17 (no `open_semaphore` in any of them).
