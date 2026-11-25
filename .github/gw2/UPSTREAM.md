# My lines in upstream files

Every line of mine in a file that athei's Wine has is a possible conflict when I rebase onto a
newer engine, so a rebase starts here, and so does `MOVING.md`. A change that adds or removes a
line of mine in an upstream file changes its row in this file.

My own files (`dlls/ntdll/unix/heat*`, `dlls/winemac.drv/cocoa_calls.h`, `gcmouse_scale.h`,
`surface_frame.h`) aren't listed. Issue numbers are in jdrly/gw2-on-mac. To list the hunks:

```zsh
git diff 501796b0ed1 HEAD -U0 -- dlls server include | grep -E '^(diff --git|@@)'
```

## Hooks

Calls into my files and the state they need. The Switch column names the switch each serves,
without its `WINE_HEAT_`, `WINE_MACDRV_` or `WINE_` prefix.

| File | Function | What the hook does | Switch | Issue |
|---|---|---|---|---|
| `dlls/ntdll/Makefile.in` | `SOURCES` | Builds the six `heat*.c` | all `WINE_HEAT_*` | #40 |
| `dlls/ntdll/unix/unix_private.h` | after `mutex_unlock` | Includes `heat.h` in every unix file | all `WINE_HEAT_*` | #40 |
| `dlls/ntdll/unix/unix_private.h` | `server_async` | The pool forgets an event the server sets when the I/O ends | `EVPOOL` | #42 |
| `dlls/ntdll/unix/loader.c` | `start_main_thread` | Calls `heat_init()` right after `load_compatdb()` | all `WINE_HEAT_*` | #40 |
| `dlls/ntdll/unix/file.c` | `NtSetInformationObject` | The pool forgets a handle that may become inheritable | `EVPOOL` | #42 |
| `dlls/ntdll/unix/registry.c` | `NtNotifyChangeMultipleKeys` | The pool forgets the event the server sets on a change | `EVPOOL` | #42 |
| `dlls/ntdll/unix/socket.c` | `sock_ioctl` | The pool forgets the event the server sets on a socket event | `EVPOOL` | #42 |
| `dlls/ntdll/unix/server.c` | `server_call_unlocked` | Records each request, its reply and its time | `COUNT` | #60 |
| `dlls/ntdll/unix/server.c` | `NtDuplicateObject` | Counts a close of the source in this process and clears its event mark, and the pool answers for a parked handle | `COUNT`, `EVPOOL` | #40, #42 |
| `dlls/ntdll/unix/server.c` | `NtClose` | Counts the close and clears the handle's event mark, and the pool parks an event instead of closing it | `EVPOOL`, `COUNT` | #40, #42 |
| `dlls/ntdll/unix/sync.c` | `heat_inproc_sync_idle` (new) | Says whether no thread of this process holds the handle's in-process sync | `EVPOOL` | #42 |
| `dlls/ntdll/unix/sync.c` | `NtCreateEvent` | Takes a parked event, offers a new one to the pool, counts creations | `EVPOOL`, `COUNT` | #40, #42 |
| `dlls/ntdll/unix/sync.c` | `NtPulseEvent` | The pool forgets an event that may keep an untaken pulse | `EVPOOL` | #42 |
| `dlls/ntdll/unix/sync.c` | `NtYieldExecution` | Times the yield, the `swtch()` call of WineHQ's `53ba24d` (ADR 0022) | `COUNT` | #40, #342 |
| `dlls/ntdll/unix/sync.c` | `NtQueryPerformanceCounter` | Counts calls | `COUNT` | #40 |
| `dlls/ntdll/unix/sync.c` | `NtAlertThreadByThreadId` | Counts alerts and times the wake call | `COUNT` | #99 |
| `dlls/ntdll/unix/sync.c` | `NtWaitForAlertByThreadId` | Spins before the first sleep, outside the emulator, and times the sleeps | `SPIN=lock`, `COUNT` | #45, #99 |
| `dlls/ntdll/unix/msync.c` | `msync_wait_single` | Times the sleep, and counts the sleepers on a semaphore | `COUNT`, `WAKE=skip` | #40, #99 |
| `dlls/ntdll/unix/msync.c` | `msync_wait_multiple` | Times the sleep, and counts the contention bailout's unregister, which athei's msync makes since Mach9 0.4.0 | `COUNT` | #44, #99 |
| `dlls/ntdll/unix/msync.c` | `signal_all` | Takes a semaphore release's count for `one` and `skip`, and times each wake call. `skip` decides by msync's reference count and this process's sleepers | `WAKE`, `COUNT` | #40, #99, #249 |
| `dlls/ntdll/unix/msync.c` | `msync_release_semaphore_obj`, `msync_set_event_obj`, `msync_release_mutex_obj` | Pass `signal_all` its count (0 for an event or a mutex), and count releases and sets | `WAKE`, `COUNT` | #40, #99 |
| `dlls/ntdll/unix/msync.c` | `msync_pulse_event_obj` | Keeps a pulse that a spinner may take | `SPIN=event` | #45 |
| `dlls/ntdll/unix/msync.c` | `heat_spin_objs` (new) | The spin on events and on a semaphore | `SPIN=event`, `SPIN=sem` | #45 |
| `dlls/ntdll/unix/msync.c` | `msync_wait_objs` | Counts waits and wakeups, spins once before the first sleep, and with `one` a wait-all woken on a semaphore passes the wake on. Its three fixes are in the second table | `COUNT`, `SPIN`, `WAKE=one` | #40, #45, #99 |
| `dlls/ntdll/unix/thread.c` | includes | Includes `heatprof.h` | `PROF` | #43 |
| `dlls/ntdll/unix/thread.c` | `pthread_exit_wrapper`, `spawn_thread` | Keeps the sampler's list of threads | `PROF` | #43 |
| `dlls/ntdll/unix/thread.c` | `set_native_thread_name` | Gives the counters the thread's name | `COUNT` | #99 |
| `dlls/ntdll/unix/thread.c` | `NtSetInformationThread` | Records the base priority a thread sets for itself, for the spin's scope | `SPIN` | #45 |
| `dlls/win32u/cursoricon.c` | `setcursor_shm`, `NtUserSetCursor` | Reads the switch, then answers an unchanged cursor from shared memory | `WIN32U_SETCURSOR` | #61 |
| `dlls/win32u/message.c` | `peek_no_yield`, `NtUserPeekMessage` | Reads the switch, then leaves out the empty peek's yield | `WIN32U_PEEK_YIELD` | #61 |
| `dlls/winemac.drv/macdrv_main.c` | includes, globals, `switch_is`, `setup_options` | Reads every `WINE_MACDRV_*` | all `WINE_MACDRV_*` | #57, #59, #62 |
| `dlls/winemac.drv/macdrv.h` | `struct macdrv_client_surface` | What a surface's last update sent to its view | `VIEW` | #62 |
| `dlls/winemac.drv/window.c` | `client_view_serial`, `client_views_changed`, `create_cocoa_window`, `destroy_cocoa_window`, `macdrv_client_surface_detach`, `add_remote_layer`, `remove_remote_layer`, `macdrv_DestroyWindow` | A serial that changes when a Cocoa window, a client view or a remote layer comes or goes | `VIEW` | #62, #166 |
| `dlls/winemac.drv/window.c` | `macdrv_client_surface_update` | Sends the view's frame and superview only when they changed | `VIEW` | #62 |
| `dlls/winemac.drv/mouse.c` | the clip record, `macdrv_ClipCursor` | Leaves out a call for the rectangle applied last | `CLIP` | #62 |
| `dlls/winemac.drv/cocoa_cursorclipping.m` | `clipping_changed`, `scale_rect_for_retina_mode`, both classes' `startClippingCursor` and `stopClippingCursor` | Bumps `clip_serial` on every change of the clipping | `CLIP` | #62 |
| `dlls/winemac.drv/event.c` | `macdrv_handle_event` | A display change bumps `clip_serial` | `CLIP` | #62 |
| `dlls/winemac.drv/event.c` | `check_fd_events` | Asks the pipe for its byte count | `DRAIN` | #62 |
| `dlls/winemac.drv/cocoa_event.m` | `initWithEventHandler` | Adds a user event to the queue's kqueue | `CALL` | #62 |
| `dlls/winemac.drv/cocoa_event.m` | `run_on_main_thread` (`OnMainThread` up to Mach9 0.3.1) | Wakes the caller through the kqueue, and leaves one byte in the pipe only for a waiting event | `CALL` | #62 |
| `dlls/winemac.drv/cocoa_gcmouse.m` | `gcmouse_post_movement`, `gcmouse_moved` | Scales the counts through `gcmouse_scale.h` and carries the fractions | `MOUSE_SCALE` | #59 |
| `dlls/winemac.drv/surface.c` | `struct macdrv_window_surface`, `macdrv_surface_flush`, `macdrv_surface_destroy` | Hands Cocoa a copy of the frame, skips an equal one, and counts flushes on the `macsurface` channel | `SURFACE` | #57 |

