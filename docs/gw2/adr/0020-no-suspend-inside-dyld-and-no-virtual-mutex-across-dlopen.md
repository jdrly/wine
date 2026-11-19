# No suspend inside dyld's lock, and no virtual_mutex across dlopen

Decided 2026-11-19. Status: accepted.

Steam's web helper (Chrome 126) hung on some first starts on Mach9 0.4.0 (#264). I sampled one
hang on 0.2.0-rc.3 with my `ntdll.so` at `5e7bde7` (2026-11-19 18:23,
`~/Desktop/gw2-rc3-steamhang-1824/sample-93265.txt` on my Mac). Three groups of threads waited on each other:

1. CrBrowserMain: `NtQueryVirtualMemory(MemoryWineLoadUnixLibByName)`, `load_unixlib_by_name`,
   `dlopen`, inside dyld's loader lock. There it took SIGUSR1 (`usr1_handler`, `wait_suspend`,
   `server_select`). Another thread had suspended it with dyld's lock held.
2. Chrome_InProcGpuThread: `NtQueryVirtualMemory(MemoryWineLoadUnixLib)`, `load_builtin_unixlib`,
   which held `virtual_mutex` (`server_enter_uninterrupted_section`) across `dlopen`. It waited
   for dyld's lock in `_os_unfair_lock_lock_slow`.
3. StackSamplingProfiler, HangWatcher, MemoryInfra and the thread pool's workers: all in
   `server_enter_uninterrupted_section(&virtual_mutex)`, from `get_basic_memory_info`,
   `allocate_virtual_memory` and `NtFreeVirtualMemory`. The profiler suspends a thread, reads its
   memory and then resumes it. It never got to the resume.

Chrome with GPU acceleration off has no in-process GPU thread to load a unix lib beside the main
thread, and that start never hung. Each change below breaks the cycle by itself. I take both,
because each closes a different way in:

- `dlopen`, `dlsym` and `dlclose` of the unix libs run with the server's signals blocked
  (`heatdl.h`), as `server_enter_uninterrupted_section` blocks them but without a mutex. A suspend
  that arrives meanwhile is taken when the mask is put back, outside dyld's lock. That removes
  step 1: a thread can't stop while it holds dyld's lock in these calls.
- `load_builtin_unixlib` looks the module up under `virtual_mutex`, drops it for `dlopen`, and
  takes it again to store the handle. If another thread stored one meanwhile, or the module is gone
  or has another path, it keeps what is there and closes its own. That removes step 2's hold: a
  thread waiting for dyld's lock no longer keeps every memory query out.

Both are fixes to Wine's own code, so they have no switch (ADR 0006).

## Consequences

- The calls with the signals blocked: the five `dlopen`s of `load_unixlib_by_name` (loader.c), the
  two `dlsym`s of `get_unixlib_funcs`, `load_builtin_unixlib`'s `dlopen` and its `dlclose` of a
  handle it lost, and the `dlclose`s of `MemoryWineLoadUnixLibByName`'s failure path and of
  `MemoryWineUnloadUnixLib` (virtual.c).
- Not changed: `dlopen_dll` (for `.so` builtins, which my engine doesn't ship),
  `load_compatdb` and msync's `dlopen(NULL)`, which run at process start, and
  `release_builtin_module`, whose `dlclose` runs under `virtual_mutex` with the signals already
  blocked. That last one still waits for dyld's lock with `virtual_mutex` held. It can only hang
  when some other thread is suspended inside dyld's lock, which these changes rule out for
  ntdll's own calls but not for a unix lib's or the Mac driver's own `dlopen`.
- A thread's unix lib constructors now run with the server's signals blocked when the lib loads by
  name, as they already did when `load_builtin_unixlib` loaded it under the mutex. A suspend
  waits until `dlopen` returns.
- `tests/gw2/dlsuspend.c` runs the three roles: one thread loads `netapi32.so` by name and unloads
  it, one maps `dnsapi.dll` and loads its unix lib, and one suspends the first, reads memory and
  resumes it. matrix.zsh's block 3b runs it three times under mine, which have to pass, and three
  times under the engine's own `ntdll.so`, where it only reports how many runs deadlocked. Whether
  a run hits the race is up to the runner's timing, and I couldn't run it before the first push.
- A row in `.github/gw2/UPSTREAM.md`'s "Fixes without a switch". Neither athei's `501796b0ed1`
  nor WineHQ's master on 2026-11-19 has either change.
- The check that matters is Steam's first start with GPU acceleration on, which only my play
  shows.

## Alternatives I rejected

- **Only the second change.** It frees `virtual_mutex`, but a sampler that suspends a thread
  inside dyld's lock still stalls every other thread's `dlopen` or `dlsym` until it resumes.
- **Only the first.** `load_builtin_unixlib` would still wait for dyld's lock with
  `virtual_mutex` held, whenever any thread outside these calls holds dyld's lock while
  suspended.
- **The signals blocked around a lib's `__wine_unix_lib_init` as well.** That runs whatever the
  lib does at init with no suspend possible. It stays outside the mask, as it was.

## Evidence

- jdrly/gw2-on-mac #304, #264.
- `~/Desktop/gw2-rc3-steamhang-1824/sample-93265.txt` on my Mac, 0.2.0-rc.3, 2026-11-19 18:23.
- WineHQ master's `load_builtin_unixlib` and `load_unixlib_by_name`, read on 2026-11-19.
