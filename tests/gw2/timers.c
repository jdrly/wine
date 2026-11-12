/* gw2 (jdrly/gw2-on-mac#99): heat.c's timed sleeps and wakes, the event counters, the waits by
 * timeout, the request log's three threads and the threads' names.
 *
 * The main thread first releases a semaphore of its own RELEASES times, so that it is the game
 * thread (the most releases). With counting on, it waits for the dumper's first block, then runs
 * the cases, each on threads of its own, so that each thread's row holds that case's counts only:
 *
 * 1. requests: two threads ask the server for their own exit code 3,000 and 2,000 times while the
 *    main thread does it 1,000 times (get_thread_info). The request log has blocks for all three,
 *    each with at least its count of get_thread_info, and the counters lines of the main thread's
 *    blocks add up to its RELEASES releases.
 * 2. lock: a holder takes a critical section, waits until the sleeper's EnterCriticalSection has
 *    raised its lock count, yields 200 times so the sleeper goes to sleep, and leaves, 50 rounds.
 *    The sleeper's row: lock_sleep and lock_ms above 0, and its five lock_ buckets add up to
 *    lock_sleep. The holder's: at least one alert that made the wake call, and alert_ms above 0.
 * 3. events: a thread sets and resets an auto-reset event nobody waits on 1,000 times: event_set
 *    1,000, event_wake_none 1,000, event_wake 0, wake_none 0, wake_ms above 0. Then another sets an
 *    event 20 times, each time after its waiter said it is about to wait and 200 yields: event_set
 *    20, event_wake at least 1, event_wake and event_wake_none 20 together, wake_none 0.
 * 4. waits: a thread waits on a semaphore at 0 1,000 times with a zero timeout and 100 times with
 *    1 ms: wait_poll 1,000, wait_timed 100, wait_sleep 100, its five wait_ buckets 100 together,
 *    wait_ms above 0, and its row's name is "-".
 * 5. name: a thread calls SetThreadDescription( L"gw2 timers name" ): its row ends in
 *    "start <its start routine> name gw2_timers_name".
 *
 * Then it waits for two more blocks in heat-requests-*.log, so that the second dump began after
 * the cases ended, and reads both files. Without counting it runs the cases and checks only what
 * the program itself sees.
 *
 * Prints "gw2 test timers: passed" and exits 0, or names what failed and exits 1. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#define RELEASES     20000
#define REQ_GAME     1000
#define REQ_A        3000
#define REQ_B        2000
#define LOCK_ROUNDS  50
#define YIELDS       200
#define NO_WAITER    1000
#define WAKE_ROUNDS  20
#define POLLS        1000
#define TIMED        100
#define NAME         L"gw2 timers name"
#define NAME_IN_LOG  "gw2_timers_name"
#define LIMIT_MS     10000

#define FAIL(...) do { \
        printf( "gw2 test timers: failed, line %d: ", __LINE__ ); \
        printf( __VA_ARGS__ ); \
        printf( "\n" ); \
        exit( 1 ); \
    } while (0)

static BOOL env_is( const char *name, const char *value )
{
    char buf[256];
    DWORD n = GetEnvironmentVariableA( name, buf, sizeof(buf) );
    return n && n < sizeof(buf) && !_stricmp( buf, value );
}

static HANDLE thread( LPTHREAD_START_ROUTINE func, void *arg, DWORD *tid )
{
    HANDLE h = CreateThread( NULL, 0, func, arg, 0, tid );
    if (!h) FAIL( "CreateThread: error %lu", GetLastError() );
    return h;
}

static void join( HANDLE h, const char *what )
{
    DWORD code;
    if (WaitForSingleObject( h, 30000 ) != WAIT_OBJECT_0) FAIL( "%s didn't end in 30 s", what );
    if (!GetExitCodeThread( h, &code ) || code) FAIL( "%s ended with %lu", what, code );
    CloseHandle( h );
}

/* yields until *value is want; FALSE after LIMIT_MS (a guard: the other side is stuck) */
static BOOL until( volatile LONG *value, LONG want )
{
    DWORD start = GetTickCount();
    while (*value != want)
    {
        SwitchToThread();
        if (GetTickCount() - start > LIMIT_MS) return FALSE;
    }
    return TRUE;
}

static void yields(void)
{
    int i;
    for (i = 0; i < YIELDS; i++) SwitchToThread();
}

/* 1: requests */

static BOOL ask( int n )
{
    DWORD code;
    while (n--) if (!GetExitCodeThread( GetCurrentThread(), &code ) || code != STILL_ACTIVE) return FALSE;
    return TRUE;
}

