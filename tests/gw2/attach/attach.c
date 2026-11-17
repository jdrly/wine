/* gw2 (jdrly/gw2-on-mac#174): a debugger attaches to a running x86_64 process, and the process
 * runs on after the detach. Built for arm64, as the engine's winedbg is.
 *
 * It starts count.exe from its own folder, an x86_64 program, and waits until count.exe says it
 * runs. DebugActiveProcess then has DbgUiIssueRemoteBreakin start a thread in count.exe at a
 * DbgUiRemoteBreakin. Under Mach9 0.3.1's ntdll.so that was this program's own, in the ARM64 half of
 * ntdll, which FEX runs as x86 code in count.exe: count.exe dies with c0000005. Under mine it is
 * count.exe's own, which raises a breakpoint.
 *
 * It continues the first breakpoint as handled, waits for the thread that raised it to exit with 0,
 * detaches and tells count.exe, which counts on and exits 0. Any other exception is passed on to
 * count.exe, as without a debugger. Each wait is without a time limit; if one never ends, run.zsh's
 * time limit ends the run, and count.exe with it.
 *
 * Prints "gw2 test attach: passed" and exits 0 once count.exe has exited with 0, or names what
 * failed and exits 1. */
#define _WIN32_WINNT 0x0A00
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>

static PROCESS_INFORMATION pi;

static void __attribute__((noreturn)) fail( const char *format, ... )
{
    va_list args;

    printf( "gw2 test attach: " );
    va_start( args, format );
    vprintf( format, args );
    va_end( args );
    printf( "\n" );
    if (pi.hProcess) TerminateProcess( pi.hProcess, 1 );
    ExitProcess( 1 );
}

int main( void )
{
    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
    STARTUPINFOA si = { sizeof(si) };
    PROCESS_MACHINE_INFORMATION machine = { 0 };
    char path[MAX_PATH], cmd[MAX_PATH + 64], *slash;
    DWORD breakin = 0, status, code = ~0u;
    HANDLE ready, detached, started[2];
    EXCEPTION_RECORD *rec;
    DEBUG_EVENT ev;
    BOOL done = FALSE;

    setvbuf( stdout, NULL, _IONBF, 0 );
    if (!GetModuleFileNameA( NULL, path, sizeof(path) - 16 ) || !(slash = strrchr( path, '\\' )))
        fail( "no path of its own (error %lu)", GetLastError() );
    strcpy( slash + 1, "count.exe" );
    if (!(ready = CreateEventA( &sa, TRUE, FALSE, NULL )) || !(detached = CreateEventA( &sa, TRUE, FALSE, NULL )))
        fail( "CreateEvent failed (error %lu)", GetLastError() );
    snprintf( cmd, sizeof(cmd), "\"%s\" %lu %lu", path, (unsigned long)(ULONG_PTR)ready,
              (unsigned long)(ULONG_PTR)detached );
    if (!CreateProcessA( NULL, cmd, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi ))
        fail( "can't start %s (error %lu)", path, GetLastError() );
    if (!GetProcessInformation( pi.hProcess, ProcessMachineTypeInfo, &machine, sizeof(machine) ))
        fail( "can't read count.exe's machine (error %lu)", GetLastError() );
    if (machine.ProcessMachine != IMAGE_FILE_MACHINE_AMD64)
        fail( "count.exe runs as machine %04x, not x86_64", machine.ProcessMachine );
    started[0] = ready;
    started[1] = pi.hProcess;
    if (WaitForMultipleObjects( 2, started, FALSE, INFINITE ) != WAIT_OBJECT_0)
    {
        GetExitCodeProcess( pi.hProcess, &code );
        fail( "count.exe exited with %08lx before it ran", code );
    }

    if (!DebugActiveProcess( pi.dwProcessId ))
        fail( "DebugActiveProcess failed (error %lu)", GetLastError() );
    while (!done)
    {
        if (!WaitForDebugEvent( &ev, INFINITE ))
            fail( "WaitForDebugEvent failed (error %lu)", GetLastError() );
        status = DBG_CONTINUE;
        switch (ev.dwDebugEventCode)
        {
        case CREATE_PROCESS_DEBUG_EVENT:
            if (ev.u.CreateProcessInfo.hFile) CloseHandle( ev.u.CreateProcessInfo.hFile );
            break;
        case LOAD_DLL_DEBUG_EVENT:
            if (ev.u.LoadDll.hFile) CloseHandle( ev.u.LoadDll.hFile );
            break;
        case EXCEPTION_DEBUG_EVENT:
            rec = &ev.u.Exception.ExceptionRecord;
            if (!breakin && rec->ExceptionCode == EXCEPTION_BREAKPOINT)
            {
                breakin = ev.dwThreadId;
                printf( "gw2 test attach: the break-in's breakpoint at %p on thread %04lx, continued\n",
                        rec->ExceptionAddress, breakin );
                break;
            }
            printf( "gw2 test attach: exception %08lx at %p on thread %04lx (%s chance), passed on\n",
                    rec->ExceptionCode, rec->ExceptionAddress, ev.dwThreadId,
                    ev.u.Exception.dwFirstChance ? "first" : "second" );
            status = DBG_EXCEPTION_NOT_HANDLED;
            break;
        case EXIT_THREAD_DEBUG_EVENT:
            if (!breakin || ev.dwThreadId != breakin) break;
            if (ev.u.ExitThread.dwExitCode)
                fail( "the break-in's thread exited with %08lx", ev.u.ExitThread.dwExitCode );
            done = TRUE;
            break;
        case EXIT_PROCESS_DEBUG_EVENT:
            fail( "count.exe died with %08lx while attached", ev.u.ExitProcess.dwExitCode );
        }
        if (!ContinueDebugEvent( ev.dwProcessId, ev.dwThreadId, status ))
            fail( "ContinueDebugEvent failed (error %lu)", GetLastError() );
    }
    if (!DebugActiveProcessStop( pi.dwProcessId ))
        fail( "DebugActiveProcessStop failed (error %lu)", GetLastError() );
    printf( "gw2 test attach: detached\n" );

    if (!SetEvent( detached )) fail( "SetEvent failed (error %lu)", GetLastError() );
    WaitForSingleObject( pi.hProcess, INFINITE );
    if (!GetExitCodeProcess( pi.hProcess, &code ) || code)
        fail( "count.exe exited with %08lx after the detach", code );
    printf( "gw2 test attach: passed\n" );
    return 0;
}
