/* gw2 (jdrly/gw2-on-mac#101): waits with a timeout end on time, now that msync reads the clock
 * for a relative timeout only where the wait first spins or sleeps.
 *
 * Each of these runs ROUNDS times on objects nobody signals, alertable with no APC queued and not
 * alertable, and has to return WAIT_TIMEOUT from a 50 ms wait:
 *   single    WaitForSingleObjectEx on an event
 *   any       WaitForMultipleObjectsEx on two events, wait-any
 *   all       the same, wait-all
 *   absolute  NtWaitForSingleObject on an event, with an absolute timeout 50 ms ahead
 * These are timing checks, not a proof that the deadline is computed once (the code shows that).
 * Every wait has to take at least 45 ms: a timed wait never ends early. The shortest of the
 * ROUNDS has to take under 150 ms: that catches a deadline that ends 100 ms or more late on every
 * wait, and a wait that never times out, while a pause of the runner makes only some waits long.
 * A deadline restarted once, ending near 100 ms, gets past it.
 * Each of the four, alertable with an APC queued, has to run the APC and return
 * WAIT_IO_COMPLETION ROUNDS times, the shortest of them before 45 ms.
 * A wait with a zero timeout on a semaphore at 0, and a wait-all with a zero timeout on that
 * semaphore and an event, have to return WAIT_TIMEOUT ZEROS times each. Their time is printed;
 * 30 s for all of them catches only a wait that sleeps.
 *
 * Prints "gw2 test timeout: passed" and exits 0, or names what failed and exits 1. */
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>

#define ROUNDS 20
#define ZEROS  100000

enum kind { KIND_SINGLE, KIND_ANY, KIND_ALL, KIND_ABSOLUTE, KIND_NB };
static const char * const names[KIND_NB] = { "single", "any", "all", "absolute" };

static HANDLE events[2], sem;
static LARGE_INTEGER freq;
static LONG (WINAPI *pNtWaitForSingleObject)( HANDLE, BOOLEAN, const LARGE_INTEGER * );
static volatile LONG apcs;

#define FAIL(...) do { \
        printf( "gw2 test timeout: failed, line %d: ", __LINE__ ); \
        printf( __VA_ARGS__ ); \
        printf( "\n" ); \
        exit( 1 ); \
    } while (0)

static double ms_since( const LARGE_INTEGER *t0 )
{
    LARGE_INTEGER t;

    QueryPerformanceCounter( &t );
    return (t.QuadPart - t0->QuadPart) * 1000.0 / freq.QuadPart;
}

/* one wait of 50 ms of <kind> on objects nobody signals */
static DWORD wait_once( enum kind kind, BOOL alertable )
{
    LARGE_INTEGER until;
    FILETIME now;

    switch (kind)
    {
    case KIND_SINGLE: return WaitForSingleObjectEx( events[0], 50, alertable );
    case KIND_ANY: return WaitForMultipleObjectsEx( 2, events, FALSE, 50, alertable );
    case KIND_ALL: return WaitForMultipleObjectsEx( 2, events, TRUE, 50, alertable );
    default:
        /* STATUS_TIMEOUT and STATUS_USER_APC have WAIT_TIMEOUT's and WAIT_IO_COMPLETION's values */
        GetSystemTimeAsFileTime( &now );
        until.LowPart = now.dwLowDateTime;
        until.HighPart = now.dwHighDateTime;
        until.QuadPart += 50 * 10000;
        return pNtWaitForSingleObject( events[0], alertable, &until );
    }
}