static DWORD WINAPI requester( void *arg )
{
    return !ask( (int)(INT_PTR)arg );
}

static DWORD req_a, req_b;

static void test_requests(void)
{
    HANDLE a = thread( requester, (void *)(INT_PTR)REQ_A, &req_a ), b = thread( requester, (void *)(INT_PTR)REQ_B, &req_b );

    if (!ask( REQ_GAME )) FAIL( "GetExitCodeThread: error %lu", GetLastError() );
    join( a, "requester A" );
    join( b, "requester B" );
}

/* 2: lock */

static CRITICAL_SECTION cs;
static volatile LONG lock_go, lock_done;
static DWORD lock_holder, lock_sleeper;

static DWORD WINAPI holder( void *arg )
{
    DWORD start;
    LONG r;

    for (r = 1; r <= LOCK_ROUNDS; r++)
    {
        EnterCriticalSection( &cs );
        InterlockedExchange( &lock_go, r );
        /* the sleeper's EnterCriticalSection raised the count: it waits for this thread */
        for (start = GetTickCount(); *(volatile LONG *)&cs.LockCount < 1;)
        {
            SwitchToThread();
            if (GetTickCount() - start > LIMIT_MS) return 2;
        }
        yields();
        LeaveCriticalSection( &cs );
        if (!until( &lock_done, r )) return 3;
    }
    return 0;
}

static DWORD WINAPI sleeper( void *arg )
{
    LONG r;

    for (r = 1; r <= LOCK_ROUNDS; r++)
    {
        if (!until( &lock_go, r )) return 2;
        EnterCriticalSection( &cs );
        LeaveCriticalSection( &cs );
        InterlockedExchange( &lock_done, r );
    }
    return 0;
}

static void test_lock(void)
{
    HANDLE h, s;

    InitializeCriticalSection( &cs );
    h = thread( holder, NULL, &lock_holder );
    s = thread( sleeper, NULL, &lock_sleeper );
    join( h, "the holder" );
    join( s, "the sleeper" );
    DeleteCriticalSection( &cs );
}

/* 3: events */

static HANDLE wake_event;
static volatile LONG wake_ready, wake_done;
static DWORD event_alone, event_setter;

static DWORD WINAPI set_alone( void *arg )
{
    HANDLE ev = CreateEventW( NULL, FALSE, FALSE, NULL );
    int i;

    if (!ev) return 2;
    for (i = 0; i < NO_WAITER; i++) if (!SetEvent( ev ) || !ResetEvent( ev )) return 3;
    CloseHandle( ev );
    return 0;
}

static DWORD WINAPI event_waiter( void *arg )
{
    LONG r;

    for (r = 1; r <= WAKE_ROUNDS; r++)
    {
        InterlockedExchange( &wake_ready, r );
        if (WaitForSingleObject( wake_event, LIMIT_MS ) != WAIT_OBJECT_0) return 2;
        InterlockedExchange( &wake_done, r );
    }
    return 0;
}

static DWORD WINAPI set_waited( void *arg )
{
    LONG r;

    for (r = 1; r <= WAKE_ROUNDS; r++)
    {
        if (!until( &wake_ready, r )) return 2;
        yields();  /* the waiter goes to sleep */
        if (!SetEvent( wake_event )) return 3;
        if (!until( &wake_done, r )) return 4;
    }
    return 0;
}

static void test_events(void)
{
    HANDLE a, w, s;

    a = thread( set_alone, NULL, &event_alone );
    join( a, "the setter without a waiter" );
    if (!(wake_event = CreateEventW( NULL, FALSE, FALSE, NULL ))) FAIL( "CreateEvent: error %lu", GetLastError() );
    w = thread( event_waiter, NULL, NULL );
    s = thread( set_waited, NULL, &event_setter );
    join( w, "the event's waiter" );
    join( s, "the setter with a waiter" );
    CloseHandle( wake_event );
}

/* 4: waits */

static DWORD waits_tid;

static DWORD WINAPI waiter( void *arg )
{
    HANDLE sem = CreateSemaphoreW( NULL, 0, 1, NULL );
    int i;

    if (!sem) return 2;
    for (i = 0; i < POLLS; i++) if (WaitForSingleObject( sem, 0 ) != WAIT_TIMEOUT) return 3;
    for (i = 0; i < TIMED; i++) if (WaitForSingleObject( sem, 1 ) != WAIT_TIMEOUT) return 4;
    CloseHandle( sem );
    return 0;
}

static void test_waits(void)
{
    join( thread( waiter, NULL, &waits_tid ), "the waiter" );
}

