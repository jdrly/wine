/* gw2 (jdrly/gw2-on-mac#45): the spins of WINE_HEAT_SPIN, with each kind off and on. Every case
 * passes under the engine's own ntdll.so and under mine in every mode; the counter checks run
 * when WINE_HEAT_COUNT=1 and WINE_HEAT_EXE names this program.
 *
 *   cs      4 threads on one critical section (no spin count) for 200,000 entries: never two
 *           inside, every entry counted.
 *   srw     2 producers and 2 consumers on a queue of 8 behind an SRW lock with two condition
 *           variables, 100,000 items: every item produced is consumed, nobody stays asleep.
 *   jobs    a game thread at HIGHEST releases 2 jobs of 20 us (then 100 us) at a time to 2
 *           workers at ABOVE_NORMAL that wait on a semaphore, and waits on an event that the
 *           round's last job sets, 2,000 rounds each: every round ends, every job runs once.
 *   alerts  a thread at HIGHEST and one at ABOVE_NORMAL wait in NtWaitForAlertByThreadId, and a
 *           third alerts each in turn after a delay that sweeps 0 to twice the lock spin, so
 *           alerts land before, during and after the spin. Each waiter arms its wait only when
 *           the alerter turns to it; otherwise its spin would be over while the alerter served
 *           the other thread. An alert is never lost: one sent before the wait stays pending, so
 *           each wait has to return STATUS_ALERTED, no exception allowed.
 *   pulse   a thread waits on an auto-reset event that only PulseEvent touches, the pulse after
 *           a delay that sweeps through the event spin, and every 16th round after 3 ms, when the
 *           waiter is asleep.
 *
 * The counters, from the threads' rows in heat.c's log:
 *   - a kind that is off shows no hit and no miss in any row
 *   - lock on: the HIGHEST alerts thread has hits; the ABOVE_NORMAL one has hits when the lock's
 *     scope keeps priority 1, and otherwise only spin_scoped (no hit, no miss); the cs threads spun
 *   - event on: the game thread spun on its jobs' event, or is scoped out; the pulse waiter has hits
 *   - sem on: the workers spun on the semaphore, or are scoped out. With WINE_HEAT_WAKE=one,skip
 *     too (matrix.zsh), a release during a worker's spin makes no wake call, and the worker joins
 *     the sleeper count only after its spin: a wake lost there would leave a round's jobs undone,
 *     and the game thread's wait for them fails after 5 s
 * A "spun" check needs one spin in a whole case. A "hits" check needs one hit in thousands of
 * rounds, and a hit needs the other thread to run during a spin of 20 or 30 us. Each round begins
 * with both threads running, so on an otherwise idle machine that happens in most rounds. With
 * more busy threads than CPUs it is left to chance: four of these programs side by side on the
 * runner's 3 CPUs gave 0 lock hits in 556 spins. So matrix.zsh runs one at a time.
 *
 * A pulse releases only a thread that is waiting when it comes (PulseEvent), and the waiter can't
 * show when its wait began, so a pulse that comes before the wait is dropped by the rules, here as
 * on Windows. A round that times out is either that or a lost pulse, and the case can't tell
 * which: it allows them in 2% of its pulsed rounds and prints how many, for the sweep and the 3 ms
 * rounds apart. With every spin off, nothing of jdrly/gw2-on-mac#45 runs, and run 43140685855
 * still counted up to 6 such timeouts in 2,000 on the runner. A stricter rule for the rounds whose
 * pulse lands during the spin can't be drawn here: the program can't see when the waiter's spin
 * begins, and a waiter taken off its core between arming and its wait drops a pulse by the rules
 * in any band. The rounds run in lockstep: the waiter starts a round only when the pulser says so,
 * and the pulser sends no pulse in a round whose 50 ms wait already ended, which happens only when
 * the pulser was off its core that long. Such a round is "pulser late" and tests nothing; more than
 * 10% of them and the case fails, because it no longer tests enough pulses. A failure says which
 * it was: a wait that returned another status (with it), or a waiter that didn't come back (with
 * the last round it finished). It is stress coverage, not proof; the proof that a pulse during a spin is kept is the
 * argument in heatspin.c. The sweep runs from a quarter of the spin's time to past its end, so pulses
 * land in the spin, at its end and in the sleep after it. The waiter's hits under event spin show
 * pulses taken during a spin.
 *
 * Prints "gw2 test spin: passed" and exits 0, or names what failed and exits 1. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <winternl.h>

#ifndef STATUS_ALERTED
#define STATUS_ALERTED ((NTSTATUS)0x00000101)
#endif

static const char *current = "startup";

#define FAIL(...) do { \
        printf( "gw2 test spin: %s failed, line %d: ", current, __LINE__ ); \
        printf( __VA_ARGS__ ); \
        printf( "\n" ); \
        fflush( stdout ); \
        exit( 1 ); \
    } while (0)

static void start( const char *name )
{
    current = name;
    printf( "gw2 test spin: %s\n", name );
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

static void set_priority( int priority )
{
    if (!SetThreadPriority( GetCurrentThread(), priority ))
        FAIL( "SetThreadPriority %d: error %lu", priority, GetLastError() );
}

static LARGE_INTEGER freq;

static LONGLONG now(void)
{
    LARGE_INTEGER t;
    QueryPerformanceCounter( &t );
    return t.QuadPart;
}

static LONGLONG us_ticks( double us )
{
    return (LONGLONG)(us * freq.QuadPart / 1e6);
}

static void busy( LONGLONG ticks )
{
    LONGLONG until = now() + ticks;
    while (now() < until) YieldProcessor();
}

/* waits until *value is want, or fails after ms */
static void await( volatile LONG *value, LONG want, DWORD ms, const char *what, LONG round )
{
    DWORD t0 = GetTickCount();
    while (*value != want)
    {
        if (GetTickCount() - t0 > ms) FAIL( "round %ld: %s", round, what );
        YieldProcessor();
    }
}