static void timed( enum kind kind, BOOL alertable )
{
    const char *how = alertable ? " alertable" : "";
    double ms, lo = 1e9, hi = 0, sum = 0;
    LARGE_INTEGER t0;
    DWORD r;
    int i;

    for (i = 0; i < ROUNDS; i++)
    {
        QueryPerformanceCounter( &t0 );
        r = wait_once( kind, alertable );
        ms = ms_since( &t0 );
        if (r != WAIT_TIMEOUT) FAIL( "%s%s, round %d: returned %#lx, not WAIT_TIMEOUT", names[kind], how, i, r );
        if (ms < 45) FAIL( "%s%s, round %d: timed out after %.1f ms, early", names[kind], how, i, ms );
        if (ms < lo) lo = ms;
        if (ms > hi) hi = ms;
        sum += ms;
    }
    printf( "gw2 test timeout: %s%s: %d timeouts, %.1f ms shortest, %.1f mean, %.1f longest\n",
            names[kind], how, ROUNDS, lo, sum / ROUNDS, hi );
    if (lo >= 150) FAIL( "%s%s: the shortest of %d timeouts took %.1f ms", names[kind], how, ROUNDS, lo );
}

static void CALLBACK apc( ULONG_PTR arg )
{
    InterlockedIncrement( &apcs );
}

static void with_apc( enum kind kind )
{
    double ms, lo = 1e9, hi = 0, sum = 0;
    LARGE_INTEGER t0;
    DWORD r;
    int i;

    for (i = 0; i < ROUNDS; i++)
    {
        apcs = 0;
        if (!QueueUserAPC( apc, GetCurrentThread(), 0 )) FAIL( "QueueUserAPC: error %lu", GetLastError() );
        QueryPerformanceCounter( &t0 );
        r = wait_once( kind, TRUE );
        ms = ms_since( &t0 );
        if (r != WAIT_IO_COMPLETION) FAIL( "%s with an APC, round %d: returned %#lx", names[kind], i, r );
        if (apcs != 1) FAIL( "%s with an APC, round %d: the APC ran %ld times", names[kind], i, apcs );
        if (ms < lo) lo = ms;
        if (ms > hi) hi = ms;
        sum += ms;
    }
    printf( "gw2 test timeout: %s alertable: %d APCs ended the wait, %.3f ms shortest, %.3f mean, %.3f longest\n",
            names[kind], ROUNDS, lo, sum / ROUNDS, hi );
    if (lo >= 45) FAIL( "%s with an APC: the shortest of %d waits took %.1f ms", names[kind], ROUNDS, lo );
}

int main(void)
{
    HANDLE pair[2];
    LARGE_INTEGER t0;
    enum kind k;
    double ms;
    DWORD r;
    int i;

    setvbuf( stdout, NULL, _IONBF, 0 );
    QueryPerformanceFrequency( &freq );
    if (!(pNtWaitForSingleObject = (void *)GetProcAddress( GetModuleHandleA( "ntdll.dll" ), "NtWaitForSingleObject" )))
        FAIL( "no NtWaitForSingleObject" );
    for (i = 0; i < 2; i++)
        if (!(events[i] = CreateEventW( NULL, FALSE, FALSE, NULL ))) FAIL( "CreateEvent: error %lu", GetLastError() );
    if (!(sem = CreateSemaphoreW( NULL, 0, 1, NULL ))) FAIL( "CreateSemaphore: error %lu", GetLastError() );

    for (k = KIND_SINGLE; k < KIND_NB; k++)
    {
        timed( k, FALSE );
        timed( k, TRUE );
        with_apc( k );
    }

    pair[0] = sem;
    pair[1] = events[0];
    QueryPerformanceCounter( &t0 );
    for (i = 0; i < ZEROS; i++)
        if ((r = WaitForSingleObject( sem, 0 )) != WAIT_TIMEOUT) FAIL( "zero timeout, single, round %d: returned %#lx", i, r );
    for (i = 0; i < ZEROS; i++)
        if ((r = WaitForMultipleObjects( 2, pair, TRUE, 0 )) != WAIT_TIMEOUT) FAIL( "zero timeout, all, round %d: returned %#lx", i, r );
    ms = ms_since( &t0 );
    printf( "gw2 test timeout: %d zero-timeout waits, single and wait-all, in %.0f ms\n", 2 * ZEROS, ms );
    if (ms >= 30000) FAIL( "%d zero-timeout waits took %.0f ms", 2 * ZEROS, ms );

    printf( "gw2 test timeout: passed\n" );
    return 0;
}