/* 5: name */

static DWORD name_tid;

static DWORD WINAPI named( void *arg )
{
    HRESULT (WINAPI *set)( HANDLE, const WCHAR * ) =
        (void *)GetProcAddress( GetModuleHandleW( L"kernel32.dll" ), "SetThreadDescription" );

    if (!set) return 2;
    return FAILED( set( GetCurrentThread(), NAME ) ) ? 3 : 0;
}

static void test_name(void)
{
    join( thread( named, NULL, &name_tid ), "the named thread" );
}

/* the logs */

static char heat_text[1 << 20];

/* $WINE_HEAT_LOG/<pattern>'s first match into <path>; with <skip>, matches starting with it don't count */
static BOOL find_log( const char *pattern, const char *skip, char *path, size_t size )
{
    char dir[MAX_PATH];
    WIN32_FIND_DATAA fd;
    HANDLE find;
    BOOL found = FALSE;

    if (!GetEnvironmentVariableA( "WINE_HEAT_LOG", dir, sizeof(dir) )) FAIL( "WINE_HEAT_LOG isn't set" );
    snprintf( path, size, "Z:%s/%s", dir, pattern );
    if ((find = FindFirstFileA( path, &fd )) == INVALID_HANDLE_VALUE) return FALSE;
    do
    {
        if (skip && !strncmp( fd.cFileName, skip, strlen( skip ) )) continue;
        snprintf( path, size, "Z:%s/%s", dir, fd.cFileName );
        found = TRUE;
    } while (!found && FindNextFileA( find, &fd ));
    FindClose( find );
    return found;
}

static DWORD log_size( const char *path )
{
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (!GetFileAttributesExA( path, GetFileExInfoStandard, &data )) FAIL( "can't read %s: error %lu", path, GetLastError() );
    return data.nFileSizeLow;
}

/* two more blocks after now: the second dump began after the first was written, after the cases */
static void await_dumps( const char *path )
{
    DWORD size = log_size( path ), start = GetTickCount();
    int n;

    for (n = 0; n < 2; n++)
    {
        DWORD before = size;
        while ((size = log_size( path )) == before)
        {
            if (GetTickCount() - start > 20000) FAIL( "%d new blocks in %s after 20 s, not 2", n, path );
            Sleep( 20 );
        }
    }
}

/* the counters file, whole: heat.c truncates and rewrites it every 5 s */
static void load_heat(void)
{
    char path[MAX_PATH * 2];
    DWORD start = GetTickCount();
    size_t len;
    FILE *f;

    for (;;)
    {
        if (!find_log( "heat-*.log", "heat-requests-", path, sizeof(path) )) FAIL( "no heat-<pid>.log" );
        len = 0;
        if ((f = fopen( path, "r" )))
        {
            len = fread( heat_text, 1, sizeof(heat_text) - 1, f );
            fclose( f );
        }
        heat_text[len] = 0;
        if (len && heat_text[len - 1] == '\n' && strstr( heat_text, "\ntotal " )) return;
        if (GetTickCount() - start > 10000) FAIL( "no complete %s in 10 s", path );
        Sleep( 20 );
    }
}

/* thread <tid>'s line in the counters file, up to its newline */
static const char *row( DWORD tid, size_t *len )
{
    char want[32];
    const char *line;

    snprintf( want, sizeof(want), "\nthread %04lx ", tid );
    if (!(line = strstr( heat_text, want ))) FAIL( "no row for thread %04lx", tid );
    line++;
    *len = strcspn( line, "\n" );
    return line;
}

/* the text after " <name> " in <line> of <len>, or NULL */
static const char *value( const char *line, size_t len, const char *name )
{
    char key[64];
    size_t n = snprintf( key, sizeof(key), " %s ", name );
    const char *p = strstr( line, key );

    return p && p < line + len ? p + n : NULL;
}

static long long counter( DWORD tid, const char *name )
{
    size_t len;
    const char *line = row( tid, &len ), *v = value( line, len, name );
    if (!v) FAIL( "no %s in the row of thread %04lx", name, tid );
    return _atoi64( v );
}

static double timer( DWORD tid, const char *name )
{
    size_t len;
    const char *line = row( tid, &len ), *v = value( line, len, name );
    if (!v) FAIL( "no %s in the row of thread %04lx", name, tid );
    return atof( v );
}