## Fixes without a switch

Changes to athei's or Wine's behaviour that are always on. I checked the "Upstream has it" column
against athei's `arm64-patched` at `501796b0ed1`, the commit of Mach9 0.4.0, on 2026-11-17, when I
moved to it. A rebase starts by checking every row again. Since 0.4.0 athei's commit has four fixes
I carried, and they left this table: the unregister on msync's contention bailout (#44, #104), both
fixes of #159 and the break-in of #174.

| File, function | What | Issue | Commit | Upstream has it |
|---|---|---|---|---|
| `dlls/ntdll/unix/msync.c`, `msync_wait_objs` | The wait-all's rollback releases only the mutexes this pass took | #44 | `f6097722b99` | no |
| `dlls/ntdll/unix/msync.c`, `msync_wait_objs` | A relative timeout's deadline is computed before the first spin or sleep, not at the wait's start | #101 | `41ca59151dc` | no |
| `dlls/ntdll/unix/msync.c`, `msync_wait_objs` | A wait-all that puts an object back wakes its waiters, all of them in every `WINE_HEAT_WAKE` mode since #249 | #219, #249 | `30c65fd5a6d` | athei: no |
| `dlls/ntdll/unix/loader.c`, `load_unixlib_by_name`; `dlls/ntdll/unix/virtual.c`, `get_unixlib_funcs`, `NtQueryVirtualMemory` | `dlopen`, `dlsym` and `dlclose` of the unix libs run with the server's signals blocked (`heatdl.h`), so no thread is suspended inside dyld's lock (ADR 0020) | #304 | at merge | athei: no. WineHQ: no, master on 2026-11-19 |
| `dlls/ntdll/unix/virtual.c`, `load_builtin_unixlib` | Drops `virtual_mutex` for `dlopen` and takes it again to store the handle, keeping one another thread stored meanwhile (ADR 0020) | #304 | at merge | athei: no. WineHQ: no, master on 2026-11-19 |
| `dlls/winemac.drv/window.c`, `macdrv_window_did_unminimize` | A restored window becomes the foreground window before it is activated | #160 | `8384433b856` | WineHQ `ede017e2b24`. athei: no |
| `dlls/winemac.drv/cocoa_app.m`, `+[WineApplicationController initialize]` | `NSEnableTextInputContextBasedAutoFill` is registered as `NO`, so AppKit starts no AutoFill helper per process (ADR 0021) | #342 | at merge | athei `5466044bde5`, after 0.4.0's commit |
| `dlls/ntdll/unix/virtual.c`, `virtual_handle_fault` | Rosetta's workaround, a read fault in a readable page taken as a write fault, only on x86_64 (ADR 0021) | #342 | at merge | WineHQ `dedead43f26`, after `wine-11.22`. athei: no |
| `dlls/ntdll/unix/sync.c`, `NtYieldExecution` | On macOS `swtch()`, and `STATUS_NO_YIELD_PERFORMED` when it returns false (ADR 0021) | #342 | at merge | WineHQ `53ba24da06b`, after `wine-11.22`. athei: no |
| `dlls/ntdll/unix/sync.c`, `NtDelayExecution` | A sleep that isn't alertable: a zero timeout is only the yield, a relative one a `nanosleep`, again with the rest after `EINTR` (ADR 0021) | #342 | at merge | WineHQ `48d67d33670`, after `wine-11.22`. athei: no |