/* lets a case's threads run until *count reaches want; 30 s without that is a hang */
static void work_until( volatile LONG *count, LONG want )
{
    DWORD t0 = GetTickCount();
    while (*count < want)
    {
        if (GetTickCount() - t0 > 30000) FAIL( "%ld of %ld after 30 s", *count, want );
        Sleep( 10 );
    }
}

/* WINE_HEAT_SPIN as heat.c reads it */

enum { LOCK, EVENT, SEM, KINDS };
static const char * const kind_names[KINDS] = { "lock", "event", "sem" };
static struct { BOOL on; long us; unsigned int not; } spin[KINDS];
static BOOL counting;

static BOOL env_is( const char *name, const char *value )
{
    char buf[256];
    DWORD n = GetEnvironmentVariableA( name, buf, sizeof(buf) );
    return n && n < sizeof(buf) && !_stricmp( buf, value );
}

static void read_spin(void)
{
    char buf[256], *item, *p, *end;
    unsigned int not;
    long us, prio;
    size_t n;
    BOOL ok;
    int k;

    counting = env_is( "WINE_HEAT_COUNT", "1" ) && env_is( "WINE_HEAT_EXE", "spin.exe" );
    n = GetEnvironmentVariableA( "WINE_HEAT_SPIN", buf, sizeof(buf) );
    if (!n || n >= sizeof(buf)) return;
    for (item = strtok( buf, "," ); item; item = strtok( NULL, "," ))
    {
        if (!(p = strchr( item, '=' ))) continue;
        *p++ = 0;
        for (k = 0; k < KINDS; k++) if (!strcmp( item, kind_names[k] )) break;
        if (k == KINDS) continue;
        us = strtol( p, &end, 10 );
        if (end == p || us < 1 || us > 1000) continue;
        not = 0;
        ok = TRUE;
        for (p = end; ok && *p; p += n)
        {
            if (*p++ != ':') { ok = FALSE; break; }
            n = strcspn( p, ":" );
            if (n == 3 && !strncmp( p, "all", 3 )) not = 0;
            else if (n == 4 && !strncmp( p, "game", 4 )) not |= 1u << 16;
            else if (n > 4 && !strncmp( p, "not=", 4 ) && (prio = strtol( p + 4, &end, 10 )) >= -15 &&
                     prio <= 15 && end == p + n)
                not |= 1u << (prio + 15);
            else ok = FALSE;
        }
        if (!ok) continue;
        spin[k].on = TRUE;
        spin[k].us = us;
        spin[k].not = not;
    }
    for (k = 0; k < KINDS; k++)
        printf( "gw2 test spin: %s %s\n", kind_names[k], spin[k].on ? "on" : "off" );
}

