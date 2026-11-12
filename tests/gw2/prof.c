/* gw2 (jdrly/gw2-on-mac#43): the sampler of WINE_HEAT_PROF on a program whose code it knows.
 *
 * A thread of this program, started in the exe and unnamed like GW2's game thread, loops over
 * four functions: hot_a and hot_b, the same loop at 600,000 and 400,000 turns (a 60 to 40 split),
 * sys_caller, 20 calls of ntdll's NtYieldExecution, and waiter, a 1 ms wait on an event nobody
 * sets. It runs GW2_TEST_PROF_SECONDS (default 8): at 1000 samples a second that is about 8,000
 * samples, 1% of them in the system call, enough for every count below.
 *
 * With WINE_HEAT_PROF=1 and WINE_HEAT_EXE=prof.exe, the program then reads the sampler's files in
 * WINE_HEAT_LOG (heatprof.h has their format) and checks what doesn't depend on the runner's load:
 *   - FEX is the pinned one, and samples of that thread arrived, at least 50 a second
 *   - none was lost: every signal sent made a sample, the ring dropped none, and the file holds
 *     each one
 *   - at least 90% of the samples in translated code name an x86 address inside this program's
 *     functions (their .pdata ranges), both hot functions have samples, and hot_a's share of the
 *     two is between 50% and 70%; the share is printed, but a 3-CPU virtual runner is no place to
 *     hold it to a point
 *   - samples in Wine's unix side name sys_caller as the x86 caller (the call and return stack's
 *     last return address, or the first return address of this program on the guest stack), and
 *     others waiter: the system call under its caller, and the wait
 * With FEX_PROFILESTATS=1 as well, the heat log must get a line of FEX's counters for the thread
 * while it runs: heat.c's dumper writes every 5 s, and FEX frees the thread's slot when it ends,
 * so the thread runs on after its time until the line came, 10 s more at most.
 *
 * Without the sampler for it, the program runs its thread for a second and passes: matrix.zsh
 * runs it under the engine's ntdll.so and under mine with the variables unset or set for another
 * program, where no file may appear. Prints "gw2 test prof: passed" and exits 0, or names what
 * failed and exits 1. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#define HOT_A 600000
#define HOT_B 400000
#define YIELDS 20

/* heatprof.h's struct heat_prof_rec without stack[], and its flags */
struct rec
{
    unsigned long long t;
    unsigned int       tid;
    unsigned short     target, flags;
    unsigned long long regs[34];
    unsigned long long sys_sp, sys_pc, x86, block, stack_at;
    unsigned int       stack_len, to_tail;
    unsigned long long callret[2];
    unsigned char      tail[40];
    unsigned char      state[256];
};
#define HP_SIM     0x0001
#define HP_SYSCALL 0x0004
#define HP_CALLRET 0x0010
#define HP_X86     0x0100
#define HP_FEX     0x0200
#define HP_GONE    0x0400

#define FAIL(...) do { \
        printf( "gw2 test prof: failed, line %d: ", __LINE__ ); \
        printf( __VA_ARGS__ ); \
        printf( "\n" ); \
        fflush( stdout ); \
        exit( 1 ); \
    } while (0)

static volatile unsigned int sink;
static volatile LONG stop;
static HANDLE never;
static LONG (WINAPI *pNtYieldExecution)(void);

/* every function below calls one, so each has a frame and a .pdata entry */
static __attribute__((noinline)) void keep( unsigned int x )
{
    sink = x;
}

static __attribute__((noinline)) void hot_a(void)
{
    unsigned int x = 0x12345678, i;
    for (i = 0; i < HOT_A; i++) { x ^= x << 13; x ^= x >> 17; x ^= x << 5; }
    keep( x );
}

static __attribute__((noinline)) void hot_b(void)
{
    unsigned int x = 0x87654321, i;
    for (i = 0; i < HOT_B; i++) { x ^= x << 13; x ^= x >> 17; x ^= x << 5; }
    keep( x );
}

