/* gw2 (jdrly/gw2-on-mac#44): an alertable wait-any on two auto-reset events against a setter.
 *
 * W waits on both events, alertably, ROUNDS times. Each round the setter waits until W is about
 * to wait, then sets one of the events after a delay that sweeps 0 to about 13 us, so that some
 * sets land while W's wait is registering with the server: then msync's contention bailout runs.
 * Every round has to end with W taking that set, within 2 s: the same number of waits completes
 * and no wake is lost. That has to hold with WINE_HEAT_MSYNC_UNREG unset (the engine's ntdll.so,
 * and mine) and set.
 *
 * With WINE_HEAT_COUNT=1 and WINE_HEAT_EXE=unreg.exe, W's row in heat.c's log has to show unreg
 * above 0 with WINE_HEAT_MSYNC_UNREG=1, so the run proves it reached the bailout, and 0 without.
 *
 * Prints "gw2 test unreg: passed" and exits 0, or names what failed and exits 1. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#define ROUNDS 50000
#define SWEEP  128  /* delays of 0 to SWEEP - 1 ticks of the performance counter */

static HANDLE events[2];
static volatile LONG armed, taken, failed_round = -1;
static volatile DWORD failed_result;

#define FAIL(...) do { \
        printf( "gw2 test unreg: failed, line %d: ", __LINE__ ); \
        printf( __VA_ARGS__ ); \
        printf( "\n" ); \
        exit( 1 ); \
    } while (0)

static DWORD WINAPI waiter( void *arg )
{
    DWORD r;
    LONG i;

    for (i = 0; i < ROUNDS; i++)
    {
        InterlockedExchange( &armed, i + 1 );
        r = WaitForMultipleObjectsEx( 2, events, FALSE, 2000, TRUE );
        if (r != WAIT_OBJECT_0 && r != WAIT_OBJECT_0 + 1)
        {
            failed_result = r;
            failed_round = i;
            return 1;
        }
        InterlockedExchange( &taken, i + 1 );
    }
    return 0;
}

/* spins until *value is want, or fails the round after ms */
static void spin_until( volatile LONG *value, LONG want, DWORD ms, const char *what, LONG round )
{
    DWORD t0 = GetTickCount();
    while (*value != want)
    {
        if (failed_round >= 0) FAIL( "round %ld: the wait returned %#lx", failed_round, failed_result );
        if (GetTickCount() - t0 > ms) FAIL( "round %ld: %s", round, what );
        YieldProcessor();
    }
}

static BOOL env_is( const char *name, const char *value )
{
    char buf[256];
    DWORD n = GetEnvironmentVariableA( name, buf, sizeof(buf) );
    return n && n < sizeof(buf) && !_stricmp( buf, value );
}

/* the counter unreg in W's row of this process's heat log, -1 if there is none */
static long long unreg_count( DWORD tid )
{
    char dir[MAX_PATH], path[MAX_PATH * 2], want[32], line[4096];
    const char *p;
    WIN32_FIND_DATAA fd;
    HANDLE find;
    long long n = -1;
    FILE *f;

    if (!GetEnvironmentVariableA( "WINE_HEAT_LOG", dir, sizeof(dir) )) FAIL( "WINE_HEAT_LOG isn't set" );
    snprintf( want, sizeof(want), "thread %04lx ", tid );
    snprintf( path, sizeof(path), "Z:%s/heat-*.log", dir );
    if ((find = FindFirstFileA( path, &fd )) == INVALID_HANDLE_VALUE) FAIL( "no heat-*.log in %s", dir );
    do
    {
        snprintf( path, sizeof(path), "Z:%s/%s", dir, fd.cFileName );
        if (!(f = fopen( path, "r" ))) continue;
        while (n < 0 && fgets( line, sizeof(line), f ))
            if (!strncmp( line, want, strlen( want ) ) && (p = strstr( line, " unreg " )))
                n = _atoi64( p + strlen( " unreg " ) );
        fclose( f );
    } while (n < 0 && FindNextFileA( find, &fd ));
    FindClose( find );
    return n;
}

int main(void)
{
    LARGE_INTEGER t, until;
    HANDLE w;
    DWORD tid, code;
    LONG i;
    long long unreg;
    BOOL on;

    setvbuf( stdout, NULL, _IONBF, 0 );
    for (i = 0; i < 2; i++)
        if (!(events[i] = CreateEventW( NULL, FALSE, FALSE, NULL ))) FAIL( "CreateEvent: error %lu", GetLastError() );
    if (!(w = CreateThread( NULL, 0, waiter, NULL, 0, &tid ))) FAIL( "CreateThread: error %lu", GetLastError() );

    for (i = 0; i < ROUNDS; i++)
    {
        spin_until( &armed, i + 1, 2000, "the waiter didn't get to its wait", i );
        QueryPerformanceCounter( &until );
        until.QuadPart += i % SWEEP;
        do QueryPerformanceCounter( &t ); while (t.QuadPart < until.QuadPart);
        if (!SetEvent( events[i & 1] )) FAIL( "SetEvent: error %lu", GetLastError() );
        spin_until( &taken, i + 1, 2000, "the waiter missed the set: a lost wake", i );
    }
    if (WaitForSingleObject( w, 10000 ) != WAIT_OBJECT_0) FAIL( "the waiter didn't end" );
    GetExitCodeThread( w, &code );
    if (code) FAIL( "round %ld: the wait returned %#lx", failed_round, failed_result );
    for (i = 0; i < 2; i++)
        if (WaitForSingleObject( events[i], 0 ) != WAIT_TIMEOUT) FAIL( "event %ld was left set", i );
    printf( "gw2 test unreg: %d waits completed, each taking its set\n", ROUNDS );

    /* counting: WINE_HEAT_COUNT=1, and WINE_HEAT_EXE names this program */
    if (env_is( "WINE_HEAT_COUNT", "1" ) && env_is( "WINE_HEAT_EXE", "unreg.exe" ))
    {
        on = env_is( "WINE_HEAT_MSYNC_UNREG", "1" );
        Sleep( 6000 );  /* the counters are written every 5 s */
        if ((unreg = unreg_count( tid )) < 0) FAIL( "no row with unreg for the waiting thread" );
        printf( "gw2 test unreg: WINE_HEAT_MSYNC_UNREG %s, the waiter's unreg %lld\n", on ? "1" : "unset", unreg );
        if (on && !unreg) FAIL( "no wait reached the bailout" );
        if (!on && unreg) FAIL( "unreg counted with the switch unset" );
    }
    printf( "gw2 test unreg: passed\n" );
    return 0;
}