static BOOL spins( int kind, int priority )
{
    return spin[kind].on && !(spin[kind].not & (1u << (priority + 15)));
}

/* heat.c's log: one row per thread, read once after the cases */

static char log_text[1 << 20];

/* this process's log into log_text; the "seconds" of its header, -1 if there is none yet */
static double load_log( const char *dir )
{
    char path[MAX_PATH * 2];
    const char *p;
    WIN32_FIND_DATAA fd;
    HANDLE find;
    size_t len = 0;
    FILE *f;

    snprintf( path, sizeof(path), "Z:%s/heat-*.log", dir );
    if ((find = FindFirstFileA( path, &fd )) == INVALID_HANDLE_VALUE) return -1;
    do
    {
        snprintf( path, sizeof(path), "Z:%s/%s", dir, fd.cFileName );
        if (!(f = fopen( path, "r" ))) continue;
        len += fread( log_text + len, 1, sizeof(log_text) - 1 - len, f );
        fclose( f );
    } while (FindNextFileA( find, &fd ));
    FindClose( find );
    log_text[len] = 0;
    /* heat.c writes the file in one call after truncating it; a read in between is incomplete */
    if (!len || log_text[len - 1] != '\n') return -1;
    return (p = strstr( log_text, " seconds " )) && strstr( log_text, "\ntotal " ) ? atof( p + 9 ) : -1;
}

/* the first dump made after the cases ended: heat.c writes one every 5 s */
static void read_log(void)
{
    char dir[MAX_PATH];
    const char *line;
    double before, seconds;
    DWORD t0 = GetTickCount();

    if (!GetEnvironmentVariableA( "WINE_HEAT_LOG", dir, sizeof(dir) )) FAIL( "WINE_HEAT_LOG isn't set" );
    /* A complete dump read now was written before this read, so the next one starts after it,
     * which is after the cases. Without one (none yet, or a read during the write), the dump
     * that completes next may have begun before the cases ended: take the one after that. */
    if ((before = load_log( dir )) < 0)
    {
        while ((before = load_log( dir )) < 0)
        {
            if (GetTickCount() - t0 > 12000) FAIL( "no counters in %s after 12 s", dir );
            Sleep( 100 );
        }
    }
    while ((seconds = load_log( dir )) <= before)
    {
        if (GetTickCount() - t0 > 17000) FAIL( "no new counters in %s", dir );
        Sleep( 100 );
    }
    printf( "gw2 test spin: the counters of %.1f s, after the cases' end at %.1f s or later\n", seconds, before );
    if ((line = strstr( log_text, "\nspin " )))
        printf( "gw2 test spin: the log's line: %.*s\n", (int)strcspn( line + 1, "\n" ), line + 1 );
}

/* counter <name> in the row of thread <tid>; fails without that row */
static long long counter( DWORD tid, const char *name )
{
    char want[32], key[64];
    const char *line, *p, *eol;

    snprintf( want, sizeof(want), "\nthread %04lx ", tid );
    if (!(line = strstr( log_text, want ))) FAIL( "no row for thread %04lx", tid );
    eol = strchr( line + 1, '\n' );
    snprintf( key, sizeof(key), " %s ", name );
    if (!(p = strstr( line, key )) || (eol && p > eol)) FAIL( "no %s in the row of thread %04lx", name, tid );
    return _atoi64( p + strlen( key ) );
}

static long long total( const char *name )
{
    char key[64];
    const char *line = strstr( log_text, "\ntotal " ), *p;

    if (!line) FAIL( "no total row" );
    snprintf( key, sizeof(key), " %s ", name );
    if (!(p = strstr( line, key ))) FAIL( "no %s in the total row", name );
    return _atoi64( p + strlen( key ) );
}

static const char * const hit_names[KINDS] = { "lspin_hit", "espin_hit", "sspin_hit" };
static const char * const miss_names[KINDS] = { "lspin_miss", "espin_miss", "sspin_miss" };