static __attribute__((noinline)) void sys_caller(void)
{
    unsigned int i;
    for (i = 0; i < YIELDS; i++) pNtYieldExecution();
    keep( i );
}

static __attribute__((noinline)) void waiter(void)
{
    keep( WaitForSingleObject( never, 1 ) );
}

static DWORD WINAPI game( void *arg )
{
    while (!stop)
    {
        hot_a();
        hot_b();
        sys_caller();
        waiter();
    }
    return 0;
}

struct range { const char *name; ULONG_PTR lo, hi; };
enum { R_HOT_A, R_HOT_B, R_SYS, R_WAIT, R_KEEP, R_GAME, R_NB };
static struct range ranges[R_NB];

static void range( int i, const char *name, void *func )
{
    DWORD64 base;
    RUNTIME_FUNCTION *f = RtlLookupFunctionEntry( (DWORD64)(ULONG_PTR)func, &base, NULL );
    if (!f) FAIL( "no .pdata entry for %s", name );
    ranges[i].name = name;
    ranges[i].lo = base + f->BeginAddress;
    ranges[i].hi = base + f->EndAddress;
    printf( "gw2 test prof: %s %#llx-%#llx\n", name, (unsigned long long)ranges[i].lo, (unsigned long long)ranges[i].hi );
}

static int which( unsigned long long addr )
{
    int i;
    for (i = 0; i < R_NB; i++) if (addr >= ranges[i].lo && addr < ranges[i].hi) return i;
    return -1;
}

static BOOL env_is( const char *name, const char *value )
{
    char buf[256];
    DWORD n = GetEnvironmentVariableA( name, buf, sizeof(buf) );
    return n && n < sizeof(buf) && !_stricmp( buf, value );
}

/* the one file matching <pattern> in WINE_HEAT_LOG, opened, or NULL if there is none */
static FILE *find_one( const char *pattern, const char *mode )
{
    char dir[MAX_PATH], path[MAX_PATH * 2];
    WIN32_FIND_DATAA fd;
    HANDLE find;
    FILE *f;

    if (!GetEnvironmentVariableA( "WINE_HEAT_LOG", dir, sizeof(dir) )) FAIL( "WINE_HEAT_LOG isn't set" );
    snprintf( path, sizeof(path), "Z:%s/%s", dir, pattern );
    if ((find = FindFirstFileA( path, &fd )) == INVALID_HANDLE_VALUE) return NULL;
    snprintf( path, sizeof(path), "Z:%s/%s", dir, fd.cFileName );
    if (FindNextFileA( find, &fd )) FAIL( "more than one %s in %s", pattern, dir );
    FindClose( find );
    if (!(f = fopen( path, mode ))) FAIL( "can't open %s", path );
    return f;
}

static FILE *open_one( const char *pattern, const char *mode )
{
    FILE *f = find_one( pattern, mode );
    if (!f) FAIL( "no %s in WINE_HEAT_LOG", pattern );
    return f;
}

/* FEX_PROFILESTATS=1: whether the heat log has the dumper's line of FEX's counters for the thread
 * yet; the dumper rewrites the file, so a read may find it empty */
static BOOL fex_stats_seen( DWORD tid )
{
    char want[32], line[4096];
    BOOL found = FALSE;
    FILE *f = find_one( "heat-*.log", "r" );

    if (!f) return FALSE;
    snprintf( want, sizeof(want), "fex thread %04lx ", tid );
    while (!found && fgets( line, sizeof(line), f ))
    {
        if (strncmp( line, want, strlen( want ) )) continue;
        found = TRUE;
        printf( "gw2 test prof: %s", line );
        if (strstr( line, "no slot" )) FAIL( "FEX keeps no counters for the thread" );
    }
    fclose( f );
    return found;
}

