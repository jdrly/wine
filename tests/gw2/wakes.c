/* gw2 (jdrly/gw2-on-mac#40): the semaphore wake policy's tests, 0.1.x's cases
 * (gw2-on-mac's docs/research/heat-locks-2026-10-29.md, heat-r7-2026-11-04.md). They pass in
 * every mode of WINE_HEAT_WAKE (off, one, skip, one,skip), and under the engine's own ntdll.so.
 *
 *   13  20,000 single jobs to 7 workers that sleep between them, all taken. A producer that
 *       outpaces its consumer for 20 seconds (the semaphore reaches its maximum), every release
 *       taken, no stall. matrix.zsh runs this program three times in each mode, so each mode
 *       gets the minute the issue asks for, in three parts.
 *   14  A waiter on a duplicated handle and a release through it. A named semaphore opened
 *       twice. A semaphore duplicated into a child process, released by the parent after
 *       300 ms: the child wakes (with a real handle to this process and with the pseudo-handle,
 *       and duplicated by the child itself out of this process).
 *   15  A wait-all on a semaphore and an event against a single waiter on the same semaphore,
 *       200 rounds. 20,000 releases to waiters with 1 ms timeouts, each taken once.
 *
 * Where a case needs a waiter asleep when the release comes, the waiter says it is about to wait,
 * the release comes well after that, and the waiter's own clock shows it spent that time in the
 * wait. A case retries when it didn't, and fails when it never did.
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
#define PRODUCER_SECONDS 20
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

/* a millisecond's pause every 100 jobs keeps it behind the producer */
static DWORD WINAPI slow_consumer( void *arg )
{
    LONG n;
    for (;;)
    {
        if (WaitForSingleObject( fast_sem, 5000 ) != WAIT_OBJECT_0) return 1;
        if (fast_done) return 0;
        n = InterlockedIncrement( &fast_taken );
        if (!(n % 100)) Sleep( 1 );
    }
}

static void test_producer(void)
{
    LONG released = 0, full = 0;
    DWORD t0;
    HANDLE consumer;

    start( "13 producer" );
    fast_sem = private_semaphore( 1000 );
    consumer = thread( slow_consumer, NULL );
    for (t0 = GetTickCount(); GetTickCount() - t0 < PRODUCER_SECONDS * 1000;)
    {
        if (ReleaseSemaphore( fast_sem, 1, NULL )) released++;
        else if (GetLastError() == ERROR_TOO_MANY_POSTS)
        {
            full++;  /* 1,000 jobs waiting: the producer is ahead */
            SwitchToThread();
        }
        else FAIL( "ReleaseSemaphore: error %lu", GetLastError() );
        if (WaitForSingleObject( consumer, 0 ) == WAIT_OBJECT_0) FAIL( "the consumer stopped after %ld", fast_taken );
    }
    if (!full) FAIL( "the producer never got ahead of the consumer" );
    await( &fast_taken, released, 10000 );
    fast_done = 1;
    release( fast_sem, 1 );
    join( consumer, 10000 );
    if (fast_taken != released) FAIL( "%ld taken of %ld released", fast_taken, released );
    printf( "gw2 test wakes: %ld releases in %d s, the semaphore full %ld times\n", released, PRODUCER_SECONDS, full );
    CloseHandle( fast_sem );
}

/* 14: duplicated and named semaphores */

#define ASLEEP 2  /* a waiter's exit code: it took the count, but too soon to have slept */

struct waiter
{
    HANDLE sem;
    HANDLE ready;    /* set right before the wait */
    DWORD  min_ms;   /* the wait has to take this long to count as asleep */
};

static DWORD WINAPI wait_one( void *arg )
{
    struct waiter *w = arg;
    DWORD t0;

    SetEvent( w->ready );
    t0 = GetTickCount();
    if (WaitForSingleObject( w->sem, 5000 ) != WAIT_OBJECT_0) return 1;
    return GetTickCount() - t0 < w->min_ms ? ASLEEP : 0;
}

static HANDLE ready_event(void)
{
    HANDLE h = CreateEventW( NULL, FALSE, FALSE, NULL );
    if (!h) FAIL( "CreateEvent: error %lu", GetLastError() );
    return h;
}