/* the spins of one kind in a thread's row: hits and misses, and the waits it didn't spin */
static void show( const char *who, DWORD tid, int kind )
{
    printf( "gw2 test spin: %s: %s %lld %s %lld spin_off %lld spin_scoped %lld spin_ms %lld\n", who,
            hit_names[kind], counter( tid, hit_names[kind] ), miss_names[kind], counter( tid, miss_names[kind] ),
            counter( tid, "spin_off" ), counter( tid, "spin_scoped" ), counter( tid, "spin_ms" ) );
}

/* a kind's spins in a thread's row: it spun at all (hits, misses or pauses) */
static BOOL spun( DWORD tid, int kind )
{
    return counter( tid, hit_names[kind] ) + counter( tid, miss_names[kind] ) > 0;
}

/* cs: 4 threads on one critical section */

#define CS_THREADS 4
#define CS_ENTRIES 200000  /* thousands of collisions, each a lock wait with no spin count */
static CRITICAL_SECTION cs;
static volatile LONG cs_stop, cs_inside, cs_twice;
static LONG cs_count, cs_local[CS_THREADS];
static DWORD cs_tid[CS_THREADS];

static DWORD WINAPI cs_thread( void *arg )
{
    int n = (int)(INT_PTR)arg;
    volatile int k;
    LONG local = 0;

    while (!cs_stop)
    {
        EnterCriticalSection( &cs );
        if (InterlockedExchange( &cs_inside, 1 )) cs_twice = 1;
        cs_count++;
        for (k = 0; k < 50; k++) ;
        InterlockedExchange( &cs_inside, 0 );
        LeaveCriticalSection( &cs );
        local++;
        for (k = 0; k < 50; k++) ;
    }
    cs_local[n] = local;
    return 0;
}

static void test_cs(void)
{
    HANDLE threads[CS_THREADS];
    LONG sum = 0;
    int i;

    start( "cs" );
    if (!InitializeCriticalSectionEx( &cs, 0, CRITICAL_SECTION_NO_DEBUG_INFO )) FAIL( "InitializeCriticalSectionEx" );
    for (i = 0; i < CS_THREADS; i++)
    {
        threads[i] = thread( cs_thread, (void *)(INT_PTR)i );
        cs_tid[i] = GetThreadId( threads[i] );
    }
    work_until( (volatile LONG *)&cs_count, CS_ENTRIES );
    cs_stop = 1;
    for (i = 0; i < CS_THREADS; i++) join( threads[i], 10000 );
    for (i = 0; i < CS_THREADS; i++) sum += cs_local[i];
    if (cs_twice) FAIL( "two threads were inside at once" );
    if (sum != cs_count) FAIL( "%ld entries counted inside, %ld by the threads", cs_count, sum );
    printf( "gw2 test spin: %ld entries\n", sum );
    DeleteCriticalSection( &cs );
}

/* srw: a queue behind an SRW lock with condition variables */

#define QUEUE 8
#define QUEUE_ITEMS 100000
static SRWLOCK srw = SRWLOCK_INIT;
static CONDITION_VARIABLE not_empty = CONDITION_VARIABLE_INIT, not_full = CONDITION_VARIABLE_INIT;
static LONGLONG queue[QUEUE];
static int q_head, q_len;
static BOOL q_stop, q_producers_done;
static LONGLONG produced_sum, consumed_sum;
static LONG produced, consumed;

static DWORD WINAPI producer( void *arg )
{
    LONGLONG value = (LONGLONG)(INT_PTR)arg << 40;

    for (;;)
    {
        AcquireSRWLockExclusive( &srw );
        while (q_len == QUEUE && !q_stop)
            if (!SleepConditionVariableSRW( &not_full, &srw, 10000, 0 )) FAIL( "a producer waited 10 s" );
        if (q_stop)
        {
            ReleaseSRWLockExclusive( &srw );
            return 0;
        }
        queue[(q_head + q_len++) % QUEUE] = ++value;
        produced++;
        produced_sum += value;
        ReleaseSRWLockExclusive( &srw );
        WakeConditionVariable( &not_empty );
    }
}

