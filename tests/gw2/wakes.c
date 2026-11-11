/* gw2 (jdrly/gw2-on-mac#40): the semaphore wake policy's tests, 0.1.x's cases
 * (gw2-on-mac's docs/research/heat-locks-2026-10-29.md, heat-r7-2026-11-04.md). They pass in
 * every mode of WINE_HEAT_WAKE (off, one, skip, one,skip), and under the engine's own ntdll.so.
 *
 *   13  20,000 single jobs to 7 workers that sleep between them, all taken. A producer that
 *       outpaces its consumer for a minute, every release taken, no stall.
 *   14  A waiter on a duplicated handle and a release through it. A named semaphore opened
 *       twice. A semaphore duplicated into a child process, released by the parent after
 *       300 ms: the child wakes (with a real handle to this process and with the pseudo-handle).
 *   15  A wait-all on a semaphore and an event against a single waiter on the same semaphore,
 *       200 rounds. 20,000 releases to waiters with 1 ms timeouts, each taken once.
 *   16  A release with no waiter, a million times, on a thread of its own. With
 *       WINE_HEAT_COUNT=1 in this process, that thread's counters must say: with skip,
 *       wake_skipped equals the releases and wake_none is 0; without, every release made a wake
 *       call that found no waiter.
 *
 * Each case prints its name first; a stall shows as the run's time limit with the last name
 * printed. Prints "gw2 test wakes: passed" and exits 0, or names what failed and exits 1.
 * "wakes.exe child" is the child of 14. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#define WORKERS 7
#define JOBS 20000
#define PRODUCER_SECONDS 60
#define ROUNDS 200
#define NO_WAITER 1000000

static const char *current = "startup";

#define FAIL(...) do { \
        printf( "gw2 test wakes: %s failed, line %d: ", current, __LINE__ ); \
        printf( __VA_ARGS__ ); \
        printf( "\n" ); \
        fflush( stdout ); \
        exit( 1 ); \
    } while (0)

static void start( const char *name )
{
    current = name;
    printf( "gw2 test wakes: %s\n", name );
    fflush( stdout );
}

static HANDLE thread( LPTHREAD_START_ROUTINE func, void *arg )
{
    HANDLE h = CreateThread( NULL, 0, func, arg, 0, NULL );
    if (!h) FAIL( "CreateThread: error %lu", GetLastError() );
    return h;
}

static void join( HANDLE h, DWORD ms )
{
    DWORD code;
    if (WaitForSingleObject( h, ms ) != WAIT_OBJECT_0) FAIL( "a thread didn't end within %lu ms", ms );
    if (!GetExitCodeThread( h, &code ) || code) FAIL( "a thread ended with %lu", code );
    CloseHandle( h );
}

static HANDLE private_semaphore( LONG max )
{
    HANDLE h = CreateSemaphoreW( NULL, 0, max, NULL );
    if (!h) FAIL( "CreateSemaphore: error %lu", GetLastError() );
    return h;
}

static void release( HANDLE sem, LONG count )
{
    if (!ReleaseSemaphore( sem, count, NULL )) FAIL( "ReleaseSemaphore: error %lu", GetLastError() );
}

/* waits until *value reaches want, or fails after ms */
static void await( volatile LONG *value, LONG want, DWORD ms )
{
    DWORD t0 = GetTickCount();
    while (*value < want)
    {
        if (GetTickCount() - t0 > ms) FAIL( "%ld of %ld taken after %lu ms", *value, want, ms );
        Sleep( 1 );
    }
}

/* 13: jobs to sleeping workers */

static HANDLE jobs_sem;
static volatile LONG jobs_taken, jobs_done;

static DWORD WINAPI job_worker( void *arg )
{
    LONG n;
    for (;;)
    {
        if (WaitForSingleObject( jobs_sem, 10000 ) != WAIT_OBJECT_0) return 1;
        if (jobs_done) return 0;
        n = InterlockedIncrement( &jobs_taken );
        if (!(n % 64)) Sleep( 1 );
    }
}

static void test_jobs(void)
{
    HANDLE workers[WORKERS];
    int i;

    start( "13 jobs" );
    jobs_sem = private_semaphore( JOBS + WORKERS );
    for (i = 0; i < WORKERS; i++) workers[i] = thread( job_worker, NULL );
    for (i = 0; i < JOBS; i++)
    {
        release( jobs_sem, 1 );
        if (!(i % 50)) Sleep( 1 );  /* the workers go back to sleep on the semaphore */
    }
    await( &jobs_taken, JOBS, 30000 );
    jobs_done = 1;
    release( jobs_sem, WORKERS );
    for (i = 0; i < WORKERS; i++) join( workers[i], 10000 );
    if (jobs_taken != JOBS) FAIL( "%ld taken, not %d", jobs_taken, JOBS );
    CloseHandle( jobs_sem );
}

