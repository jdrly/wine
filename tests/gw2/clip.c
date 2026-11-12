/* gw2 (jdrly/gw2-on-mac#62): ClipCursor with one rectangle, CALLS times, against the Mac driver's
 * WINE_MACDRV_CLIP=changes and WINE_MACDRV_CALL=quiet.
 *
 * Every ClipCursor is a set_cursor request, and the server queues WM_WINE_CLIPCURSOR to the
 * foreground thread whether or not the rectangle changed. That thread's next PeekMessage takes it
 * (get_message), and win32u calls the driver's ClipCursor, which runs on the Cocoa main thread and
 * waits for it. The main thread wakes the caller with a byte in its event pipe, and the caller
 * leaves it there. The server sees the pipe readable, so a later pump drains nothing and sends a
 * set_queue_mask with poll_events. With CLIP=changes only the first two calls go to the main
 * thread: the first starts the clipping, the second finds it. With CALL=quiet no byte stays.
 *
 * The thread first releases a semaphore RELEASES times, so that it is the game thread by the
 * session report's rule (the most releases). It shows a window, makes it the foreground window
 * (up to 5 s, else a failure of its own when counting) and pumps its messages. Without the Mac
 * driver it says so and passes. With WINE_HEAT_COUNT=1 and WINE_HEAT_EXE=clip.exe, each part in a
 * 5 s block of the dumper's of its own:
 * A. CALLS rounds of a 1 ms sleep and pumping its messages until none is left;
 * B. the same with ClipCursor(the window's client area) before each pump.
 * It reads this thread's two blocks in heat-requests-*.log. B must have CALLS set_cursor requests
 * with a clip rectangle and at least CALLS get_message requests, so the clip messages came to this
 * thread. Then by the switches: with WINE_MACDRV_CLIP=changes or WINE_MACDRV_CALL=quiet, B has at
 * most CALLS / 100 more set_queue_mask poll requests than A; with neither, at least CALLS / 2 more,
 * the bytes the switches remove. Without counting it makes the calls once and passes: the matrix
 * runs every program.
 *
 * The workflow counts the driver's "unchanged, not sent" lines (channel cursor) besides, and its
 * "not applied" warnings, which say that the runner refused the clip itself.
 *
 * Prints "gw2 test clip: passed" and exits 0, or names what failed and exits 1. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#define RELEASES 20000
#define CALLS    1000

#define FAIL(...) do { \
        printf( "gw2 test clip: failed, line %d: ", __LINE__ ); \
        printf( __VA_ARGS__ ); \
        printf( "\n" ); \
        exit( 1 ); \
    } while (0)

struct block
{
    long long set_cursor, get_message, clip, internal, poll;
};

static BOOL env_is( const char *name, const char *value )
{
    char buf[256];
    DWORD n = GetEnvironmentVariableA( name, buf, sizeof(buf) );
    return n && n < sizeof(buf) && !_stricmp( buf, value );
}

/* the requests file of this run in $WINE_HEAT_LOG, or FALSE if there is none yet */
static BOOL find_log( char *path, size_t size )
{
    char dir[MAX_PATH];
    WIN32_FIND_DATAA fd;
    HANDLE find;

    if (!GetEnvironmentVariableA( "WINE_HEAT_LOG", dir, sizeof(dir) )) FAIL( "WINE_HEAT_LOG isn't set" );
    snprintf( path, size, "Z:%s/heat-requests-*.log", dir );
    if ((find = FindFirstFileA( path, &fd )) == INVALID_HANDLE_VALUE) return FALSE;
    snprintf( path, size, "Z:%s/%s", dir, fd.cFileName );
    FindClose( find );
    return TRUE;
}

/* the number after " <name> " in <line>, or 0 */
static long long field( const char *line, const char *name )
{
    char key[64];
    const char *p;
    snprintf( key, sizeof(key), " %s ", name );
    return (p = strstr( line, key )) ? _atoi64( p + strlen( key ) ) : 0;
}

/* the number of this thread's blocks in <path>, and the last one's counts in <last> */
static int read_blocks( const char *path, struct block *last )
{
    char want[64], line[512];
    BOOL mine = FALSE;
    int blocks = 0;
    FILE *f;

    if (!(f = fopen( path, "r" ))) FAIL( "can't open %s", path );
    snprintf( want, sizeof(want), "requests thread %04lx ", GetCurrentThreadId() );
    while (fgets( line, sizeof(line), f ))
    {
        if (!strncmp( line, "requests thread ", 16 ))
        {
            if ((mine = !strncmp( line, want, strlen( want ) )))
            {
                blocks++;
                memset( last, 0, sizeof(*last) );
            }
            continue;
        }
        if (!mine) continue;
        if (!strncmp( line, "req set_cursor ", 15 )) last->set_cursor = _atoi64( line + 15 );
        else if (!strncmp( line, "req get_message ", 16 )) last->get_message = _atoi64( line + 16 );
        else if (!strncmp( line, "set_cursor ", 11 )) last->clip = field( line, "clip" );
        else if (!strncmp( line, "get_message ", 12 )) last->internal = field( line, "internal" );
        else if (!strncmp( line, "set_queue_mask ", 15 )) last->poll = field( line, "poll" );
    }
    fclose( f );
    return blocks;
}

/* wait until the dumper has written this thread's block number <blocks>, and return its counts */
static struct block wait_block( const char *path, int blocks )
{
    struct block block;
    DWORD start = GetTickCount();