static DWORD WINAPI consumer( void *arg )
{
    LONGLONG value;

    for (;;)
    {
        AcquireSRWLockExclusive( &srw );
        while (!q_len && !q_producers_done)
            if (!SleepConditionVariableSRW( &not_empty, &srw, 10000, 0 )) FAIL( "a consumer waited 10 s" );
        if (!q_len)
        {
            ReleaseSRWLockExclusive( &srw );
            return 0;
        }
        value = queue[q_head];
        q_head = (q_head + 1) % QUEUE;
        q_len--;
        consumed++;
        consumed_sum += value;
        ReleaseSRWLockExclusive( &srw );
        WakeConditionVariable( &not_full );
    }
}

static void test_srw(void)
{
    HANDLE p[2], c[2];
    int i;

    start( "srw" );
    for (i = 0; i < 2; i++)
    {
        p[i] = thread( producer, (void *)(INT_PTR)(i + 1) );
        c[i] = thread( consumer, NULL );
    }
    work_until( (volatile LONG *)&produced, QUEUE_ITEMS );
    AcquireSRWLockExclusive( &srw );
    q_stop = TRUE;
    ReleaseSRWLockExclusive( &srw );
    WakeAllConditionVariable( &not_full );
    for (i = 0; i < 2; i++) join( p[i], 10000 );
    AcquireSRWLockExclusive( &srw );
    q_producers_done = TRUE;
    ReleaseSRWLockExclusive( &srw );
    WakeAllConditionVariable( &not_empty );
    for (i = 0; i < 2; i++) join( c[i], 10000 );
    if (consumed != produced || consumed_sum != produced_sum)
        FAIL( "%ld items produced, %ld consumed", produced, consumed );
    printf( "gw2 test spin: %ld items through the queue\n", produced );
}

/* jobs: a game thread, its job semaphore and the event of its rounds */

#define WORKERS 2
#define JOB_ROUNDS 2000  /* each round sleeps or spins the game thread once and each worker once */
static HANDLE job_sem, job_done;
static volatile LONG jobs_left, jobs_stop;
static LONG jobs_ran;
static LONGLONG job_ticks;
static DWORD game_tid, worker_tid[WORKERS];

static DWORD WINAPI job_worker( void *arg )
{
    set_priority( THREAD_PRIORITY_ABOVE_NORMAL );
    for (;;)
    {
        if (WaitForSingleObject( job_sem, 10000 ) != WAIT_OBJECT_0) return 1;
        if (jobs_stop) return 0;
        busy( job_ticks );
        InterlockedIncrement( &jobs_ran );
        if (!InterlockedDecrement( &jobs_left )) SetEvent( job_done );
    }
}

static DWORD WINAPI game_thread( void *arg )
{
    LONG rounds = 0;
    LONGLONG t0;

    set_priority( THREAD_PRIORITY_HIGHEST );
    t0 = now();
    while (rounds < JOB_ROUNDS)
    {
        jobs_left = WORKERS;
        if (!ReleaseSemaphore( job_sem, WORKERS, NULL )) FAIL( "ReleaseSemaphore: error %lu", GetLastError() );
        if (WaitForSingleObject( job_done, 5000 ) != WAIT_OBJECT_0) FAIL( "round %ld: its jobs never said they were done", rounds );
        rounds++;
    }
    printf( "gw2 test spin: %ld rounds of %d jobs, %.1f us a round\n", rounds, WORKERS,
            (double)(now() - t0) * 1e6 / freq.QuadPart / rounds );
    if (jobs_ran != rounds * WORKERS) FAIL( "%ld jobs ran in %ld rounds", jobs_ran, rounds );
    return 0;
}

static void test_jobs( int us )
{
    HANDLE workers[WORKERS], game;
    char name[32];
    int i;

    snprintf( name, sizeof(name), "jobs of %d us", us );
    start( name );
    job_ticks = us_ticks( us );
    jobs_ran = 0;
    jobs_stop = 0;
    if (!(job_sem = CreateSemaphoreW( NULL, 0, 1000, NULL ))) FAIL( "CreateSemaphore: error %lu", GetLastError() );
    if (!(job_done = CreateEventW( NULL, FALSE, FALSE, NULL ))) FAIL( "CreateEvent: error %lu", GetLastError() );
    for (i = 0; i < WORKERS; i++)
    {
        workers[i] = thread( job_worker, NULL );
        worker_tid[i] = GetThreadId( workers[i] );
    }
    game = thread( game_thread, NULL );
    game_tid = GetThreadId( game );
    join( game, 30000 );
    jobs_stop = 1;
    ReleaseSemaphore( job_sem, WORKERS, NULL );
    for (i = 0; i < WORKERS; i++) join( workers[i], 10000 );
    CloseHandle( job_sem );
    CloseHandle( job_done );
}

