/*
 * GW2 on Mac: the WINE_HEAT_* switches of one process, and its counters
 *
 * Everything here is off unless one of these is set and the process's image (the last part of
 * its ImagePathName) is WINE_HEAT_EXE, default Gw2-64.exe. The decision is made once, in
 * heat_init() right after load_compatdb(), since a compatdb rule's env= entries arrive in that
 * call:
 *
 *   WINE_HEAT_COUNT=1     per-thread counters (enum heat_counter), which a thread of their own
 *                         adds up and writes to $WINE_HEAT_LOG/heat-<pid>.log (default /tmp)
 *                         every 5 seconds and at exit. No clock is read while it is off.
 *                         The server requests of the game thread and two more go to
 *                         heat-requests-<pid>.log (jdrly/gw2-on-mac#60, heatreq.c).
 *                         With FEX_PROFILESTATS=1 each of those blocks has its thread's FEX
 *                         counters (jdrly/gw2-on-mac#111).
 *   WINE_HEAT_WAKE=one    a semaphore release of count n makes at most n wake-one calls and
 *                         stops at the first that finds no waiter. msync wakes every waiter on
 *                         every release. Events and mutexes keep that (msync.c).
 *   WINE_HEAT_WAKE=skip   a release of a private semaphore with no thread of this process
 *                         asleep on it makes no wake call (heatwake.c). "one,skip" for both.
 *   WINE_HEAT_EVPOOL=1    NtClose of an event that only this process can hold parks the handle,
 *                         and NtCreateEvent takes one that has been parked for 2 ms: neither asks
 *                         the server (jdrly/gw2-on-mac#42, heatpool.c). "1,mask" blocks signals
 *                         around the pool's mutex, as 0.1.x did, instead of the per-thread flag.
 *   WINE_HEAT_SPIN=lock=<us>,event=<us>,sem=<us>  spin up to that long (1 to 1000 us, each kind
 *                         optional) before a lock sleep, a sleep on events only, or a single
 *                         wait's sleep on a semaphore (jdrly/gw2-on-mac#45, heatspin.c).
 *                         A kind may end in a scope: ":not=<p>" leaves out the threads whose
 *                         priority is p, and repeats; ":game" is ":not=1", GW2's ABOVE_NORMAL
 *                         workers; ":all", the default, leaves out none.
 *   WINE_HEAT_SPIN_PAUSE=<n>  waits without a spin after 4 misses in a row (default 32)
 *   WINE_HEAT_PROF=1      the game thread's sampler under FEX (heatprof.c, jdrly/gw2-on-mac#43).
 *                         With FEX_PROFILESTATS=1 the log also gets the game thread's FEX counters.
 *
 * See jdrly/gw2-on-mac#40 and, for 0.1.x's version of the same switches, gw2-on-mac's
 * docs/research/heat-locks-2026-10-29.md. This file has the switches, heat_init(), the counters
 * and the dumper; heat_private.h has what the five files share (jdrly/gw2-on-mac#106), and
 * heatpool.c has heat_mode and why.
 *
 * The counters file has a "total" line and a "thread <tid>" line per thread, each with name and value
 * pairs: the counters of enum heat_counter, then the timers in milliseconds (*_ms). A thread's line
 * ends in "start <hex> name <text>": its start address, and the name it gave itself
 * (SetThreadDescription), with _ for a space and - for no name. Since jdrly/gw2-on-mac#99:
 *
 *   lock_ms      the time in NtWaitForAlertByThreadId's sleeps, which lock_sleep counts
 *   lock_5us lock_20us lock_100us lock_1ms lock_long  those sleeps by length: under 5 us, 20 us,
 *                100 us, 1 ms, and longer, as the yield's
 *   wait_sleep   the sleeps in msync_wait_single and msync_wait_multiple, timed in wait_ms
 *   wait_5us wait_20us wait_100us wait_1ms wait_long  those sleeps by length
 *   wake_ms      the time in signal_all's wake calls, and in the one a wait-all passes on
 *   alert        NtAlertThreadByThreadId calls; alert_wake those that made the wake call, timed in
 *                alert_ms
 *   event_set    msync_set_event_obj calls; event_wake and event_wake_none the wake calls on an event,
 *                a set's or a pulse's, that woke a thread and that found none
 *   wake_none    as before, but on semaphores and mutexes only: up to jdrly/gw2-on-mac#99 it counted
 *                events too
 *   wait         msync_wait_objs calls; wait_timed those with a timeout neither infinite nor zero,
 *                wait_poll those with a zero timeout
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#if 0
#pragma makedep unix
#endif

#include "config.h"

#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <mach/mach_time.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winternl.h"
#include "unix_private.h"
#include "heat.h"
#include "heatprof.h"
#define HEAT_INLINE  /* this file has heat_add(), heat_now() and heat_time() */
#include "heat_private.h"