/* 13: a producer faster than its consumer */

static HANDLE fast_sem;
static volatile LONG fast_taken, fast_done;

static DWORD WINAPI slow_consumer( void *arg )
{
    for (;;)
    {
        if (WaitForSingleObject( fast_sem, 5000 ) != WAIT_OBJECT_0) return 1;
        if (fast_done) return 0;
        InterlockedIncrement( &fast_taken );
    }
}

static void test_producer(void)
{
    LONG released = 0;
    DWORD t0;
    HANDLE consumer;

    start( "13 producer" );
    fast_sem = private_semaphore( 10000 );
    consumer = thread( slow_consumer, NULL );
    for (t0 = GetTickCount(); GetTickCount() - t0 < PRODUCER_SECONDS * 1000;)
    {
        if (ReleaseSemaphore( fast_sem, 1, NULL )) released++;
        else if (GetLastError() == ERROR_TOO_MANY_POSTS) SwitchToThread();
        else FAIL( "ReleaseSemaphore: error %lu", GetLastError() );
    }
    await( &fast_taken, released, 10000 );
    fast_done = 1;
    release( fast_sem, 1 );
    join( consumer, 10000 );
    if (fast_taken != released) FAIL( "%ld taken of %ld released", fast_taken, released );
    printf( "gw2 test wakes: %ld releases in %d s\n", released, PRODUCER_SECONDS );
    CloseHandle( fast_sem );
}

/* 14: duplicated and named semaphores */

static DWORD WINAPI wait_one( void *sem )
{
    return WaitForSingleObject( sem, 5000 ) != WAIT_OBJECT_0;
}

/* a thread waits on <wait>, and after it has gone to sleep the release goes through <signal> */
static void wake_through( HANDLE wait, HANDLE signal )
{
    HANDLE waiter = thread( wait_one, wait );
    Sleep( 50 );
    release( signal, 1 );
    join( waiter, 10000 );
}

static void test_duplicates(void)
{
    WCHAR name[64];
    HANDLE sem, dup, named, opened;

    start( "14 duplicated handle" );
    sem = private_semaphore( 10 );
    if (!DuplicateHandle( GetCurrentProcess(), sem, GetCurrentProcess(), &dup, 0, FALSE, DUPLICATE_SAME_ACCESS ))
        FAIL( "DuplicateHandle: error %lu", GetLastError() );
    wake_through( sem, dup );
    wake_through( dup, sem );
    CloseHandle( dup );
    CloseHandle( sem );

    start( "14 named semaphore" );
    swprintf( name, ARRAYSIZE(name), L"gw2-wakes-%lu", GetCurrentProcessId() );
    if (!(named = CreateSemaphoreW( NULL, 0, 10, name ))) FAIL( "CreateSemaphore: error %lu", GetLastError() );
    if (!(opened = OpenSemaphoreW( SYNCHRONIZE | SEMAPHORE_MODIFY_STATE, FALSE, name )))
        FAIL( "OpenSemaphore: error %lu", GetLastError() );
    wake_through( opened, named );
    wake_through( named, opened );
    CloseHandle( opened );
    CloseHandle( named );
}

/* 14: into a child process. The child reads the handle's value from its stdin. */

static int child(void)
{
    HANDLE sem;
    DWORD got, ret;

    if (!ReadFile( GetStdHandle( STD_INPUT_HANDLE ), &sem, sizeof(sem), &got, NULL ) || got != sizeof(sem))
        return 2;
    ret = WaitForSingleObject( sem, 1500 );
    return ret != WAIT_OBJECT_0;
}