/* the first address on the guest's stack, or the last call's return address, in sys_caller or
 * waiter: the x86 code that called into native code */
static int caller( const struct rec *r, const unsigned char *stack )
{
    unsigned long long w;
    unsigned int i;
    int k;

    if ((r->flags & HP_CALLRET) && ((k = which( r->callret[0] )) == R_SYS || k == R_WAIT)) return k;
    for (i = 0; i + 8 <= r->stack_len; i += 8)
    {
        memcpy( &w, stack + i, 8 );
        if ((k = which( w )) == R_SYS || k == R_WAIT) return k;
    }
    return -1;
}

static void check_samples( DWORD tid, unsigned int seconds )
{
    unsigned long long sent = 0, recorded = 0, skipped = 0, dropped = 0, late = 0;
    unsigned long long n = 0, x86 = 0, known = 0, fex = 0, gone = 0, native = 0, with_stack = 0;
    unsigned long long hot[2] = { 0 }, sys = 0, wait = 0, wait_in_unix = 0;
    unsigned int version = 0, size = 0, max_stack = 0, hz = 0, numer, denom;
    unsigned char stack[4096];
    char line[4096], want[64];
    BOOL pinned = FALSE, counted = FALSE;
    struct rec r;
    double share;
    FILE *f;
    int k;

    /* heat-prof-<pid>.txt: FEX as found, and the last counts of this thread */
    f = open_one( "heat-prof-*.txt", "r" );
    snprintf( want, sizeof(want), " game %04lx ", tid );
    while (fgets( line, sizeof(line), f ))
    {
        if (line[0] == 'F')
        {
            printf( "gw2 test prof: %s", line );
            pinned = strstr( line, " pinned" ) != NULL;
        }
        else if (line[0] == 'C' && strstr( line, want ) &&
                 sscanf( strstr( line, " sent " ), " sent %llu recorded %llu skipped %llu dropped %llu late %llu",
                         &sent, &recorded, &skipped, &dropped, &late ) == 5)
            counted = TRUE;
    }
    fclose( f );
    if (!pinned) FAIL( "the sampler didn't find the pinned FEX" );
    if (!counted) FAIL( "no counts for thread %04lx", tid );

    /* heat-prof-<pid>.bin */
    f = open_one( "heat-prof-*.bin", "rb" );
    if (!fgets( line, sizeof(line), f ) ||
        sscanf( line, "HEATPROF %u rec %u stack %u hz %u timebase %u/%u", &version, &size, &max_stack, &hz, &numer, &denom ) != 6)
        FAIL( "the sample file's first line: %s", line );
    if (version != 1 || size != sizeof(r) || max_stack > sizeof(stack))
        FAIL( "version %u, record %u bytes and stack %u, not 1, %u and at most %u", version, size, max_stack,
              (unsigned int)sizeof(r), (unsigned int)sizeof(stack) );
    while (fread( &r, sizeof(r), 1, f ) == 1)
    {
        if (r.stack_len > max_stack) FAIL( "a record with %u bytes of stack", r.stack_len );
        if (r.stack_len && fread( stack, r.stack_len, 1, f ) != 1) FAIL( "a record cut short" );
        if (r.tid != tid) continue;
        n++;
        if (r.stack_len) with_stack++;
        if (r.flags & HP_X86)
        {
            x86++;
            k = which( r.x86 );
            if (k != -1) known++;
            if (k == R_HOT_A || k == R_HOT_B) hot[k]++;
        }
        else if (r.flags & HP_FEX) fex++;
        else if (r.flags & HP_GONE) gone++;
        else if (!(r.flags & HP_SIM))
        {
            native++;
            k = caller( &r, stack );
            if (k == R_SYS && (r.flags & HP_SYSCALL)) sys++;
            if (k == R_WAIT)
            {
                wait++;
                if (r.flags & HP_SYSCALL) wait_in_unix++;
            }
        }
    }
    fclose( f );

    share = hot[0] + hot[1] ? (double)hot[0] / (hot[0] + hot[1]) : 0;
    printf( "gw2 test prof: thread %04lx, %u s at %u Hz: %llu samples, %llu with stack; %llu in translated code, %llu in "
            "these functions, hot_a %llu hot_b %llu (hot_a's share %.1f%%); %llu FEX, %llu gone; %llu native: the "
            "system call under sys_caller %llu, the wait under waiter %llu (%llu in the unix side)\n",
            tid, seconds, hz, n, with_stack, x86, known, hot[0], hot[1], share * 100, fex, gone, native, sys, wait,
            wait_in_unix );
    printf( "gw2 test prof: sent %llu recorded %llu skipped %llu dropped %llu late %llu\n", sent, recorded, skipped,
            dropped, late );

    if (n < 50ull * seconds) FAIL( "%llu samples in %u s, fewer than 50 a second", n, seconds );
    if (dropped || sent - recorded > 1 || n != recorded)
        FAIL( "samples lost: %llu sent, %llu recorded, %llu dropped, %llu in the file", sent, recorded, dropped, n );
    if (!with_stack) FAIL( "no sample holds the guest's stack" );
    if (!hot[0] || !hot[1]) FAIL( "a hot function has no sample" );
    if (known * 10 < x86 * 9) FAIL( "%llu of %llu translated samples outside this program's functions", x86 - known, x86 );
    if (share < 0.5 || share > 0.7) FAIL( "hot_a's share %.1f%%, not between 50%% and 70%%", share * 100 );
    if (sys < 5) FAIL( "%llu samples of the system call under sys_caller", sys );
    if (wait < 5) FAIL( "%llu samples of the wait under waiter", wait );
}