    while (read_blocks( path, &block ) < blocks)
    {
        if (GetTickCount() - start > 15000) FAIL( "no block %d of this thread in 15 s", blocks );
        Sleep( 20 );
    }
    return block;
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

/* CALLS rounds, with ClipCursor(<clip>) in each if <clip> isn't NULL; returns the milliseconds */
static DWORD rounds( const RECT *clip )
{
    DWORD start = GetTickCount();
    int i;

    for (i = 0; i < CALLS; i++)
    {
        Sleep( 1 );
        if (clip && !ClipCursor( clip )) FAIL( "ClipCursor %d: error %lu", i, GetLastError() );
        pump();
    }
    return GetTickCount() - start;
}

int main( void )
{
    WNDCLASSW cls = { .lpfnWndProc = DefWindowProcW, .lpszClassName = L"gw2 test clip" };
    BOOL counting = env_is( "WINE_HEAT_COUNT", "1" ) && env_is( "WINE_HEAT_EXE", "clip.exe" );
    BOOL removed = env_is( "WINE_MACDRV_CLIP", "changes" ) || env_is( "WINE_MACDRV_CALL", "quiet" );
    char path[MAX_PATH * 2];
    struct block a, b;
    DWORD start, ms;
    RECT clip;
    HANDLE sem;
    HWND hwnd;
    int i, n;

    setvbuf( stdout, NULL, _IONBF, 0 );
    if (!(sem = CreateSemaphoreW( NULL, 0, 0x7fffffff, NULL ))) FAIL( "CreateSemaphore: error %lu", GetLastError() );
    for (i = 0; i < RELEASES; i++)
        if (!ReleaseSemaphore( sem, 1, NULL )) FAIL( "ReleaseSemaphore: error %lu", GetLastError() );
    CloseHandle( sem );

    cls.hInstance = GetModuleHandleW( NULL );
    if (!RegisterClassW( &cls )) FAIL( "RegisterClass: error %lu", GetLastError() );
    hwnd = CreateWindowExW( 0, cls.lpszClassName, L"gw2 test", WS_OVERLAPPEDWINDOW, 200, 200, 400, 300,
                            NULL, NULL, cls.hInstance, NULL );
    if (!GetModuleHandleW( L"winemac.drv" ))
    {
        printf( "gw2 test clip: no Mac driver, nothing to count\n" );
        if (hwnd) DestroyWindow( hwnd );
        printf( "gw2 test clip: passed\n" );
        return 0;
    }
    if (!hwnd) FAIL( "CreateWindowEx: error %lu", GetLastError() );
    ShowWindow( hwnd, SW_SHOWNORMAL );
    /* up to 5 s for the window to become the foreground window, asking again about every 500 ms */
    for (start = GetTickCount(), i = 0; GetForegroundWindow() != hwnd && GetTickCount() - start < 5000; Sleep( 10 ), i++)
    {
        if (i % 50 == 0) SetForegroundWindow( hwnd );
        pump();
    }
    for (start = GetTickCount(); GetTickCount() - start < 250; Sleep( 10 )) pump();
    if (!GetClientRect( hwnd, &clip )) FAIL( "GetClientRect: error %lu", GetLastError() );
    MapWindowPoints( hwnd, NULL, (POINT *)&clip, 2 );
    printf( "gw2 test clip: the window is %sthe foreground window, clipping to %ld,%ld-%ld,%ld\n",
            GetForegroundWindow() == hwnd ? "" : "not ", clip.left, clip.top, clip.right, clip.bottom );
    if (counting && GetForegroundWindow() != hwnd)
        FAIL( "the runner didn't make the window the foreground window in 5 s, so the clip messages would "
              "go to another thread" );

    if (!counting)
    {
        rounds( &clip );
        ClipCursor( NULL );
        pump();
        DestroyWindow( hwnd );
        printf( "gw2 test clip: passed\n" );
        return 0;
    }

    for (start = GetTickCount(); !find_log( path, sizeof(path) ); Sleep( 20 ))
        if (GetTickCount() - start > 15000) FAIL( "the dumper wrote no heat-requests-*.log in 15 s" );
    /* start each part right after a block of this thread */
    wait_block( path, 1 );
    n = read_blocks( path, &a );
    wait_block( path, n + 1 );
    if ((ms = rounds( NULL )) > 4000) FAIL( "part A took %lu ms, too long for one block", ms );
    a = wait_block( path, n + 2 );
    if ((ms = rounds( &clip )) > 4000) FAIL( "part B took %lu ms, too long for one block", ms );
    b = wait_block( path, n + 3 );
    ClipCursor( NULL );
    pump();
    DestroyWindow( hwnd );

    printf( "gw2 test clip: A: set_cursor %lld get_message %lld (internal %lld) set_queue_mask poll %lld\n",
            a.set_cursor, a.get_message, a.internal, a.poll );
    printf( "gw2 test clip: B: set_cursor %lld (clip %lld) get_message %lld (internal %lld) set_queue_mask poll %lld\n",
            b.set_cursor, b.clip, b.get_message, b.internal, b.poll );
    if (b.clip != CALLS) FAIL( "%lld set_cursor requests with a clip rectangle in B, not %d", b.clip, CALLS );
    if (b.get_message < CALLS) FAIL( "%lld get_message requests in B: the clip messages didn't come to this thread",
                                     b.get_message );
    if (removed && b.poll - a.poll > CALLS / 100)
        FAIL( "B has %lld more set_queue_mask poll requests than A, with WINE_MACDRV_CLIP=changes or "
              "WINE_MACDRV_CALL=quiet", b.poll - a.poll );
    if (!removed && b.poll - a.poll < CALLS / 2)
        FAIL( "B has only %lld more set_queue_mask poll requests than A without the switches: the test doesn't "
              "see the byte", b.poll - a.poll );
    printf( "gw2 test clip: B has %lld more set_queue_mask poll requests than A\n", b.poll - a.poll );
    printf( "gw2 test clip: passed\n" );
    return 0;
}