/* alerts: NtWaitForAlertByThreadId against NtAlertThreadByThreadId */

static NTSTATUS (WINAPI *pNtWaitForAlertByThreadId)( const void *, const LARGE_INTEGER * );
static NTSTATUS (WINAPI *pNtAlertThreadByThreadId)( HANDLE );

struct alerted
{
    int           priority;
    DWORD         tid;
    HANDLE        go;  /* a semaphore the alerter releases when it turns to this thread */
    volatile LONG ready, armed, taken, stop;
    NTSTATUS      failed;
};

static DWORD WINAPI alert_waiter( void *arg )
{
    struct alerted *w = arg;
    LARGE_INTEGER timeout = { .QuadPart = -20000000 };  /* 2 s */
    NTSTATUS status;
    LONG i;

    set_priority( w->priority );
    w->tid = GetCurrentThreadId();
    InterlockedExchange( &w->ready, 1 );
    for (i = 1;; i++)
    {
        /* armed only when the alerter turns to this thread, so its delay starts at the wait;
         * asleep meanwhile, so that it takes no CPU from the alerter and the other waiter */
        if (WaitForSingleObject( w->go, 10000 ) != WAIT_OBJECT_0) return 1;
        if (w->stop) return 0;
        InterlockedExchange( &w->armed, i );
        status = pNtWaitForAlertByThreadId( (const void *)&w->armed, &timeout );
        if (status != STATUS_ALERTED)
        {
            w->failed = status;
            return 1;
        }
        InterlockedExchange( &w->taken, i );
    }
}

static HANDLE start_waiter( struct alerted *w )
{
    HANDLE h;
    DWORD t0 = GetTickCount();

    if (!(w->go = CreateSemaphoreW( NULL, 0, 1, NULL ))) FAIL( "CreateSemaphore: error %lu", GetLastError() );
    h = thread( alert_waiter, w );

    /* alone: no other thread starts or ends while it does, and it set its priority */
    while (!w->ready)
    {
        if (GetTickCount() - t0 > 10000) FAIL( "a waiter didn't start" );
        Sleep( 1 );
    }
    return h;
}

static struct alerted high = { THREAD_PRIORITY_HIGHEST }, above = { THREAD_PRIORITY_ABOVE_NORMAL };
#define ALERT_ROUNDS 2000

static DWORD WINAPI alerter( void *arg )
{
    struct alerted *w[2] = { &high, &above };
    LONGLONG span = us_ticks( 2 * (spin[LOCK].on ? spin[LOCK].us : 20) );
    LONG i;
    int j;

    for (i = 1; i <= ALERT_ROUNDS; i++)
    {
        for (j = 0; j < 2; j++)
        {
            ReleaseSemaphore( w[j]->go, 1, NULL );
            await( &w[j]->armed, i, 2000, "a waiter didn't get to its wait", i );
            busy( span * (i % 64) / 64 );
            pNtAlertThreadByThreadId( ULongToHandle( w[j]->tid ) );
            if (j == 1 && w[0]->failed) FAIL( "round %ld: the HIGHEST thread's wait returned %#lx", i, w[0]->failed );
            if (w[j]->failed) FAIL( "round %ld: a wait returned %#lx, a lost alert", i, w[j]->failed );
            await( &w[j]->taken, i, 3000, "the alert wasn't taken", i );
        }
    }
    return 0;
}

