/* gw2 (jdrly/gw2-on-mac#219): a wait-all that takes an object and then has to put it back must
 * wake the threads that looked at the object meanwhile.
 *
 * P polls a wait-all on {T, E, E2} with a zero timeout. T is the token under test: a semaphore
 * with one count, an auto-reset event, or a mutex. E is an auto-reset event that stays set, and E2
 * a second handle to it. msync's wait-all can never complete on that: each pass takes T, takes E
 * through the first handle, finds it gone through the second, and puts both back. W takes T and
 * gives it back in a loop. Whenever W looks while P has T, W goes to sleep on it, and P's put-back
 * has to wake it. msync put T back without a wake call, so W slept on with T free: nothing else
 * releases T, so W's wait ran into its timeout.
 *
 * The engine's own ntdll.so fails this; mine has to pass it in every WINE_HEAT_WAKE mode. Each
 * kind of T runs until W has taken it TAKES times, SECONDS at most, or until the first violation.
 * Windows refuses a wait-all with two handles to one object, so this is a test of Wine's msync
 * only. Prints "gw2 test putback: passed" and exits 0, or names what failed and exits 1. */
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>

#define TAKES 2000
#define SECONDS 20
#define STALL_MS 500  /* P has T for microseconds at a time: after this long W was not woken */
#define HELD_MAX 20   /* W's waits that timed out with T really gone: a runner that stalled P */

enum kind { SEMAPHORE, EVENT, MUTEX, KINDS };
static const char * const kind_names[KINDS] = { "a semaphore", "an auto-reset event", "a mutex" };

static enum kind kind;
static HANDLE token, event, event2;
static volatile LONG stop, lost, bad_wait, bad_give;
static volatile DWORD bad_result;
static volatile LONG64 polls, takes, held;

static BOOL give(void)
{
    switch (kind)
    {
    case SEMAPHORE: return ReleaseSemaphore( token, 1, NULL );
    case EVENT:     return SetEvent( token );
    default:        return ReleaseMutex( token );
    }
}

/* P: the wait-all that can't complete */
static DWORD WINAPI poller( void *arg )
{
    HANDLE objs[3] = { token, event, event2 };
    DWORD r;

    while (!stop)
    {
        r = WaitForMultipleObjects( 3, objs, TRUE, 0 );
        InterlockedIncrement64( &polls );
        if (r != WAIT_TIMEOUT)
        {
            bad_result = r;
            bad_wait = stop = 1;
        }
    }
    return 0;
}

/* W: takes the token and gives it back */
static DWORD WINAPI waiter( void *arg )
{
    DWORD r;
    int i;

    while (!stop && takes < TAKES)
    {
        r = WaitForSingleObject( token, STALL_MS );
        if (r == WAIT_TIMEOUT)
        {
            /* P never has the token for long, so it is free now or about to be. Taking it here
             * means it was there while this thread slept on it. */
            for (i = 0; i < 100 && r == WAIT_TIMEOUT; i++) r = WaitForSingleObject( token, 0 );
            if (r == WAIT_OBJECT_0)
            {
                lost = stop = 1;
                give();
                return 0;
            }
            if (r == WAIT_TIMEOUT)
            {
                if (InterlockedIncrement64( &held ) > HELD_MAX) stop = 1;
                continue;
            }
        }
        if (r != WAIT_OBJECT_0)
        {
            bad_result = r;
            bad_wait = stop = 1;
            return 0;
        }
        InterlockedIncrement64( &takes );
        if (!give())
        {
            bad_result = GetLastError();
            bad_give = stop = 1;
        }
    }
    return 0;
}

static int run_kind(void)
{
    HANDLE threads[2];
    DWORD t0, ms;
    int i;

    stop = lost = bad_wait = bad_give = 0;
    polls = takes = held = 0;
    switch (kind)
    {
    case SEMAPHORE: token = CreateSemaphoreW( NULL, 1, 1, NULL ); break;
    case EVENT:     token = CreateEventW( NULL, FALSE, TRUE, NULL ); break;
    default:        token = CreateMutexW( NULL, FALSE, NULL ); break;
    }
    event = CreateEventW( NULL, FALSE, TRUE, NULL );
    if (!token || !event) return 3;
    if (!DuplicateHandle( GetCurrentProcess(), event, GetCurrentProcess(), &event2, 0, FALSE, DUPLICATE_SAME_ACCESS ))
        return 3;
    threads[0] = CreateThread( NULL, 0, poller, NULL, 0, NULL );
    threads[1] = CreateThread( NULL, 0, waiter, NULL, 0, NULL );
    if (!threads[0] || !threads[1]) return 3;

    for (t0 = GetTickCount(); !stop && takes < TAKES && GetTickCount() - t0 < SECONDS * 1000;) Sleep( 10 );
    ms = GetTickCount() - t0;
    stop = 1;
    if (WaitForSingleObject( threads[1], 10000 ) != WAIT_OBJECT_0)
    {
        printf( "gw2 test putback: failed: %s: the waiter didn't end\n", kind_names[kind] );
        return 1;
    }
    /* P's wait-all loops inside ntdll while T and E are both there, and a pass may set E again
     * after this reset: reset it until P is out */
    for (i = 0; i < 1000 && WaitForSingleObject( threads[0], 10 ) == WAIT_TIMEOUT; i++) ResetEvent( event );
    if (i == 1000)
    {
        printf( "gw2 test putback: failed: %s: the wait-all didn't return\n", kind_names[kind] );
        return 1;
    }

    printf( "gw2 test putback: %s: %lld takes, %lld wait-alls, %lld waits with the token gone, %lu ms\n",
            kind_names[kind], takes, polls, held, ms );
    if (lost)
    {
        printf( "gw2 test putback: failed: %s: the waiter slept on with the token put back, after %lld takes\n",
                kind_names[kind], takes );
        return 1;
    }
    if (bad_wait)
    {
        printf( "gw2 test putback: failed: %s: a wait returned %#lx\n", kind_names[kind], bad_result );
        return 1;
    }
    if (bad_give)
    {
        printf( "gw2 test putback: failed: %s: giving the token back failed (error %lu)\n", kind_names[kind], bad_result );
        return 1;
    }
    if (held > HELD_MAX)
    {
        printf( "gw2 test putback: failed: %s: the token stayed away for %d waits of %d ms\n", kind_names[kind], HELD_MAX + 1, STALL_MS );
        return 1;
    }
    if (takes < TAKES)
    {
        printf( "gw2 test putback: failed: %s: only %lld takes in %d s\n", kind_names[kind], takes, SECONDS );
        return 1;
    }
    CloseHandle( threads[0] );
    CloseHandle( threads[1] );
    CloseHandle( event2 );
    CloseHandle( event );
    CloseHandle( token );
    return 0;
}

int main(void)
{
    int ret;

    setvbuf( stdout, NULL, _IONBF, 0 );
    printf( "gw2 test putback: P polls a wait-all on {T, E, E2} that can't complete, W takes T and gives it back\n" );
    for (kind = SEMAPHORE; kind < KINDS; kind++)
        if ((ret = run_kind())) return ret;
    printf( "gw2 test putback: passed\n" );
    return 0;
}