/* <prefix>_5us ... <prefix>_long of thread <tid>, added up */
static long long buckets( DWORD tid, const char *prefix )
{
    static const char * const lengths[] = { "5us", "20us", "100us", "1ms", "long" };
    char name[32];
    long long sum = 0;
    unsigned int i;

    for (i = 0; i < ARRAYSIZE(lengths); i++)
    {
        snprintf( name, sizeof(name), "%s_%s", prefix, lengths[i] );
        sum += counter( tid, name );
    }
    return sum;
}

/* the start and name at the end of thread <tid>'s row */
static void start_and_name( DWORD tid, ULONG_PTR *start, char *name, size_t size )
{
    size_t len;
    const char *line = row( tid, &len ), *s = value( line, len, "start" ), *n = value( line, len, "name" );

    if (!s || !n) FAIL( "no start or name in the row of thread %04lx", tid );
    if (n + strcspn( n, " \n" ) != line + len) FAIL( "the row of thread %04lx doesn't end in its name", tid );
    *start = (ULONG_PTR)_strtoui64( s, NULL, 16 );
    snprintf( name, size, "%.*s", (int)strcspn( n, " \n" ), n );
}

static void check_heat(void)
{
    long long sleeps, alerts, woke, none, wake;
    char name[64];
    ULONG_PTR start;

    load_heat();

    /* 2 */
    sleeps = counter( lock_sleeper, "lock_sleep" );
    alerts = counter( lock_holder, "alert" );
    woke = counter( lock_holder, "alert_wake" );
    printf( "gw2 test timers: lock: the sleeper's lock_sleep %lld lock_ms %.3f buckets %lld; the holder's alert %lld "
            "alert_wake %lld alert_ms %.3f\n", sleeps, timer( lock_sleeper, "lock_ms" ), buckets( lock_sleeper, "lock" ),
            alerts, woke, timer( lock_holder, "alert_ms" ) );
    if (sleeps <= 0) FAIL( "the sleeper never slept on the lock" );
    if (timer( lock_sleeper, "lock_ms" ) <= 0) FAIL( "no lock_ms" );
    if (buckets( lock_sleeper, "lock" ) != sleeps) FAIL( "the lock buckets add up to %lld, not %lld", buckets( lock_sleeper, "lock" ), sleeps );
    if (woke < 1 || alerts < woke) FAIL( "the holder: alert %lld, alert_wake %lld", alerts, woke );
    if (timer( lock_holder, "alert_ms" ) <= 0) FAIL( "no alert_ms" );

    /* 3 */
    printf( "gw2 test timers: events: alone event_set %lld event_wake %lld event_wake_none %lld wake_none %lld wake_ms %.3f; "
            "with a waiter event_set %lld event_wake %lld event_wake_none %lld wake_none %lld\n",
            counter( event_alone, "event_set" ), counter( event_alone, "event_wake" ), counter( event_alone, "event_wake_none" ),
            counter( event_alone, "wake_none" ), timer( event_alone, "wake_ms" ), counter( event_setter, "event_set" ),
            counter( event_setter, "event_wake" ), counter( event_setter, "event_wake_none" ), counter( event_setter, "wake_none" ) );
    if (counter( event_alone, "event_set" ) != NO_WAITER || counter( event_alone, "event_wake_none" ) != NO_WAITER ||
        counter( event_alone, "event_wake" ) || counter( event_alone, "wake_none" ))
        FAIL( "the event without a waiter" );
    if (timer( event_alone, "wake_ms" ) <= 0) FAIL( "no wake_ms" );
    wake = counter( event_setter, "event_wake" );
    none = counter( event_setter, "event_wake_none" );
    if (counter( event_setter, "event_set" ) != WAKE_ROUNDS || wake < 1 || wake + none != WAKE_ROUNDS ||
        counter( event_setter, "wake_none" ))
        FAIL( "the event with a waiter" );

    /* 4 */
    sleeps = counter( waits_tid, "wait_sleep" );
    printf( "gw2 test timers: waits: wait %lld wait_poll %lld wait_timed %lld wait_sleep %lld buckets %lld wait_ms %.3f\n",
            counter( waits_tid, "wait" ), counter( waits_tid, "wait_poll" ), counter( waits_tid, "wait_timed" ), sleeps,
            buckets( waits_tid, "wait" ), timer( waits_tid, "wait_ms" ) );
    if (counter( waits_tid, "wait_poll" ) != POLLS || counter( waits_tid, "wait_timed" ) != TIMED || sleeps != TIMED)
        FAIL( "the waits" );
    if (counter( waits_tid, "wait" ) < POLLS + TIMED) FAIL( "%lld waits", counter( waits_tid, "wait" ) );
    if (buckets( waits_tid, "wait" ) != sleeps) FAIL( "the wait buckets add up to %lld, not %lld", buckets( waits_tid, "wait" ), sleeps );
    if (timer( waits_tid, "wait_ms" ) <= 0) FAIL( "no wait_ms" );
    start_and_name( waits_tid, &start, name, sizeof(name) );
    if (strcmp( name, "-" )) FAIL( "the waiter's name is %s, not -", name );

    /* 5 */
    start_and_name( name_tid, &start, name, sizeof(name) );
    printf( "gw2 test timers: name: start %#llx (named() is at %p) name %s\n", (unsigned long long)start, named, name );
    if (strcmp( name, NAME_IN_LOG )) FAIL( "the named thread's name is %s, not " NAME_IN_LOG, name );
    if (start != (ULONG_PTR)named) FAIL( "the named thread's start is %#llx, not %p", (unsigned long long)start, named );
}

