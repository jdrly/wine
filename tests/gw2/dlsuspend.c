/* gw2 (jdrly/gw2-on-mac#304): a thread suspended inside dlopen must not deadlock a sampler that
 * reads memory before it resumes it.
 *
 * Steam's web helper hung that way. Its stack sampler suspended a thread that was inside dlopen,
 * holding dyld's loader lock. A second thread loaded a builtin's unix lib, which took
 * virtual_mutex and then waited for dyld's lock. The sampler's next memory query waited for
 * virtual_mutex, so it never got to resume the first thread. Here:
 *
 * - L loads ws2_32.so by name and unloads it, in a loop (MemoryWineLoadUnixLibByName,
 *   MemoryWineUnloadUnixLib), so most of its time is inside dyld's lock. ws2_32.so always has its
 *   call table. Mach9 builds netapi32.so without libnetapi, so it has none, and the load fails.
 * - B maps dnsapi.dll without its imports or DllMain, asks for its unix lib (MemoryWineLoadUnixLib)
 *   and unmaps it, in a loop. Each pass loads dnsapi.so and unloads it again. DllMain is skipped
 *   because it resolves the host name.
 * - S suspends L, waits until L is stopped, sleeps HOLD_MS, queries memory, and resumes L, in a
 *   loop.
 *
 * L and S make no heap, stdio or loader call of their own, so a thread suspended in one of those
 * can't hang the test. ntdll's load by name still calls malloc and free outside the mask, so a
 * stall can also come from a suspend inside malloc's lock, though dyld's lock is far likelier.
 * When S makes no progress for STALL_MS, or the threads don't end within STALL_MS of the stop,
 * the test prints the deadlock, then resumes L itself so the threads can end. The engine's own
 * ntdll.so is expected to deadlock here. Mine has to pass. Runs SECONDS, and fails if a thread
 * made fewer than MIN_PASSES passes.
 * Prints "gw2 test dlsuspend: passed" and exits 0, or names what failed and exits 1. */
#include <stdio.h>
#include <windows.h>

#define SECONDS 15
#define WARMUP_MS 500
#define HOLD_MS 2
#define STALL_MS 10000
#define MIN_PASSES 100

#define MemoryWineLoadUnixLib 1000
#define MemoryWineLoadUnixLibByName 1002
#define MemoryWineUnloadUnixLib 1004

struct name_string
{
    USHORT Length;
    USHORT MaximumLength;
    WCHAR *Buffer;
};

static LONG (WINAPI *pNtQueryVirtualMemory)( HANDLE process, const void *addr, int info_class,
                                             void *buffer, SIZE_T len, SIZE_T *res_len );

static HANDLE loader_thread;
static volatile LONG stop;
static volatile LONG loader_status, unload_status, builtin_status, builtin_stayed;
static volatile DWORD builtin_error, sampler_error;
static volatile LONG64 loads, builtins, samples;

/* L: ws2_32.so by name, loaded and unloaded */
static DWORD WINAPI loader( void *arg )
{
    static WCHAR ws2_32[] = L"ws2_32";
    struct name_string name = { sizeof(ws2_32) - sizeof(WCHAR), sizeof(ws2_32), ws2_32 };
    UINT64 res[2];
    LONG status;

    while (!stop)
    {
        if ((status = pNtQueryVirtualMemory( GetCurrentProcess(), &name, MemoryWineLoadUnixLibByName,
                                             res, sizeof(res), NULL )))
        {
            loader_status = status;
            stop = 1;
            break;
        }
        if ((status = pNtQueryVirtualMemory( GetCurrentProcess(), &res[0], MemoryWineUnloadUnixLib, NULL, 0, NULL )))
        {
            unload_status = status;
            stop = 1;
            break;
        }
        InterlockedIncrement64( &loads );
    }
    return 0;
}

/* B: dnsapi.dll mapped, its unix lib loaded, and unmapped */
static DWORD WINAPI builtin( void *arg )
{
    UINT64 funcs;
    HMODULE module;
    LONG status;

    while (!stop)
    {
        if (!(module = LoadLibraryExW( L"dnsapi.dll", NULL, DONT_RESOLVE_DLL_REFERENCES )))
        {
            builtin_error = GetLastError();
            stop = 1;
            break;
        }
        status = pNtQueryVirtualMemory( GetCurrentProcess(), module, MemoryWineLoadUnixLib, &funcs, sizeof(funcs), NULL );
        FreeLibrary( module );
        if (status)
        {
            builtin_status = status;
            stop = 1;
            break;
        }
        if (GetModuleHandleW( L"dnsapi.dll" ))
        {
            builtin_stayed = 1;
            stop = 1;
            break;
        }
        InterlockedIncrement64( &builtins );
    }
    return 0;
}