/***********************************************************************
 *  counters
 *
 * Each thread writes only its own row, so no cache line moves between threads. The rows come
 * from one mapping made at the start, since a count can happen in a signal handler, where
 * malloc isn't safe, and they are never freed: a thread's counts stay in the totals after it
 * ends.
 *
 * The rows, heat_row(), heat_add(), heat_now() and heat_time() are in heat_private.h, since
 * heatreq.c, heatwake.c, heatpool.c and heatspin.c count too.
 */
static char heat_spin_line[256];  /* the log's line for WINE_HEAT_SPIN, empty without it */

struct heat_row *heat_rows;
unsigned int heat_nrows;
__thread struct heat_row *heat_self;
mach_timebase_info_data_t heat_timebase;
struct heat_fex_clock heat_fex_started;  /* jdrly/gw2-on-mac#156 */
static char heat_log_path[1024];
static char heat_exe[256];
static unsigned long long heat_started;

/* a sleep that began at <start>: counted as <c>, timed in <t>, and in the bucket of its length among
 * the five from <first> (under 5 us, 20 us, 100 us, 1 ms, and longer) */
static void heat_slept( enum heat_counter c, enum heat_counter first, enum heat_timer t, unsigned long long start )
{
    struct heat_row *row = heat_row();
    unsigned long long d = heat_now() - start;
    unsigned int bucket;

    if (!row) return;
    if (d < 5000) bucket = 0;
    else if (d < 20000) bucket = 1;
    else if (d < 100000) bucket = 2;
    else if (d < 1000000) bucket = 3;
    else bucket = 4;
    heat_bump( &row->c[c], 1 );
    heat_bump( &row->c[first + bucket], 1 );
    heat_bump( &row->t[t], d );
}

void heat_yield_done( unsigned long long start )
{
    heat_slept( HC_YIELD, HC_YIELD_5US, HT_YIELD, start );
}

/***********************************************************************
 *  sleeps and wakes (WINE_HEAT_COUNT=1, jdrly/gw2-on-mac#99)
 *
 * Each call site reads the clock through heat_start(), which is 0 without counting, and calls
 * one of these only when it isn't.
 */

/* NtWaitForAlertByThreadId's futex_wait returned */
void heat_lock_slept( unsigned long long start )
{
    heat_slept( HC_LOCK_SLEEP, HC_LOCK_5US, HT_LOCK, start );
}

/* msync_wait_single's or msync_wait_multiple's ulock_wait returned */
void heat_wait_slept( unsigned long long start )
{
    heat_slept( HC_WAIT_SLEEP, HC_WAIT_5US, HT_WAIT, start );
}

/* signal_all's wake-all call returned: on an event or not, and whether it found a waiter */
void heat_wake_done( BOOL event, BOOL found, unsigned long long start )
{
    struct heat_row *row = heat_row();

    if (!row) return;
    if (event) heat_bump( &row->c[found ? HC_EVENT_WAKE : HC_EVENT_WAKE_NONE], 1 );
    else if (!found) heat_bump( &row->c[HC_WAKE_NONE], 1 );
    heat_bump( &row->t[HT_WAKE], heat_now() - start );
}

/* time without a count of its own */
void heat_add_time( enum heat_timer t, unsigned long long start )
{
    struct heat_row *row = heat_row();
    if (row) heat_bump( &row->t[t], heat_now() - start );
}

void heat_wait_counted( const LARGE_INTEGER *timeout )
{
    struct heat_row *row = heat_row();

    if (!row) return;
    heat_bump( &row->c[HC_WAIT], 1 );
    if (timeout && !timeout->QuadPart) heat_bump( &row->c[HC_WAIT_POLL], 1 );
    else if (timeout && timeout->QuadPart != TIMEOUT_INFINITE) heat_bump( &row->c[HC_WAIT_TIMED], 1 );
}

/* Only the row's own thread writes its name, a byte at a time, so the dumper reads no torn byte;
 * a dump during a rename may print a mix of the two names. */