static void test_alerts(void)
{
    HMODULE ntdll = GetModuleHandleA( "ntdll.dll" );
    HANDLE h, a, s;

    start( "alerts" );
    pNtWaitForAlertByThreadId = (void *)GetProcAddress( ntdll, "NtWaitForAlertByThreadId" );
    pNtAlertThreadByThreadId = (void *)GetProcAddress( ntdll, "NtAlertThreadByThreadId" );
    if (!pNtWaitForAlertByThreadId || !pNtAlertThreadByThreadId) FAIL( "ntdll has no NtWaitForAlertByThreadId" );
    a = start_waiter( &above );
    h = start_waiter( &high );
    s = thread( alerter, NULL );
    join( s, 60000 );
    high.stop = above.stop = 1;  /* both wait for their next turn */
    ReleaseSemaphore( high.go, 1, NULL );
    ReleaseSemaphore( above.go, 1, NULL );
    join( h, 10000 );
    join( a, 10000 );
    CloseHandle( high.go );
    CloseHandle( above.go );
    printf( "gw2 test spin: %d rounds of an alert to each thread, every one taken\n", ALERT_ROUNDS );
}

/* pulse: PulseEvent during an event spin */

static HANDLE pulse_event;
static HANDLE pulse_go;  /* a semaphore the pulser releases for each round */
/* the waiter's last round armed and done, and its result */
static volatile LONG pulse_armed, pulse_taken, pulse_stop;
static volatile DWORD pulse_result;
static DWORD pulse_tid;

#define PULSE_ROUNDS 2000
#define PULSE_WAIT_MS 50

/* waits for round after round that the pulser starts, each with one wait */
static DWORD WINAPI pulse_waiter( void *arg )
{
    LONG i;

    for (i = 1;; i++)
    {
        if (WaitForSingleObject( pulse_go, 10000 ) != WAIT_OBJECT_0) return 1;
        if (pulse_stop) return 0;
        InterlockedExchange( &pulse_armed, i );
        pulse_result = WaitForSingleObject( pulse_event, PULSE_WAIT_MS );
        InterlockedExchange( &pulse_taken, i );
        if (pulse_result != WAIT_OBJECT_0 && pulse_result != WAIT_TIMEOUT) return 1;
    }
}

static void test_pulse(void)
{
    LONGLONG span = us_ticks( 2 * (spin[EVENT].on ? spin[EVENT].us : 30) );
    LONG i, pulsed = 0, late = 0, missed = 0, slept_missed = 0, seen;
    DWORD t0;
    BOOL sent;
    HANDLE w;

    start( "pulse" );
    if (!(pulse_event = CreateEventW( NULL, FALSE, FALSE, NULL ))) FAIL( "CreateEvent: error %lu", GetLastError() );
    if (!(pulse_go = CreateSemaphoreW( NULL, 0, 1, NULL ))) FAIL( "CreateSemaphore: error %lu", GetLastError() );
    w = thread( pulse_waiter, NULL );
    pulse_tid = GetThreadId( w );
    for (i = 1; i <= PULSE_ROUNDS; i++)
    {
        ReleaseSemaphore( pulse_go, 1, NULL );
        await( &pulse_armed, i, 2000, "the waiter didn't get to its wait", i );
        busy( i % 16 ? span / 4 + span * (i % 32) / 32 : us_ticks( 3000 ) );
        /* this thread was off its core longer than the wait: the round tests nothing */
        if (!(sent = pulse_taken != i)) late++;
        else if (!PulseEvent( pulse_event )) FAIL( "PulseEvent: error %lu", GetLastError() );
        t0 = GetTickCount();
        while ((seen = pulse_taken) != i)
        {
            if (seen > i) FAIL( "round %ld: the waiter ran ahead to round %ld", i, seen );
            if (GetTickCount() - t0 > 2000)
                FAIL( "round %ld: the waiter didn't come back, its last round done is %ld", i, seen );
            YieldProcessor();
        }
        if (pulse_result != WAIT_OBJECT_0 && pulse_result != WAIT_TIMEOUT)
            FAIL( "round %ld: the wait returned %#lx", i, pulse_result );
        if (!sent && pulse_result == WAIT_OBJECT_0) FAIL( "round %ld: the wait took a pulse nobody sent", i );
        if (!sent) continue;
        pulsed++;
        if (pulse_result == WAIT_TIMEOUT)
        {
            missed++;
            if (!(i % 16)) slept_missed++;
        }
    }
    pulse_stop = 1;
    ReleaseSemaphore( pulse_go, 1, NULL );
    join( w, 10000 );
    i--;
    printf( "gw2 test spin: %ld rounds, %ld pulsed, %ld with the pulser late; %ld waits timed out "
            "(a pulse before the wait, or a lost one): %ld in the sweep, %ld of the 3 ms rounds\n",
            i, pulsed, late, missed, missed - slept_missed, slept_missed );
    if (late * 10 > i) FAIL( "the pulser was late in %ld of %ld rounds, more than 10%%: too few pulses tested", late, i );
    if (missed * 50 > pulsed) FAIL( "%ld of %ld pulsed waits timed out, more than 2%%", missed, pulsed );
    CloseHandle( pulse_event );
    CloseHandle( pulse_go );
}