/* S: suspends L, reads memory while L is stopped, resumes L */
static DWORD WINAPI sampler( void *arg )
{
    MEMORY_BASIC_INFORMATION info;
    CONTEXT context;

    while (!stop)
    {
        if (SuspendThread( loader_thread ) == (DWORD)-1)
        {
            sampler_error = GetLastError();
            stop = 1;
            break;
        }
        /* returns once L is stopped */
        context.ContextFlags = CONTEXT_CONTROL;
        if (!GetThreadContext( loader_thread, &context )) sampler_error = GetLastError();
        Sleep( HOLD_MS );
        if (!VirtualQuery( &info, &info, sizeof(info) ) && !sampler_error) sampler_error = GetLastError();
        ResumeThread( loader_thread );
        if (sampler_error)
        {
            stop = 1;
            break;
        }
        InterlockedIncrement64( &samples );
    }
    return 0;
}

/* the deadlock's line, written without the heap: a thread may be stuck holding anything */
static void print_stall(void)
{
    char line[256];
    DWORD written;
    int len;

    len = snprintf( line, sizeof(line), "gw2 test dlsuspend: failed: deadlock: the sampler made no progress for %d ms "
                    "(%lld samples, %lld loads by name, %lld builtin loads)\n", STALL_MS, samples, loads, builtins );
    WriteFile( GetStdHandle( STD_OUTPUT_HANDLE ), line, len, &written, NULL );
}

int main(void)
{
    HANDLE threads[3];
    LONG64 last = -1;
    DWORD t0, moved, now;
    BOOL stalled = FALSE;
    int i;

    setvbuf( stdout, NULL, _IONBF, 0 );
    printf( "gw2 test dlsuspend: L loads a unix lib by name, B a builtin's, S suspends L and reads memory\n" );
    if (!(pNtQueryVirtualMemory = (void *)GetProcAddress( GetModuleHandleA( "ntdll.dll" ), "NtQueryVirtualMemory" )))
    {
        printf( "gw2 test dlsuspend: ntdll.dll has no NtQueryVirtualMemory\n" );
        return 1;
    }
    if (GetModuleHandleW( L"dnsapi.dll" ))
    {
        printf( "gw2 test dlsuspend: failed: dnsapi.dll is loaded already, so B wouldn't unload it\n" );
        return 1;
    }

    loader_thread = threads[0] = CreateThread( NULL, 0, loader, NULL, 0, NULL );
    threads[1] = CreateThread( NULL, 0, builtin, NULL, 0, NULL );
    if (!threads[0] || !threads[1]) return 3;
    Sleep( WARMUP_MS );
    if (!(threads[2] = CreateThread( NULL, 0, sampler, NULL, 0, NULL ))) return 3;

    for (t0 = moved = GetTickCount(); !stop && (now = GetTickCount()) - t0 < SECONDS * 1000; Sleep( 100 ))
    {
        if (samples != last)
        {
            last = samples;
            moved = now;
        }
        else if (now - moved >= STALL_MS)
        {
            stalled = TRUE;
            break;
        }
    }
    stop = 1;
    /* a stall that started in the last STALL_MS shows as threads that don't end */
    if (!stalled && WaitForMultipleObjects( 3, threads, TRUE, STALL_MS ) != WAIT_OBJECT_0) stalled = TRUE;
    if (stalled)
    {
        print_stall();
        /* nothing else resumes L: undo every suspend of S's, so the threads can end */
        for (i = 0; i < 10 && ResumeThread( loader_thread ) > 1; i++);
    }
    if (WaitForMultipleObjects( 3, threads, TRUE, 10000 ) != WAIT_OBJECT_0)
    {
        printf( "gw2 test dlsuspend: failed: the threads didn't end\n" );
        return 1;
    }

    printf( "gw2 test dlsuspend: %lld samples, %lld loads by name, %lld builtin loads, %lu ms\n",
            samples, loads, builtins, GetTickCount() - t0 );
    if (stalled) return 1;
    if (loader_status)
    {
        printf( "gw2 test dlsuspend: failed: loading ws2_32.so by name failed (status %#lx)\n", loader_status );
        return 1;
    }
    if (unload_status)
    {
        printf( "gw2 test dlsuspend: failed: unloading ws2_32.so failed (status %#lx)\n", unload_status );
        return 1;
    }
    if (builtin_error)
    {
        printf( "gw2 test dlsuspend: failed: mapping dnsapi.dll failed (error %lu)\n", builtin_error );
        return 1;
    }
    if (builtin_status)
    {
        printf( "gw2 test dlsuspend: failed: loading dnsapi's unix lib failed (status %#lx)\n", builtin_status );
        return 1;
    }
    if (builtin_stayed)
    {
        printf( "gw2 test dlsuspend: failed: dnsapi.dll stayed loaded after FreeLibrary\n" );
        return 1;
    }
    if (sampler_error)
    {
        printf( "gw2 test dlsuspend: failed: suspending or reading L failed (error %lu)\n", sampler_error );
        return 1;
    }
    if (samples < MIN_PASSES || loads < MIN_PASSES || builtins < MIN_PASSES)
    {
        printf( "gw2 test dlsuspend: failed: fewer than %d passes of a thread in %d s\n", MIN_PASSES, SECONDS );
        return 1;
    }
    for (i = 0; i < 3; i++) CloseHandle( threads[i] );
    printf( "gw2 test dlsuspend: passed\n" );
    return 0;
}