void heat_named( const char *name )
{
    struct heat_row *row = heat_row();
    unsigned int i;

    if (!row) return;
    for (i = 0; i < sizeof(row->name) - 1 && name[i]; i++) __atomic_store_n( &row->name[i], name[i], __ATOMIC_RELAXED );
    __atomic_store_n( &row->name[i], 0, __ATOMIC_RELAXED );
}

/* <row>'s name as one word of the log: _ for a space or a control character, - for no name */
static void heat_row_name( const struct heat_row *row, char *out )
{
    unsigned int i;
    char c;

    for (i = 0; i < sizeof(row->name) - 1 && (c = __atomic_load_n( &row->name[i], __ATOMIC_RELAXED )); i++)
        out[i] = (unsigned char)c <= ' ' || c == 0x7f ? '_' : c;
    if (!i) out[i++] = '-';
    out[i] = 0;
}

static int heat_print_row( char *buf, size_t size, const unsigned long long *c, const unsigned long long *t )
{
    int i, len = 0;

    for (i = 0; i < HC_NB && len < size; i++)
        len += snprintf( buf + len, size - len, " %s %llu", heat_counter_names[i], c[i] );
    for (i = 0; i < HT_NB && len < size; i++)
        len += snprintf( buf + len, size - len, " %s %.3f", heat_timer_names[i], t[i] / 1e6 );
    return len < size ? len : size - 1;
}

/* the whole file again: a header, the totals, then one line per thread */
static void heat_dump(void)
{
    static char buf[1 << 20];
    unsigned long long total_c[HC_NB] = {0}, total_t[HT_NB] = {0}, c[HC_NB], t[HT_NB];
    unsigned int i, j, n = __atomic_load_n( &heat_nrows, __ATOMIC_RELAXED );
    char name[sizeof(heat_rows->name)];
    int len, fd;

    if (n > HEAT_ROWS) n = HEAT_ROWS;
    for (i = 0; i < n; i++)
    {
        for (j = 0; j < HC_NB; j++) total_c[j] += __atomic_load_n( &heat_rows[i].c[j], __ATOMIC_RELAXED );
        for (j = 0; j < HT_NB; j++) total_t[j] += __atomic_load_n( &heat_rows[i].t[j], __ATOMIC_RELAXED );
    }
    len = snprintf( buf, sizeof(buf), "heat pid %d exe %s mode%s%s%s%s%s seconds %.1f threads %u\n%s",
                    getpid(), heat_exe, heat_mode & HEAT_COUNT ? " count" : "",
                    heat_mode & HEAT_WAKE_ONE ? " one" : "", heat_mode & HEAT_WAKE_SKIP ? " skip" : "",
                    heat_mode & HEAT_EVPOOL_MASK ? " evpool,mask" : heat_mode & HEAT_EVPOOL ? " evpool" : "",
                    heat_mode & HEAT_PROF ? " prof" : "", (heat_now() - heat_started) / 1e9, n, heat_spin_line );
    /* jdrly/gw2-on-mac#43: FEX's counters of the game thread, when FEX keeps them */
    if (heat_mode & HEAT_PROF) len += heat_prof_fex_stats( buf + len, sizeof(buf) - len );
    if (heat_mode & HEAT_COUNT)
    {
        len += snprintf( buf + len, sizeof(buf) - len, "total" );
        len += heat_print_row( buf + len, sizeof(buf) - len, total_c, total_t );
        len += snprintf( buf + len, sizeof(buf) - len, "\n" );
    }
    for (i = 0; i < n && len < sizeof(buf) - 1; i++)
    {
        for (j = 0; j < HC_NB; j++) c[j] = __atomic_load_n( &heat_rows[i].c[j], __ATOMIC_RELAXED );
        for (j = 0; j < HT_NB; j++) t[j] = __atomic_load_n( &heat_rows[i].t[j], __ATOMIC_RELAXED );
        len += snprintf( buf + len, sizeof(buf) - len, "thread %04x",
                         __atomic_load_n( &heat_rows[i].tid, __ATOMIC_RELAXED ) );
        if (len < sizeof(buf) - 1) len += heat_print_row( buf + len, sizeof(buf) - len, c, t );
        /* jdrly/gw2-on-mac#99: last, so that a reader of name and value pairs reads them as two more */
        heat_row_name( &heat_rows[i], name );
        if (len < sizeof(buf) - 1)
            len += snprintf( buf + len, sizeof(buf) - len, " start %#llx name %s\n",
                             (unsigned long long)__atomic_load_n( &heat_rows[i].start, __ATOMIC_RELAXED ), name );
    }
    if (len > sizeof(buf) - 1) len = sizeof(buf) - 1;
    if ((fd = open( heat_log_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644 )) == -1) return;
    write( fd, buf, len );
    close( fd );
}