/* what the counters say */

static void check_counters(void)
{
    long long n;
    int k, i;

    read_log();
    for (k = 0; k < KINDS; k++)
        if (!spin[k].on && (n = total( hit_names[k] ) + total( miss_names[k] )))
            FAIL( "%s is off, and %lld of its spins were counted", kind_names[k], n );

    show( "the HIGHEST alerts thread", high.tid, LOCK );
    show( "the ABOVE_NORMAL alerts thread", above.tid, LOCK );
    show( "the game thread", game_tid, EVENT );
    show( "the pulse waiter", pulse_tid, EVENT );
    for (i = 0; i < WORKERS; i++) show( "a worker", worker_tid[i], SEM );
    printf( "gw2 test spin: the game thread: release %lld wake_skipped %lld wake_one %lld wake_none %lld\n",
            counter( game_tid, "release" ), counter( game_tid, "wake_skipped" ), counter( game_tid, "wake_one" ),
            counter( game_tid, "wake_none" ) );

    if (spins( LOCK, THREAD_PRIORITY_HIGHEST ) && !counter( high.tid, "lspin_hit" ))
        FAIL( "the HIGHEST thread never took an alert while it spun" );
    if (spins( LOCK, THREAD_PRIORITY_ABOVE_NORMAL ) && !counter( above.tid, "lspin_hit" ))
        FAIL( "the ABOVE_NORMAL thread never took an alert while it spun, with its priority in scope" );
    if (spin[LOCK].on && !spins( LOCK, THREAD_PRIORITY_ABOVE_NORMAL ))
    {
        if (spun( above.tid, LOCK )) FAIL( "the ABOVE_NORMAL thread spun, with its priority out of scope" );
        if (!counter( above.tid, "spin_scoped" )) FAIL( "the ABOVE_NORMAL thread counted no spin_scoped" );
    }
    if (spins( LOCK, 0 ))
    {
        for (i = 0, n = 0; i < CS_THREADS; i++) n += spun( cs_tid[i], LOCK );
        if (!n) FAIL( "no cs thread spun" );
    }
    if (spins( EVENT, THREAD_PRIORITY_HIGHEST ) && !spun( game_tid, EVENT ))
        FAIL( "the game thread never spun on its jobs' event" );
    if (spin[EVENT].on && !spins( EVENT, THREAD_PRIORITY_HIGHEST ) &&
        (spun( game_tid, EVENT ) || !counter( game_tid, "spin_scoped" )))
        FAIL( "the game thread's event spins, out of scope" );
    if (spins( EVENT, 0 ) && !counter( pulse_tid, "espin_hit" ))
        FAIL( "a pulse never ended the waiter's spin" );
    for (i = 0; i < WORKERS; i++)
    {
        if (spins( SEM, THREAD_PRIORITY_ABOVE_NORMAL ) && !spun( worker_tid[i], SEM ))
            FAIL( "a worker never spun on the semaphore" );
        if (spin[SEM].on && !spins( SEM, THREAD_PRIORITY_ABOVE_NORMAL ) &&
            (spun( worker_tid[i], SEM ) || !counter( worker_tid[i], "spin_scoped" )))
            FAIL( "a worker's semaphore spins, out of scope" );
    }
}

int main(void)
{
    setvbuf( stdout, NULL, _IONBF, 0 );
    QueryPerformanceFrequency( &freq );
    read_spin();

    test_cs();
    test_srw();
    test_jobs( 20 );
    test_jobs( 100 );
    test_alerts();
    test_pulse();
    if (counting) check_counters();

    printf( "gw2 test spin: passed\n" );
    return 0;
}
