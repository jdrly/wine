/* gw2 (jdrly/gw2-on-mac#61): what SetCursor returns, which WINE_WIN32U_SETCURSOR=shm in my
 * win32u.so has to keep while it answers a repeated cursor from shared memory.
 *
 * Two cursors of its own, a and b, and in order:
 * 1. a once, then a 1,000 times: each returns a.
 * 2. b returns a, and b again returns b.
 * 3. NULL returns b, NULL again returns NULL, then b returns NULL and b again b.
 * 4. A second thread with an input of its own: a returns NULL, a again a. The main thread attaches
 *    its input to the second's, where the cursor is a: b returns a, which a stale read of the old
 *    input would answer with b. The second thread repeats b (returns b) and sets a (returns b);
 *    the main thread's a then returns a and its b returns a. Detached, the main thread has a new
 *    input without a cursor: a returns NULL, and a again a. GetCursor returns a.
 * None of it needs a window, only the threads' queues. The program checks the answers only; the
 * requests the repeats leave out are requests.c's to count.
 *
 * Prints "gw2 test setcursor: passed" and exits 0, or names what failed and exits 1. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#define REPEATS 1000

#define FAIL(...) do { \
        printf( "gw2 test setcursor: failed, line %d: ", __LINE__ ); \
        printf( __VA_ARGS__ ); \
        printf( "\n" ); \
        exit( 1 ); \
    } while (0)

static HCURSOR a, b;
static HANDLE go, done;
static volatile LONG step;

static HCURSOR cursor( BYTE fill )
{
    BYTE mask[32 * 32 / 8], bits[32 * 32 / 8];
    HCURSOR c;

    memset( mask, fill, sizeof(mask) );
    memset( bits, 0, sizeof(bits) );
    if (!(c = CreateCursor( GetModuleHandleW( NULL ), 0, 0, 32, 32, mask, bits )))
        FAIL( "CreateCursor: error %lu", GetLastError() );
    return c;
}

static void expect( const char *what, HCURSOR c, HCURSOR want )
{
    HCURSOR prev = SetCursor( c );
    if (prev != want) FAIL( "%s: SetCursor(%p) returned %p, not %p", what, c, prev, want );
}

/* the second thread: its queue, then one step each time the main thread asks */
static DWORD WINAPI second( void *arg )
{
    MSG msg;

    PeekMessageW( &msg, NULL, 0, 0, PM_NOREMOVE );
    SetEvent( done );
    for (;;)
    {
        if (WaitForSingleObject( go, 10000 )) FAIL( "the second thread waited 10 s for a step" );
        switch (step)
        {
        case 1:
            expect( "4, the second thread's own input", a, NULL );
            expect( "4, the second thread's own input, a again", a, a );
            break;
        case 2:
            expect( "4, attached, the second thread repeats the main thread's b", b, b );
            expect( "4, attached, the second thread sets a", a, b );
            break;
        default:
            return 0;
        }
        SetEvent( done );
    }
}

static void on_second( LONG s )
{
    step = s;
    SetEvent( go );
    if (WaitForSingleObject( done, 10000 )) FAIL( "step %ld of the second thread didn't end in 10 s", s );
}

int main(void)
{
    HANDLE thread;
    DWORD tid;
    MSG msg;
    int i;

    setvbuf( stdout, NULL, _IONBF, 0 );
    a = cursor( 0xff );
    b = cursor( 0x0f );
    PeekMessageW( &msg, NULL, 0, 0, PM_NOREMOVE );  /* this thread's queue */

    /* 1: the first call's answer depends on what set this thread's input before; not checked */
    SetCursor( a );
    for (i = 0; i < REPEATS; i++) expect( "1, a repeated", a, a );

    /* 2 */
    expect( "2, b after a", b, a );
    expect( "2, b again", b, b );

    /* 3 */
    expect( "3, NULL after b", NULL, b );
    expect( "3, NULL again", NULL, NULL );
    expect( "3, b after NULL", b, NULL );
    expect( "3, b again", b, b );

    /* 4 */
    if (!(go = CreateEventW( NULL, FALSE, FALSE, NULL )) || !(done = CreateEventW( NULL, FALSE, FALSE, NULL )))
        FAIL( "CreateEvent: error %lu", GetLastError() );
    if (!(thread = CreateThread( NULL, 0, second, NULL, 0, &tid ))) FAIL( "CreateThread: error %lu", GetLastError() );
    if (WaitForSingleObject( done, 10000 )) FAIL( "the second thread had no queue in 10 s" );
    on_second( 1 );
    if (!AttachThreadInput( GetCurrentThreadId(), tid, TRUE )) FAIL( "AttachThreadInput: error %lu", GetLastError() );
    expect( "4, attached to the second thread's input, where the cursor is a", b, a );
    expect( "4, attached, b again", b, b );
    on_second( 2 );
    expect( "4, attached, the main thread repeats the second thread's a", a, a );
    expect( "4, attached, the main thread sets b", b, a );
    if (!AttachThreadInput( GetCurrentThreadId(), tid, FALSE )) FAIL( "AttachThreadInput: detach: error %lu", GetLastError() );
    expect( "4, detached, a new input without a cursor", a, NULL );
    expect( "4, detached, a again", a, a );
    if (GetCursor() != a) FAIL( "GetCursor returned %p, not %p", GetCursor(), a );
    step = 3;  /* the second thread's end: it returns without setting done */
    SetEvent( go );
    if (WaitForSingleObject( thread, 10000 )) FAIL( "the second thread didn't end in 10 s" );

    printf( "gw2 test setcursor: passed\n" );
    return 0;
}