/* the server requests' log (heatreq.c has the rules). heat_req_dump() is here since
 * heat_dump_locked() inlines it. */
static char heat_req_path[1024];
static BOOL heat_req_fex;                    /* FEX_PROFILESTATS=1 (jdrly/gw2-on-mac#111) */

/* Under heat_dump_mutex. The game thread's block, then those of the two other threads with the
 * most server time since the last dump (jdrly/gw2-on-mac#99), in one write. */
static void heat_req_dump(void)
{
    static char buf[3 * 8192];
    unsigned long long server, d, most[2] = {0};
    unsigned int i, game = 0, rows = __atomic_load_n( &heat_nrows, __ATOMIC_RELAXED ), busy[2];
    double now = (heat_now() - heat_started) / 1e9;
    const unsigned char *fex = NULL;
    size_t fex_size = 0;
    int len, fd;

    if (rows > HEAT_ROWS) rows = HEAT_ROWS;
    if (!rows) return;
    for (i = 1; i < rows; i++)
        if (__atomic_load_n( &heat_rows[i].c[HC_RELEASE], __ATOMIC_RELAXED ) >
            __atomic_load_n( &heat_rows[game].c[HC_RELEASE], __ATOMIC_RELAXED ))
            game = i;
    busy[0] = busy[1] = rows;
    for (i = 0; i < rows; i++)
    {
        server = __atomic_load_n( &heat_rows[i].t[HT_SERVER], __ATOMIC_RELAXED );
        d = server - heat_reqs_prev[i].server;
        heat_reqs_prev[i].server = server;
        if (i == game || !d) continue;
        if (d > most[0])
        {
            busy[1] = busy[0];
            most[1] = most[0];
            busy[0] = i;
            most[0] = d;
        }
        else if (d > most[1])
        {
            busy[1] = i;
            most[1] = d;
        }
    }

    /* jdrly/gw2-on-mac#111: no fex line while FEX hasn't made its stats object, or makes none */
    if (heat_req_fex) fex = heat_prof_fex_map( &fex_size );
    len = heat_req_block( buf, sizeof(buf), game, now, fex, fex_size );
    for (i = 0; i < 2 && busy[i] < rows; i++)
        len += heat_req_block( buf + len, sizeof(buf) - len, busy[i], now, fex, fex_size );
    if (fex) munmap( (void *)fex, fex_size );

    if ((fd = open( heat_req_path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644 )) == -1) return;
    write( fd, buf, len );
    close( fd );
}

static pthread_mutex_t heat_dump_mutex = PTHREAD_MUTEX_INITIALIZER;

static void heat_dump_locked(void)
{
    pthread_mutex_lock( &heat_dump_mutex );
    heat_dump();
    if (heat_mode & HEAT_COUNT) heat_req_dump();  /* jdrly/gw2-on-mac#60 */
    pthread_mutex_unlock( &heat_dump_mutex );
}

static void *heat_dumper( void *arg )
{
    for (;;)
    {
        sleep( 5 );
        heat_dump_locked();
    }
    return NULL;
}

/***********************************************************************
 *  WINE_HEAT_SPIN (heatspin.c has the rules)
 */
static const char * const heat_spin_names[HEAT_SPIN_KINDS] = { "lock", "event", "sem" };

/* "<kind>=<us>[:all|:game|:not=<p>]...", the kinds separated by commas. A kind with a time out of
 * range or a word it doesn't know stays off. Returns the HEAT_SPIN_* bits of the kinds on. */
