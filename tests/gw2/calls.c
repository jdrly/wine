/* gw2 (jdrly/gw2-on-mac#62): synchronous calls to the Cocoa main thread from several threads while
 * events arrive, for WINE_MACDRV_CALL=quiet, where a call's end wakes its caller through the
 * caller's kqueue instead of a byte in its event pipe. A lost wakeup leaves a caller waiting
 * forever, so the test counts calls and a watchdog fails it when a thread stops moving.
 *
 * With GW2_TEST_CALLS=1 and the Mac driver: THREADS threads, each with a window of its own (so it
 * has an event queue), make CALLS SetCursorPos calls each, between two points so that every call
 * moves the cursor and reaches the driver (win32u calls it only for a move), and each is a
 * synchronous call to the main thread. Every BURST calls a thread activates its window and moves
 * it, and pumps its messages after every call, so focus and frame events reach the queues while
 * the other threads are waiting in their calls. The main thread waits for them. If a thread that
 * is still running makes no call for STUCK_MS, or the threads aren't done in DEADLINE_MS, it
 * prints how far each got and fails. Without GW2_TEST_CALLS, or without the Mac driver, it says
 * so and passes: the matrix runs every program.
 *
 * It prints "workers start" and "workers done" around the threads' calls. The workflow counts the
 * driver's handled events (channel event) between the two, so the run shows that events arrived
 * while the threads made their calls. That makes it a stress test: it doesn't force any one
 * interleaving, such as a wake that comes before the caller waits.
 *
 * Prints "gw2 test calls: passed" and exits 0, or names what failed and exits 1. */
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>

#define THREADS  4
#define CALLS    2000
#define BURST    100
#define STUCK_MS 10000
#define DEADLINE_MS 120000

static volatile LONG done[THREADS];
static volatile LONG failed;

static void pump(void)
{
    MSG msg;

    while (PeekMessageW( &msg, NULL, 0, 0, PM_REMOVE ))
    {
        TranslateMessage( &msg );
        DispatchMessageW( &msg );
    }
}

static DWORD WINAPI worker( void *arg )
{
    int n = (int)(INT_PTR)arg, i, x = 100 + 220 * n, y = 300;
    HWND hwnd = CreateWindowExW( 0, L"gw2 test calls", L"gw2 test", WS_OVERLAPPEDWINDOW, x, y - 100, 200, 150,
                                 NULL, NULL, GetModuleHandleW( NULL ), NULL );

    if (!hwnd)
    {
        printf( "gw2 test calls: thread %d: CreateWindowEx failed (error %lu)\n", n, GetLastError() );
        InterlockedExchange( &failed, 1 );
        return 1;
    }
    ShowWindow( hwnd, SW_SHOWNORMAL );
    for (i = 0; i < CALLS; i++)
    {
        if (i % BURST == 0)
        {
            SetForegroundWindow( hwnd );
            SetWindowPos( hwnd, NULL, x + (i / BURST % 2) * 10, y - 100, 0, 0, SWP_NOSIZE | SWP_NOZORDER );
        }
        if (!SetCursorPos( x + 20 + i % 2, y ))
        {
            printf( "gw2 test calls: thread %d: SetCursorPos %d failed (error %lu)\n", n, i, GetLastError() );
            InterlockedExchange( &failed, 1 );
            break;
        }
        pump();
        InterlockedExchange( &done[n], i + 1 );
    }
    DestroyWindow( hwnd );
    return 0;
}

int main( void )
{
    WNDCLASSW cls = { .lpfnWndProc = DefWindowProcW, .lpszClassName = L"gw2 test calls" };
    LONG last[THREADS] = {0};
    DWORD moved[THREADS], start, now, rc;
    HANDLE threads[THREADS];
    const char *why;
    HWND probe;
    int i;

    setvbuf( stdout, NULL, _IONBF, 0 );
    if (!getenv( "GW2_TEST_CALLS" ))
    {
        printf( "gw2 test calls: GW2_TEST_CALLS is unset, nothing to stress\n" );
        printf( "gw2 test calls: passed\n" );
        return 0;
    }
    cls.hInstance = GetModuleHandleW( NULL );
    if (!RegisterClassW( &cls )) { printf( "gw2 test calls: RegisterClass failed\n" ); return 1; }
    probe = CreateWindowExW( 0, cls.lpszClassName, L"gw2 probe", WS_POPUP, 0, 0, 1, 1, NULL, NULL, cls.hInstance, NULL );
    if (!GetModuleHandleW( L"winemac.drv" ))
    {
        printf( "gw2 test calls: no Mac driver, nothing to stress\n" );
        if (probe) DestroyWindow( probe );
        printf( "gw2 test calls: passed\n" );
        return 0;
    }
    if (probe) DestroyWindow( probe );

    printf( "gw2 test calls: workers start\n" );
    start = GetTickCount();
    for (i = 0; i < THREADS; i++)
    {
        moved[i] = start;
        if (!(threads[i] = CreateThread( NULL, 0, worker, (void *)(INT_PTR)i, 0, NULL )))
        {
            printf( "gw2 test calls: CreateThread failed (error %lu)\n", GetLastError() );
            return 1;
        }
    }
    for (;;)
    {
        rc = WaitForMultipleObjects( THREADS, threads, TRUE, 200 );
        if (rc < WAIT_OBJECT_0 + THREADS) break;
        now = GetTickCount();
        why = NULL;
        for (i = 0; i < THREADS; i++)
        {
            if (done[i] != last[i])
            {
                last[i] = done[i];
                moved[i] = now;
            }
            else if (now - moved[i] > STUCK_MS && WaitForSingleObject( threads[i], 0 ) == WAIT_TIMEOUT)
                why = "a running thread made no call in 10 s: a caller is waiting for a wakeup that never came";
        }
        if (!why && now - start > DEADLINE_MS) why = "the threads weren't done in 120 s";
        if (why)
        {
            for (i = 0; i < THREADS; i++)
                printf( "gw2 test calls: thread %d made %ld of %d calls\n", i, done[i], CALLS );
            printf( "gw2 test calls: %s\n", why );
            ExitProcess( 1 );
        }
    }
    printf( "gw2 test calls: workers done\n" );
    if (failed) return 1;
    printf( "gw2 test calls: %d threads made %d synchronous calls each in %lu ms\n", THREADS, CALLS,
            GetTickCount() - start );
    printf( "gw2 test calls: passed\n" );
    return 0;
}
