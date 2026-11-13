/* gw2 (jdrly/gw2-on-mac#160): a window that comes back from the Dock has to be the foreground
 * window before it is activated. When Cocoa deminiaturizes a window that Windows has minimized,
 * the Mac driver's macdrv_window_did_unminimize activates it and sends it SC_RESTORE. Since Wine
 * 11.8's revert d56f36c1aa0 it activates it with NtUserSetActiveWindow, and win32u's
 * set_active_window sends WM_NCACTIVATE with wParam "is it the foreground window", so the window
 * gets 0 and thinks it is still in the background. WineHQ's ede017e2b24 makes it the foreground
 * window first.
 *
 * With GW2_TEST_REOPEN (the path of tests/gw2/native/reopen, built natively) and GW2_TEST_RESTORE
 * (foreground or background), where the Mac driver starts:
 * 1. It makes a window, shows it, and runs "reopen shown", which returns once the window is on
 *    the screen.
 * 2. It minimizes the window the way Cmd+M does (WM_SYSCOMMAND SC_MINIMIZE, which the driver's
 *    macdrv_window_minimize_requested sends), then makes the desktop window the foreground
 *    window. On my Mac the driver does that when I switch to another app (macdrv_app_deactivated).
 *    Nobody switches apps on the runner, so the program does it.
 * 3. It runs "reopen restore", which waits until the window is off the screen and then sends the
 *    process the reopen Apple event, as a click on its Dock icon does, until the window is back.
 *    AppKit deminiaturizes it, and the driver restores it.
 * 4. From the moment the foreground has left the window until SC_RESTORE is done, it records
 *    WM_NCACTIVATE (its wParam, and whether the window was the foreground window then),
 *    WM_ACTIVATE and WM_SYSCOMMAND SC_RESTORE.
 * The first WM_NCACTIVATE has to come before SC_RESTORE. With foreground (my winemac.so) its
 * wParam is nonzero and the window is the foreground window. With background (the engine's) its
 * wParam is 0 and the window isn't the foreground window: the engine's bug, which shows that the
 * test sees it.
 *
 * Without the two variables, or without the Mac driver, it says so and passes: the matrix runs
 * every program. If the system refuses reopen's Apple event, it says "inconclusive" and passes,
 * and the workflow's step warns. No step passes on time: each waits for the one before it, and
 * run.zsh's time limit fails a run that never gets there. Prints "gw2 test restore: passed", or
 * names what failed and exits 1. */
#include <stdio.h>
#include <string.h>
#include <windows.h>

static LONG (WINAPI *p__wine_unix_spawnvp)( char * const argv[], int wait );

static HWND window;
static BOOL armed, restored;

/* what the window got while armed, in order */
static struct
{
    UINT msg;
    WPARAM wparam;
    BOOL foreground;
} record[64];
static unsigned int recorded;

static char reopen[MAX_PATH];
static char *reopen_argv[3];
static LONG reopen_status;

static LRESULT CALLBACK wndproc( HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam )
{
    BOOL restore = msg == WM_SYSCOMMAND && (wparam & 0xfff0) == SC_RESTORE;
    LRESULT ret;

    if (armed && !restored && recorded < sizeof(record) / sizeof(record[0]) &&
        (msg == WM_NCACTIVATE || msg == WM_ACTIVATE || restore))
    {
        record[recorded].msg = msg;
        record[recorded].wparam = wparam;
        record[recorded].foreground = GetForegroundWindow() == hwnd;
        recorded++;
    }
    ret = DefWindowProcW( hwnd, msg, wparam, lparam );
    if (restore && armed) restored = TRUE;
    return ret;
}

static int fail( const char *what )
{
    printf( "gw2 test restore: %s failed (error %lu)\n", what, GetLastError() );
    return 1;
}

static void pump(void)
{
    MSG msg;

    while (PeekMessageW( &msg, NULL, 0, 0, PM_REMOVE ))
    {
        TranslateMessage( &msg );
        DispatchMessageW( &msg );
    }
}

static DWORD WINAPI reopen_thread( void *arg )
{
    reopen_status = p__wine_unix_spawnvp( reopen_argv, 1 );
    return 0;
}

/* runs "reopen <mode>" on a thread of its own and takes the window's messages until it exits, and
 * with <until> also until *until is set. Returns reopen's exit status, or -1 if it couldn't start
 * it. */
static LONG run_reopen( char *mode, const BOOL *until )
{
    HANDLE thread;
    BOOL done = FALSE;

    reopen_argv[0] = reopen;
    reopen_argv[1] = mode;
    reopen_argv[2] = NULL;
    if (!(thread = CreateThread( NULL, 0, reopen_thread, NULL, 0, NULL ))) return -1;
    for (;;)
    {
        pump();
        if (!done && !WaitForSingleObject( thread, 0 )) done = TRUE;
        /* reopen failed: no restore will come */
        if (done && reopen_status) break;
        if (done && (!until || *until)) break;
        MsgWaitForMultipleObjects( done ? 0 : 1, &thread, FALSE, INFINITE, QS_ALLINPUT );
    }
    CloseHandle( thread );
    return reopen_status;
}