static unsigned int heat_spin_parse( const char *spec )
{
    char buf[256], *item, *save = NULL, *p, *end;
    unsigned int mode = 0, kind, not;
    size_t n;
    long us, prio;
    BOOL ok;

    snprintf( buf, sizeof(buf), "%s", spec );
    for (item = strtok_r( buf, ",", &save ); item; item = strtok_r( NULL, ",", &save ))
    {
        if (!(p = strchr( item, '=' ))) continue;
        *p++ = 0;
        for (kind = 0; kind < HEAT_SPIN_KINDS; kind++) if (!strcmp( item, heat_spin_names[kind] )) break;
        if (kind == HEAT_SPIN_KINDS) continue;
        us = strtol( p, &end, 10 );
        if (end == p || us < 1 || us > 1000) continue;
        not = 0;
        ok = TRUE;
        for (p = end; ok && *p; p += n)
        {
            if (*p++ != ':') { ok = FALSE; break; }
            n = strcspn( p, ":" );
            if (n == 3 && !strncmp( p, "all", 3 )) not = 0;
            else if (n == 4 && !strncmp( p, "game", 4 )) not |= HEAT_PRIORITY_BIT( 1 );
            else if (n > 4 && !strncmp( p, "not=", 4 ) && (prio = strtol( p + 4, &end, 10 )) >= -15 &&
                     prio <= 15 && end == p + n)
                not |= HEAT_PRIORITY_BIT( prio );
            else ok = FALSE;
        }
        if (!ok) continue;
        heat_spin_ns[kind] = us * 1000;
        heat_spin_not[kind] = not;
        mode |= HEAT_SPIN_LOCK << kind;
    }
    return mode;
}

/* the log's line: each kind's time and the priorities it leaves out */
static void heat_spin_describe( unsigned int mode )
{
    size_t len = snprintf( heat_spin_line, sizeof(heat_spin_line), "spin" );
    unsigned int kind;
    int prio;

    for (kind = 0; kind < HEAT_SPIN_KINDS && len < sizeof(heat_spin_line); kind++)
    {
        if (!(mode & (HEAT_SPIN_LOCK << kind)))
        {
            len += snprintf( heat_spin_line + len, sizeof(heat_spin_line) - len, " %s=off", heat_spin_names[kind] );
            continue;
        }
        len += snprintf( heat_spin_line + len, sizeof(heat_spin_line) - len, " %s=%lluus",
                         heat_spin_names[kind], heat_spin_ns[kind] / 1000 );
        if (!heat_spin_not[kind] && len < sizeof(heat_spin_line))
            len += snprintf( heat_spin_line + len, sizeof(heat_spin_line) - len, ":all" );
        for (prio = -15; prio <= 15 && len < sizeof(heat_spin_line); prio++)
            if (heat_spin_not[kind] & HEAT_PRIORITY_BIT( prio ))
                len += snprintf( heat_spin_line + len, sizeof(heat_spin_line) - len, ":not=%d", prio );
    }
    if (len < sizeof(heat_spin_line))
        snprintf( heat_spin_line + len, sizeof(heat_spin_line) - len, " pause %u\n", heat_spin_pause );
    else
        heat_spin_line[sizeof(heat_spin_line) - 2] = '\n';
}

/***********************************************************************
 *  heat_init
 *
 * Called once, from start_main_thread() right after load_compatdb(), while this is the only
 * thread.
 */
static BOOL heat_is_exe( const char *want )
{
    const UNICODE_STRING *path = &peb->ProcessParameters->ImagePathName;
    unsigned int len = path->Length / sizeof(WCHAR), start = len, i;

    while (start && path->Buffer[start - 1] != '\\' && path->Buffer[start - 1] != '/') start--;
    for (i = 0; start + i < len; i++)
    {
        WCHAR c = path->Buffer[start + i];
        if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
        if (!want[i] || c != (want[i] >= 'A' && want[i] <= 'Z' ? want[i] + 'a' - 'A' : want[i])) return FALSE;
    }
    return !want[i];
}

static void *heat_map( size_t size )
{
    void *ptr = mmap( NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0 );
    return ptr == MAP_FAILED ? NULL : ptr;
}