/* the number after " <name> " in a counters line, or 0 */
static long long field( const char *line, const char *name )
{
    char key[64];
    const char *p;
    snprintf( key, sizeof(key), " %s ", name );
    return (p = strstr( line, key )) ? _atoi64( p + strlen( key ) ) : 0;
}

static void check_requests( const char *path )
{
    static char line[16384];
    DWORD tids[3] = { GetCurrentThreadId(), req_a, req_b };
    const long long want[3] = { REQ_GAME, REQ_A, REQ_B };
    long long blocks[3] = {0}, asked[3] = {0}, released = 0, server_a = 0;
    char head[64];
    int who = -1, i;
    FILE *f;

    if (!(f = fopen( path, "r" ))) FAIL( "can't open %s", path );
    while (fgets( line, sizeof(line), f ))
    {
        if (!strncmp( line, "requests thread ", 16 ))
        {
            for (who = -1, i = 0; i < 3; i++)
            {
                snprintf( head, sizeof(head), "requests thread %04lx ", tids[i] );
                if (!strncmp( line, head, strlen( head ) )) who = i;
            }
            if (who >= 0) blocks[who]++;
            continue;
        }
        if (who < 0) continue;
        if (!strncmp( line, "req get_thread_info ", 20 )) asked[who] += _atoi64( line + 20 );
        else if (!strncmp( line, "counters", 8 ))
        {
            if (who == 0) released += field( line, "release" );
            if (who == 1) server_a += field( line, "server" );
        }
    }
    fclose( f );

    printf( "gw2 test timers: requests: blocks %lld %lld %lld, get_thread_info %lld %lld %lld; the game thread's counters "
            "lines: release %lld; A's: server %lld\n", blocks[0], blocks[1], blocks[2], asked[0], asked[1], asked[2],
            released, server_a );
    for (i = 0; i < 3; i++)
    {
        if (!blocks[i]) FAIL( "no block of thread %04lx", tids[i] );
        if (asked[i] < want[i]) FAIL( "thread %04lx: %lld get_thread_info, not %lld or more", tids[i], asked[i], want[i] );
    }
    if (released != RELEASES) FAIL( "the game thread's counters lines add up to %lld releases, not %d", released, RELEASES );
    if (server_a < REQ_A) FAIL( "A's counters lines add up to %lld server calls, not %d or more", server_a, REQ_A );
}

int main(void)
{
    BOOL counting = env_is( "WINE_HEAT_COUNT", "1" ) && env_is( "WINE_HEAT_EXE", "timers.exe" );
    char path[MAX_PATH * 2];
    DWORD start;
    HANDLE sem;
    int i;

    setvbuf( stdout, NULL, _IONBF, 0 );
    if (!(sem = CreateSemaphoreW( NULL, 0, 0x7fffffff, NULL ))) FAIL( "CreateSemaphore: error %lu", GetLastError() );
    for (i = 0; i < RELEASES; i++)
        if (!ReleaseSemaphore( sem, 1, NULL )) FAIL( "ReleaseSemaphore: error %lu", GetLastError() );
    CloseHandle( sem );

    if (counting)
    {
        for (start = GetTickCount(); !find_log( "heat-requests-*.log", NULL, path, sizeof(path) ); Sleep( 20 ))
            if (GetTickCount() - start > 15000) FAIL( "the dumper wrote no heat-requests-*.log in 15 s" );
    }

    test_requests();
    test_lock();
    test_events();
    test_waits();
    test_name();
    printf( "gw2 test timers: the cases ran\n" );

    if (counting)
    {
        await_dumps( path );
        check_requests( path );
        check_heat();
    }
    printf( "gw2 test timers: passed\n" );
    return 0;
}