static const char *name( UINT msg )
{
    switch (msg)
    {
    case WM_NCACTIVATE: return "WM_NCACTIVATE";
    case WM_ACTIVATE: return "WM_ACTIVATE";
    default: return "WM_SYSCOMMAND SC_RESTORE";
    }
}

int main( void )
{
    WNDCLASSW cls = { .lpfnWndProc = wndproc, .lpszClassName = L"gw2 test restore" };
    char want[16];
    BOOL foreground;
    unsigned int i, first = ~0u, restore_at = ~0u;
    LONG status;

    if (!GetEnvironmentVariableA( "GW2_TEST_REOPEN", reopen, sizeof(reopen) ) ||
        !GetEnvironmentVariableA( "GW2_TEST_RESTORE", want, sizeof(want) ))
    {
        printf( "gw2 test restore: GW2_TEST_REOPEN or GW2_TEST_RESTORE isn't set, so nothing to do\n" );
        printf( "gw2 test restore: passed\n" );
        return 0;
    }
    if (!strcmp( want, "foreground" )) foreground = TRUE;
    else if (!strcmp( want, "background" )) foreground = FALSE;
    else
    {
        printf( "gw2 test restore: GW2_TEST_RESTORE is \"%s\", not foreground or background\n", want );
        return 1;
    }
    if (!(p__wine_unix_spawnvp = (void *)GetProcAddress( GetModuleHandleA( "ntdll.dll" ), "__wine_unix_spawnvp" )))
    {
        printf( "gw2 test restore: ntdll.dll has no __wine_unix_spawnvp\n" );
        return 1;
    }

    cls.hInstance = GetModuleHandleW( NULL );
    if (!RegisterClassW( &cls )) return fail( "RegisterClass" );
    window = CreateWindowExW( 0, cls.lpszClassName, L"gw2 test restore", WS_OVERLAPPEDWINDOW, 100, 100, 320, 240,
                              NULL, NULL, cls.hInstance, NULL );
    if (!GetModuleHandleW( L"winemac.drv" ))
    {
        printf( "gw2 test restore: no Mac driver, so nothing to restore\n" );
        if (window) DestroyWindow( window );
        printf( "gw2 test restore: passed\n" );
        return 0;
    }
    if (!window) return fail( "CreateWindowEx" );
    ShowWindow( window, SW_SHOWNORMAL );
    if (!UpdateWindow( window )) return fail( "UpdateWindow" );

    /* 1. on the screen */
    if ((status = run_reopen( "shown", NULL )))
    {
        printf( "gw2 test restore: reopen shown exited %ld\n", status );
        return 1;
    }

    /* 2. minimized, and the foreground elsewhere */
    SendMessageW( window, WM_SYSCOMMAND, SC_MINIMIZE, 0 );
    if (!IsIconic( window ))
    {
        printf( "gw2 test restore: SC_MINIMIZE didn't minimize the window\n" );
        return 1;
    }
    SetForegroundWindow( GetDesktopWindow() );
    pump();
    if (GetForegroundWindow() == window)
    {
        printf( "gw2 test restore: the window is still the foreground window after SetForegroundWindow(desktop)\n" );
        return 1;
    }
    armed = TRUE;

    /* 3. back from the Dock */
    status = run_reopen( "restore", &restored );
    if (status == 2)
    {
        printf( "gw2 test restore: inconclusive, the system refused reopen's Apple event\n" );
        DestroyWindow( window );
        printf( "gw2 test restore: passed\n" );
        return 0;
    }
    if (status)
    {
        printf( "gw2 test restore: reopen restore exited %ld\n", status );
        return 1;
    }

    /* 4. the record */
    for (i = 0; i < recorded; i++)
    {
        if (record[i].msg == WM_SYSCOMMAND)
            printf( "gw2 test restore: %s\n", name( record[i].msg ) );
        else
            printf( "gw2 test restore: %s wParam %#llx, the foreground window: %s\n", name( record[i].msg ),
                    (unsigned long long)record[i].wparam, record[i].foreground ? "yes" : "no" );
        if (record[i].msg == WM_NCACTIVATE && first == ~0u) first = i;
        if (record[i].msg == WM_SYSCOMMAND && restore_at == ~0u) restore_at = i;
    }
    if (IsIconic( window ))
    {
        printf( "gw2 test restore: the window is still minimized after SC_RESTORE\n" );
        return 1;
    }
    if (first == ~0u || first > restore_at)
    {
        printf( "gw2 test restore: no WM_NCACTIVATE before SC_RESTORE\n" );
        return 1;
    }
    if (foreground && (!record[first].wparam || !record[first].foreground))
    {
        printf( "gw2 test restore: the first WM_NCACTIVATE came before the window was the foreground window\n" );
        return 1;
    }
    if (!foreground && (record[first].wparam || record[first].foreground))
    {
        printf( "gw2 test restore: the first WM_NCACTIVATE came after the window was the foreground window, "
                "which the engine's winemac.so doesn't do\n" );
        return 1;
    }
    if (!DestroyWindow( window )) return fail( "DestroyWindow" );
    printf( "gw2 test restore: passed\n" );
    return 0;
}