static void test_child( BOOL real )
{
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
    WCHAR cmd[MAX_PATH + 16];
    HANDLE sem, in_read, in_write, self, target;
    DWORD written, code, t0;

    start( real ? "14 duplicated into a child, real handle" : "14 duplicated into a child, pseudo-handle" );
    sem = private_semaphore( 10 );
    if (!CreatePipe( &in_read, &in_write, &sa, 0 )) FAIL( "CreatePipe: error %lu", GetLastError() );
    SetHandleInformation( in_write, HANDLE_FLAG_INHERIT, 0 );
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = in_read;
    si.hStdOutput = GetStdHandle( STD_OUTPUT_HANDLE );
    si.hStdError = GetStdHandle( STD_ERROR_HANDLE );
    cmd[0] = '"';
    GetModuleFileNameW( NULL, cmd + 1, MAX_PATH );
    wcscat( cmd, L"\" child" );
    if (!CreateProcessW( NULL, cmd, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi ))
        FAIL( "CreateProcess: error %lu", GetLastError() );
    CloseHandle( in_read );

    self = real ? OpenProcess( PROCESS_DUP_HANDLE, FALSE, GetCurrentProcessId() ) : GetCurrentProcess();
    if (!self) FAIL( "OpenProcess: error %lu", GetLastError() );
    if (!DuplicateHandle( self, sem, pi.hProcess, &target, 0, FALSE, DUPLICATE_SAME_ACCESS ))
        FAIL( "DuplicateHandle: error %lu", GetLastError() );
    if (!WriteFile( in_write, &target, sizeof(target), &written, NULL )) FAIL( "WriteFile: error %lu", GetLastError() );

    Sleep( 300 );
    t0 = GetTickCount();
    release( sem, 1 );
    if (WaitForSingleObject( pi.hProcess, 5000 ) != WAIT_OBJECT_0) FAIL( "the child didn't end" );
    if (!GetExitCodeProcess( pi.hProcess, &code ) || code)
        FAIL( "the child ended with %lu, %lu ms after the release", code, GetTickCount() - t0 );
    if (real) CloseHandle( self );
    CloseHandle( in_write );
    CloseHandle( pi.hThread );
    CloseHandle( pi.hProcess );
    CloseHandle( sem );
}

/* 15: a wait-all waiter and a single waiter on one semaphore */

static HANDLE mix_sem, mix_event;

static DWORD WINAPI wait_all( void *arg )
{
    HANDLE objs[2] = { mix_sem, mix_event };
    return WaitForMultipleObjects( 2, objs, TRUE, 5000 ) != WAIT_OBJECT_0;
}

static void test_wait_all(void)
{
    HANDLE all, single;
    int i;

    start( "15 wait-all against a single waiter" );
    mix_sem = private_semaphore( 10 );
    if (!(mix_event = CreateEventW( NULL, FALSE, FALSE, NULL ))) FAIL( "CreateEvent: error %lu", GetLastError() );
    for (i = 0; i < ROUNDS; i++)
    {
        all = thread( wait_all, NULL );
        single = thread( wait_one, mix_sem );
        Sleep( 2 );  /* both asleep */
        /* the single waiter needs this count; the wait-all waiter can't take it without the event */
        release( mix_sem, 1 );
        if (WaitForSingleObject( single, 3000 ) != WAIT_OBJECT_0) FAIL( "round %d: the single waiter missed the release", i );
        join( single, 0 );
        SetEvent( mix_event );
        release( mix_sem, 1 );
        join( all, 5000 );
    }
    if (WaitForSingleObject( mix_sem, 0 ) != WAIT_TIMEOUT) FAIL( "a count was left" );
    CloseHandle( mix_event );
    CloseHandle( mix_sem );
}

/* 15: waiters with 1 ms timeouts */

static HANDLE short_sem;
static volatile LONG short_taken, short_done;

static DWORD WINAPI short_waiter( void *arg )
{
    while (!short_done)
        if (WaitForSingleObject( short_sem, 1 ) == WAIT_OBJECT_0) InterlockedIncrement( &short_taken );
    return 0;
}

static void test_short_timeouts(void)
{
    HANDLE waiters[4];
    int i;

    start( "15 1 ms timeouts" );
    short_sem = private_semaphore( JOBS );
    for (i = 0; i < ARRAYSIZE(waiters); i++) waiters[i] = thread( short_waiter, NULL );
    for (i = 0; i < JOBS; i++)
    {
        release( short_sem, 1 );
        if (!(i % 50)) Sleep( 1 );
    }
    await( &short_taken, JOBS, 30000 );
    short_done = 1;
    for (i = 0; i < ARRAYSIZE(waiters); i++) join( waiters[i], 10000 );
    if (short_taken != JOBS) FAIL( "%ld taken of %d", short_taken, JOBS );
    if (WaitForSingleObject( short_sem, 0 ) != WAIT_TIMEOUT) FAIL( "a count was left" );
    CloseHandle( short_sem );
}