int main(void)
{
    BOOL on = env_is( "WINE_HEAT_PROF", "1" ) && env_is( "WINE_HEAT_EXE", "prof.exe" );
    unsigned int seconds = 1;
    char buf[32];
    HANDLE thread;
    DWORD tid;

    setvbuf( stdout, NULL, _IONBF, 0 );
    if (on && GetEnvironmentVariableA( "GW2_TEST_PROF_SECONDS", buf, sizeof(buf) )) seconds = atoi( buf );
    else if (on) seconds = 8;
    if (!(pNtYieldExecution = (void *)GetProcAddress( GetModuleHandleA( "ntdll.dll" ), "NtYieldExecution" )))
        FAIL( "no NtYieldExecution" );
    if (!(never = CreateEventA( NULL, TRUE, FALSE, NULL ))) FAIL( "CreateEvent: error %lu", GetLastError() );
    range( R_HOT_A, "hot_a", hot_a );
    range( R_HOT_B, "hot_b", hot_b );
    range( R_SYS, "sys_caller", sys_caller );
    range( R_WAIT, "waiter", waiter );
    range( R_KEEP, "keep", keep );
    range( R_GAME, "game", game );

    if (!(thread = CreateThread( NULL, 0, game, NULL, 0, &tid ))) FAIL( "CreateThread: error %lu", GetLastError() );
    Sleep( seconds * 1000 );
    if (on && env_is( "FEX_PROFILESTATS", "1" ))
    {
        /* the thread runs on until the line came, up to two of the dumper's 5 s more */
        unsigned int waited;
        for (waited = 0; !fex_stats_seen( tid ); waited += 250)
        {
            if (waited >= 10000) FAIL( "no line of FEX's counters for thread %04lx in the heat log while it ran", tid );
            Sleep( 250 );
        }
    }
    InterlockedExchange( &stop, 1 );
    if (WaitForSingleObject( thread, 60000 )) FAIL( "the thread didn't end" );
    CloseHandle( thread );

    if (on)
    {
        Sleep( 2500 );  /* the sampler writes each second */
        check_samples( tid, seconds );
    }
    printf( "gw2 test prof: passed\n" );
    return 0;
}