void heat_init(void)
{
    const char *count = getenv( "WINE_HEAT_COUNT" ), *wake = getenv( "WINE_HEAT_WAKE" );
    const char *evpool = getenv( "WINE_HEAT_EVPOOL" );
    const char *exe = getenv( "WINE_HEAT_EXE" ), *dir = getenv( "WINE_HEAT_LOG" ), *p;
    unsigned int mode = 0;
    sigset_t all, old;
    pthread_t thread;

    if (count && !strcmp( count, "1" )) mode |= HEAT_COUNT;
    for (p = wake; p && *p; p += strcspn( p, "," ), p += (*p == ','))
    {
        size_t n = strcspn( p, "," );
        if (n == 3 && !strncmp( p, "one", 3 )) mode |= HEAT_WAKE_ONE;
        else if (n == 4 && !strncmp( p, "skip", 4 )) mode |= HEAT_WAKE_SKIP;
    }
    if (evpool && !strcmp( evpool, "1" )) mode |= HEAT_EVPOOL;
    else if (evpool && !strcmp( evpool, "1,mask" )) mode |= HEAT_EVPOOL | HEAT_EVPOOL_MASK;
    /* jdrly/gw2-on-mac#45 */
    if ((p = getenv( "WINE_HEAT_SPIN" ))) mode |= heat_spin_parse( p );
    if ((p = getenv( "WINE_HEAT_SPIN_PAUSE" )) && *p)
    {
        char *end;
        unsigned long n = strtoul( p, &end, 10 );
        if (!*end && n <= 1000000) heat_spin_pause = n;
    }
    /* jdrly/gw2-on-mac#43 */
    if ((p = getenv( "WINE_HEAT_PROF" )) && !strcmp( p, "1" )) mode |= HEAT_PROF;
    if (!mode) return;
    if (!exe || !*exe) exe = "Gw2-64.exe";
    if (!peb || !peb->ProcessParameters || !heat_is_exe( exe )) return;

    if (!(heat_handles = heat_map( HEAT_HANDLES ))) return;
    if ((mode & HEAT_WAKE_SKIP) && !(heat_waiters = heat_map( HEAT_SLOTS * sizeof(int) )))
        mode &= ~HEAT_WAKE_SKIP;
    if ((mode & HEAT_COUNT) && !(heat_rows = heat_map( HEAT_ROWS * sizeof(*heat_rows) )))
        mode &= ~HEAT_COUNT;
    if ((mode & HEAT_EVPOOL) && !(heat_pools = heat_map( 2 * sizeof(*heat_pools) )))
        mode &= ~(HEAT_EVPOOL | HEAT_EVPOOL_MASK);
    if (mode & HEAT_EVPOOL) mach_timebase_info( &heat_timebase );  /* heat_now() */
    /* jdrly/gw2-on-mac#45 */
    if ((mode & HEAT_SPIN_EVENT) && !(heat_spinners = heat_map( HEAT_SLOTS * sizeof(*heat_spinners) )))
        mode &= ~HEAT_SPIN_EVENT;
    /* jdrly/gw2-on-mac#60: the rows' requests, then the dumper's copy of them */
    if ((mode & HEAT_COUNT) && !(heat_reqs = heat_map( 2 * HEAT_ROWS * sizeof(*heat_reqs) )))
        mode &= ~HEAT_COUNT;
    if (mode & HEAT_COUNT)
    {
        heat_reqs_prev = heat_reqs + HEAT_ROWS;
        snprintf( heat_req_path, sizeof(heat_req_path), "%s/heat-requests-%d.log", dir && *dir ? dir : "/tmp", getpid() );
        /* jdrly/gw2-on-mac#111: FEX's own switch, which a rule's env= sets for FEX and this file alike */
        heat_req_fex = (p = getenv( "FEX_PROFILESTATS" )) && !strcmp( p, "1" );
    }
    if (mode & (HEAT_COUNT | HEAT_PROF))
    {
        mach_timebase_info( &heat_timebase );
        heat_started = heat_now();
        heat_fex_clock_read( &heat_fex_started );
        snprintf( heat_exe, sizeof(heat_exe), "%s", exe );
        snprintf( heat_log_path, sizeof(heat_log_path), "%s/heat-%d.log", dir && *dir ? dir : "/tmp", getpid() );
    }
    /* jdrly/gw2-on-mac#45: a spin reads the clock whether or not it counts */
    if (mode & HEAT_SPIN_ANY)
    {
        mach_timebase_info( &heat_timebase );
        heat_spin_describe( mode );
    }
    /* jdrly/gw2-on-mac#43: the sampler's thread; its handler comes once FEX is loaded and checked */
    if ((mode & HEAT_PROF) && !heat_prof_init( dir && *dir ? dir : "/tmp" )) mode &= ~HEAT_PROF;
    heat_mode = mode;
    /* the dumper prints the counters, and with the sampler FEX's counters */
    if (mode & (HEAT_COUNT | HEAT_PROF))
    {
        /* the dumper isn't a Wine thread: no signal of Wine's may land on it */
        sigfillset( &all );
        pthread_sigmask( SIG_SETMASK, &all, &old );
        if (!pthread_create( &thread, NULL, heat_dumper, NULL )) pthread_detach( thread );
        pthread_sigmask( SIG_SETMASK, &old, NULL );
        atexit( heat_dump_locked );
    }
}