/* 16: releases with no waiter, and what the counters say about them */

static BOOL env_is( const char *name, const char *value )
{
    char buf[256];
    DWORD n = GetEnvironmentVariableA( name, buf, sizeof(buf) );
    return n && n < sizeof(buf) && !_stricmp( buf, value );
}

static BOOL env_has( const char *name, const char *word )
{
    char buf[256], *p;
    DWORD n = GetEnvironmentVariableA( name, buf, sizeof(buf) );
    if (!n || n >= sizeof(buf)) return FALSE;
    for (p = strtok( buf, "," ); p; p = strtok( NULL, "," )) if (!strcmp( p, word )) return TRUE;
    return FALSE;
}

/* this thread's counter <name> in heat.c's log, -1 if it isn't there */
static long long counter( const char *line, const char *name )
{
    char key[64];
    const char *p;
    snprintf( key, sizeof(key), " %s ", name );
    return (p = strstr( line, key )) ? _atoi64( p + strlen( key ) ) : -1;
}

static DWORD WINAPI no_waiter( void *arg )
{
    HANDLE sem = private_semaphore( 0x7fffffff );
    int i;

    for (i = 0; i < NO_WAITER; i++) release( sem, 1 );
    CloseHandle( sem );
    return 0;
}

static void test_no_waiter(void)
{
    char dir[MAX_PATH], path[MAX_PATH * 2], want[32], line[4096];
    WIN32_FIND_DATAA fd;
    HANDLE releaser, find;
    BOOL found = FALSE, skip;
    long long releases = 0, skipped = 0, none = 0, one = 0;
    DWORD tid;
    FILE *f;

    start( "16 releases with no waiter" );
    releaser = thread( no_waiter, NULL );
    tid = GetThreadId( releaser );
    join( releaser, 60000 );

    /* counting: WINE_HEAT_COUNT=1, and WINE_HEAT_EXE names this program */
    if (!env_is( "WINE_HEAT_COUNT", "1" ) || !env_is( "WINE_HEAT_EXE", "wakes.exe" )) return;
    if (!GetEnvironmentVariableA( "WINE_HEAT_LOG", dir, sizeof(dir) )) FAIL( "WINE_HEAT_LOG isn't set" );
    skip = env_has( "WINE_HEAT_WAKE", "skip" );
    Sleep( 6000 );  /* the counters are written every 5 s */

    /* this process's file is the one with that thread's row; the child's of 14 is there too */
    snprintf( want, sizeof(want), "thread %04lx ", tid );
    snprintf( path, sizeof(path), "Z:%s/heat-*.log", dir );
    if ((find = FindFirstFileA( path, &fd )) == INVALID_HANDLE_VALUE) FAIL( "no heat-*.log in %s", dir );
    do
    {
        snprintf( path, sizeof(path), "Z:%s/%s", dir, fd.cFileName );
        if (!(f = fopen( path, "r" ))) continue;
        while (!found && fgets( line, sizeof(line), f ))
        {
            if (strncmp( line, want, strlen( want ) )) continue;
            found = TRUE;
            releases = counter( line, "release" );
            skipped = counter( line, "wake_skipped" );
            none = counter( line, "wake_none" );
            one = counter( line, "wake_one" );
        }
        fclose( f );
    } while (!found && FindNextFileA( find, &fd ));
    FindClose( find );
    if (!found) FAIL( "no row for the releasing thread in %s", dir );

    printf( "gw2 test wakes: the releasing thread: release %lld wake_skipped %lld wake_none %lld wake_one %lld\n",
            releases, skipped, none, one );
    if (releases < NO_WAITER) FAIL( "%lld releases counted, fewer than %d", releases, NO_WAITER );
    if (skip && (skipped != releases || none)) FAIL( "with skip, wake_skipped must equal the releases and wake_none be 0" );
    if (!skip && (skipped || none + one != releases)) FAIL( "without skip, every release makes a wake call" );
}

int main( int argc, char **argv )
{
    if (argc == 2 && !strcmp( argv[1], "child" )) return child();
    setvbuf( stdout, NULL, _IONBF, 0 );

    test_jobs();
    test_producer();
    test_duplicates();
    test_child( TRUE );
    test_child( FALSE );
    test_wait_all();
    test_short_timeouts();
    test_no_waiter();

    printf( "gw2 test wakes: passed\n" );
    return 0;
}