/* a thread waits on <wait>, and once it has gone to sleep the release goes through <signal> */
static void wake_through( HANDLE wait, HANDLE signal )
{
    struct waiter w = { wait, ready_event(), 25 };
    HANDLE waiter;
    DWORD code;
    int tries;

    for (tries = 0; tries < 3; tries++)
    {
        waiter = thread( wait_one, &w );
        if (WaitForSingleObject( w.ready, 5000 ) != WAIT_OBJECT_0) FAIL( "the waiter didn't start" );
        Sleep( 50 );
        release( signal, 1 );
        if (WaitForSingleObject( waiter, 10000 ) != WAIT_OBJECT_0) FAIL( "the waiter didn't wake" );
        GetExitCodeThread( waiter, &code );
        CloseHandle( waiter );
        if (!code) break;
        if (code != ASLEEP) FAIL( "the waiter's wait failed" );
    }
    if (tries == 3) FAIL( "the waiter never was asleep when the release came" );
    CloseHandle( w.ready );
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

/* 14: into a child process. The child reads from its stdin how it gets the semaphore, sets the
 * parent's ready event right before it waits, and exits with 0 when it woke after sleeping. */

enum dup_how { DUP_PSEUDO, DUP_REAL, DUP_BY_CHILD };

struct child_msg
{
    DWORD  how;
    DWORD  parent;  /* the parent's process id */
    HANDLE sem;     /* in the child, or with DUP_BY_CHILD in the parent */
};

static HANDLE ready_name( DWORD parent, BOOL create )
{
    WCHAR name[64];
    swprintf( name, ARRAYSIZE(name), L"gw2-wakes-ready-%lu", parent );
    return create ? CreateEventW( NULL, FALSE, FALSE, name ) : OpenEventW( EVENT_MODIFY_STATE, FALSE, name );
}

static int child(void)
{
    struct child_msg msg;
    HANDLE parent, ready;
    DWORD got, t0;

    if (!ReadFile( GetStdHandle( STD_INPUT_HANDLE ), &msg, sizeof(msg), &got, NULL ) || got != sizeof(msg))
        return 3;
    if (msg.how == DUP_BY_CHILD &&
        (!(parent = OpenProcess( PROCESS_DUP_HANDLE, FALSE, msg.parent )) ||
         !DuplicateHandle( parent, msg.sem, GetCurrentProcess(), &msg.sem, 0, FALSE, DUPLICATE_SAME_ACCESS )))
        return 3;
    if (!(ready = ready_name( msg.parent, FALSE ))) return 3;
    SetEvent( ready );
    t0 = GetTickCount();
    if (WaitForSingleObject( msg.sem, 1500 ) != WAIT_OBJECT_0) return 1;
    return GetTickCount() - t0 < 100 ? ASLEEP : 0;
}

static const char * const dup_names[] =
{
    "14 duplicated into a child, pseudo-handle",
    "14 duplicated into a child, real handle",
    "14 duplicated by the child out of this process",
};

static void test_child( enum dup_how how )
{
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
    WCHAR cmd[MAX_PATH + 16];
    struct child_msg msg = { how, GetCurrentProcessId() };
    HANDLE sem, in_read, in_write, self = GetCurrentProcess(), ready;
    DWORD written, code = 0, t0;
    int tries;

    start( dup_names[how] );
    sem = private_semaphore( 10 );
    if (!(ready = ready_name( msg.parent, TRUE ))) FAIL( "CreateEvent: error %lu", GetLastError() );
    if (how == DUP_REAL && !(self = OpenProcess( PROCESS_DUP_HANDLE, FALSE, GetCurrentProcessId() )))
        FAIL( "OpenProcess: error %lu", GetLastError() );
    cmd[0] = '"';
    GetModuleFileNameW( NULL, cmd + 1, MAX_PATH );
    wcscat( cmd, L"\" child" );

    for (tries = 0; tries < 3; tries++)
    {
        if (!CreatePipe( &in_read, &in_write, &sa, 0 )) FAIL( "CreatePipe: error %lu", GetLastError() );
        SetHandleInformation( in_write, HANDLE_FLAG_INHERIT, 0 );
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = in_read;
        si.hStdOutput = GetStdHandle( STD_OUTPUT_HANDLE );
        si.hStdError = GetStdHandle( STD_ERROR_HANDLE );
        if (!CreateProcessW( NULL, cmd, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi ))
            FAIL( "CreateProcess: error %lu", GetLastError() );
        CloseHandle( in_read );

        msg.sem = sem;
        if (how != DUP_BY_CHILD &&
            !DuplicateHandle( self, sem, pi.hProcess, &msg.sem, 0, FALSE, DUPLICATE_SAME_ACCESS ))
            FAIL( "DuplicateHandle: error %lu", GetLastError() );
        if (!WriteFile( in_write, &msg, sizeof(msg), &written, NULL )) FAIL( "WriteFile: error %lu", GetLastError() );
        if (WaitForSingleObject( ready, 10000 ) != WAIT_OBJECT_0) FAIL( "the child didn't get to its wait" );

        Sleep( 300 );
        t0 = GetTickCount();
        release( sem, 1 );
        if (WaitForSingleObject( pi.hProcess, 5000 ) != WAIT_OBJECT_0) FAIL( "the child didn't end" );
        GetExitCodeProcess( pi.hProcess, &code );
        CloseHandle( in_write );
        CloseHandle( pi.hThread );
        CloseHandle( pi.hProcess );
        if (!code) break;
        if (code != ASLEEP) FAIL( "the child ended with %lu, %lu ms after the release", code, GetTickCount() - t0 );
    }
    if (tries == 3) FAIL( "the child never was asleep when the release came" );
    if (how == DUP_REAL) CloseHandle( self );
    CloseHandle( ready );
    CloseHandle( sem );
}

/* 15: a wait-all waiter and a single waiter on one semaphore */

static HANDLE mix_sem, mix_event, all_ready;

static DWORD WINAPI wait_all( void *arg )
{
    HANDLE objs[2] = { mix_sem, mix_event };
    SetEvent( all_ready );
    return WaitForMultipleObjects( 2, objs, TRUE, 5000 ) != WAIT_OBJECT_0;
}

static void test_wait_all(void)
{
    struct waiter w;
    HANDLE all, single;
    DWORD code;
    int i, asleep = 0;

    start( "15 wait-all against a single waiter" );
    mix_sem = private_semaphore( 10 );
    if (!(mix_event = CreateEventW( NULL, FALSE, FALSE, NULL ))) FAIL( "CreateEvent: error %lu", GetLastError() );
    all_ready = ready_event();
    w.sem = mix_sem;
    w.ready = ready_event();
    w.min_ms = 10;
    for (i = 0; i < ROUNDS; i++)
    {
        all = thread( wait_all, NULL );
        single = thread( wait_one, &w );
        if (WaitForSingleObject( all_ready, 5000 ) || WaitForSingleObject( w.ready, 5000 )) FAIL( "round %d: a waiter didn't start", i );
        Sleep( 20 );  /* both asleep */
        /* the single waiter needs this count; the wait-all waiter can't take it without the event */
        release( mix_sem, 1 );
        if (WaitForSingleObject( single, 3000 ) != WAIT_OBJECT_0) FAIL( "round %d: the single waiter missed the release", i );
        GetExitCodeThread( single, &code );
        CloseHandle( single );
        if (code == ASLEEP) code = 0;
        else asleep++;
        if (code) FAIL( "round %d: the single waiter's wait failed", i );
        SetEvent( mix_event );
        release( mix_sem, 1 );
        join( all, 5000 );
    }
    if (asleep < ROUNDS / 2) FAIL( "the single waiter was asleep at the release in only %d of %d rounds", asleep, ROUNDS );
    if (WaitForSingleObject( mix_sem, 0 ) != WAIT_TIMEOUT) FAIL( "a count was left" );
    printf( "gw2 test wakes: the single waiter was asleep in %d of %d rounds\n", asleep, ROUNDS );
    CloseHandle( w.ready );
    CloseHandle( all_ready );
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
    test_child( DUP_PSEUDO );
    test_child( DUP_REAL );
    test_child( DUP_BY_CHILD );
    test_wait_all();
    test_short_timeouts();
    test_no_waiter();

    printf( "gw2 test wakes: passed\n" );
    return 0;
}
