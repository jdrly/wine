# My switches

A switch is an environment variable that turns one change of mine on. Unset, each file behaves
as athei's does. Each is read once: the `WINE_HEAT_*` ones in `heat_init()` at process start,
right after compatdb, so that a compatdb rule's `env=` can set them; the Mac driver's in
`setup_options()` when the driver loads; win32u's at the first call that needs them. A value a
switch doesn't know counts as unset, and the Mac driver and win32u log it. The lists
(`WINE_HEAT_WAKE`, `WINE_HEAT_SPIN`, `WINE_HEAT_PROF_THREADS`) skip an entry they don't know and
keep the rest. The `WINE_HEAT_*`
switches act only in the process `WINE_HEAT_EXE` names. The line `switches: …` of a build's
`VERSION` names the variables each file reads (`.github/gw2/switches.zsh`), and my app greys a
switch its file doesn't have. Issue numbers are in jdrly/gw2-on-mac.

Lint holds this table to the sources: a `getenv` of a `WINE_HEAT_*`, `WINE_MACDRV_*` or
`WINE_WIN32U_*` name without a row fails it, and so does a row whose name nothing reads.

| Variable | Read in, then used in | Values | When unset | What it does | Issue |
|---|---|---|---|---|---|
| `WINE_HEAT_EXE` | `heat.c` | an exe's file name, any case | `Gw2-64.exe` | The one process the `WINE_HEAT_*` switches act in | #40 |
| `WINE_HEAT_LOG` | `heat.c` | a directory | `/tmp` | Where the counters', the requests' and the sampler's files go | #40 |
| `WINE_HEAT_COUNT` | `heat.c`, `heatreq.c` | `1` | off | Per-thread counters in `heat-<pid>.log`, every 5 s and at exit, and the server requests of the game thread and two more in `heat-requests-<pid>.log`. With `FEX_PROFILESTATS=1` those get FEX's counters too | #40, #60, #99, #111 |
| `WINE_HEAT_DUMP_MS` | `heat.c` | 100 to 60000 | 5000 | The counters' dump period in milliseconds. Only the tests set it | #222 |
| `WINE_HEAT_WAKE` | `heat.c`, `heatwake.c`, `msync.c` | `one`, `skip`, `one,skip` | off: msync wakes every waiter on every release | `one`: a semaphore release of count n makes at most n wake-one calls and stops at the first that finds no waiter. `skip`: no wake call for a semaphore that only this process has looked up (msync's reference count at most 2) and that no thread of this process sleeps on | #40, #249 |
| `WINE_HEAT_EVPOOL` | `heat.c`, `heatpool.c` | `1`, `1,mask` | off | `NtClose` parks an event only this process can hold, and `NtCreateEvent` takes one parked for at least 2 ms, so GW2's event churn makes no server call. `1,mask` blocks signals around the pool's mutex instead of using the per-thread flag | #42 |
| `WINE_HEAT_SPIN` | `heat.c`, `heatspin.c`, `msync.c`, `sync.c` | `lock=<us>,event=<us>,sem=<us>`, each 1 to 1000 and optional, each with an optional scope `:all`, `:game`, `:game+halfworkers` or `:not=<priority>` | off | Spins up to that long before a lock sleep, a sleep on events only, or a single wait's sleep on a semaphore. `:game+halfworkers` is `:game` except that every other ABOVE_NORMAL thread spins, numbered at its first wait of any kind with that scope (ADR 0023) | #45, #368 |
| `WINE_HEAT_SPIN_PAUSE` | `heat.c`, `heatspin.c` | 0 to 1000000 | 32 | After 4 misses in a row, that many waits of the kind go to sleep without a spin | #45 |
| `WINE_HEAT_PROF` | `heat.c`, `heatprof.c` | `1` | off | Samples the game thread by x86 address under FEX, into `heat-prof-<pid>.bin` and `.txt` | #43 |
| `WINE_HEAT_PROF_HZ` | `heatprof.c` | 10 to 10000 | 1000 | The sampler's mean rate per second | #43 |
| `WINE_HEAT_PROF_THREADS` | `heatprof.c` | a list of `render`, `worker` | the game thread only | Also samples bgfx's render thread (`render`), or the next busiest unnamed thread the exe started after the game thread (`worker`) | #43 |
| `WINE_HEAT_PROF_FEX` | `heatprof.c`, `heatfex.h` | `<GUID>:<age>:<SizeOfImage>`: the CodeView GUID's 16 bytes as 32 hex digits in file order (not a GUID string without its hyphens), the age in decimal, the size in hex with `0x` or decimal, as my FEX's `VERSION` line `libarm64ecfex.dll: build id …` gives them | only the engine's FEX, pinned in `heatprof.h` | Names a FEX of mine the sampler may read | #111 |
| `WINE_MACDRV_SURFACE` | `macdrv_main.c`, `surface.c` | `copy`, `copy,skip` | off | `copy` gives Cocoa a copy of each window surface frame, so the launcher's layered window can't show a torn one. `copy,skip` also leaves out a frame equal to the last one handed over | #57 |
| `WINE_MACDRV_MOUSE_SCALE` | `macdrv_main.c`, `cocoa_gcmouse.m` | a number above 0 and up to 100, digits and a point | `1` | A factor on a GameController mouse's counts, for the game's mouse-look | #59 |
| `WINE_MACDRV_VIEW` | `macdrv_main.c`, `window.c` | `changes` | off | A client surface's view frame and superview go to the main thread only when they changed | #62 |
| `WINE_MACDRV_CLIP` | `macdrv_main.c`, `mouse.c` | `changes` | off | `ClipCursor` doesn't call the main thread for the rectangle it applied last, while nothing changed the clipping since | #62 |
| `WINE_MACDRV_CALL` | `macdrv_main.c`, `cocoa_event.m` | `quiet` | off | The main thread wakes a synchronous call's caller through its kqueue, not a byte in its event pipe | #62 |
| `WINE_MACDRV_DRAIN` | `macdrv_main.c`, `event.c` | `fionread` | off | `ProcessEvents` asks the event pipe for its byte count instead of polling it | #62 |
| `WINE_WIN32U_SETCURSOR` | `cursoricon.c` | `shm` | off | `SetCursor` answers an unchanged cursor from shared memory, without the server | #61 |
| `WINE_WIN32U_PEEK_YIELD` | `message.c` | `off` | Wine's yield | An empty `PeekMessage` doesn't yield, unless the Win16 thunk lock's callback is registered | #61 |
