/*
 * GW2 on Mac: counters and the semaphore wake policy for one process (WINE_HEAT_*)
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
 *                         heat-requests-<pid>.log (jdrly/gw2-on-mac#60, "server requests" below).
 *   WINE_HEAT_WAKE=one    a semaphore release of count n makes at most n wake-one calls and
 *                         stops at the first that finds no waiter. msync wakes every waiter on
 *                         every release. Events and mutexes keep that.
 *   WINE_HEAT_WAKE=skip   a release of a private semaphore with no thread of this process
 *                         asleep on it makes no wake call. "one,skip" for both.
 *   WINE_HEAT_MSYNC_UNREG=1  a multiple-object wait that finds an object signaled right after
 *                         it registered with the server unregisters there, and drops the alert
 *                         object's count, instead of dropping only the objects' counts
 *                         (dappermint/winecx 9be392b3b4, jdrly/gw2-on-mac#44). Counted as unreg.
 *   WINE_HEAT_EVPOOL=1    NtClose of an event that only this process can hold parks the handle,
 *                         and NtCreateEvent takes one that has been parked for 2 ms: neither asks
 *                         the server (jdrly/gw2-on-mac#42). "1,mask" blocks signals around the
 *                         pool's mutex, as 0.1.x did, instead of the per-thread flag.
 *   WINE_HEAT_SPIN=lock=<us>,event=<us>,sem=<us>  spin up to that long (1 to 1000 us, each kind
 *                         optional) before a lock sleep, a sleep on events only, or a single
 *                         wait's sleep on a semaphore (jdrly/gw2-on-mac#45, "the spins" below).
 *                         A kind may end in a scope: ":not=<p>" leaves out the threads whose
 *                         priority is p, and repeats; ":game" is ":not=1", GW2's ABOVE_NORMAL
 *                         workers; ":all", the default, leaves out none.
 *   WINE_HEAT_SPIN_PAUSE=<n>  waits without a spin after 4 misses in a row (default 32)
 *   WINE_HEAT_PROF=1      the game thread's sampler under FEX (heatprof.c, jdrly/gw2-on-mac#43).
 *                         With FEX_PROFILESTATS=1 the log also gets the game thread's FEX counters.
 *
 * See jdrly/gw2-on-mac#40 and, for 0.1.x's version of the same switches, gw2-on-mac's
 * docs/research/heat-locks-2026-10-29.md.
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
#include "heatreq.h"

unsigned int heat_mode;

/***********************************************************************
 *  counters
 *
 * Each thread writes only its own row, so no cache line moves between threads. The rows come
 * from one mapping made at the start, since a count can happen in a signal handler, where
 * malloc isn't safe, and they are never freed: a thread's counts stay in the totals after it
 * ends.
 */
#define HEAT_ROWS 4096

struct heat_row
{
    unsigned int       tid;
    unsigned long long c[HC_NB];
    unsigned long long t[HT_NB];  /* nanoseconds */
    /* jdrly/gw2-on-mac#99 */
    ULONG_PTR          start;     /* the thread's start address, thread_data's */
    char               name[64];  /* the name it last gave itself, empty for none */
};

static const char * const heat_counter_names[HC_NB] =
{
    "release", "wake_skipped", "wake_one", "wake_none", "woken", "spurious", "event_create",
    "event_close", "close", "server", "lock_sleep", "qpc", "yield", "yield_5us", "yield_20us", "yield_100us",
    "yield_1ms", "yield_long",
    "unreg",  /* jdrly/gw2-on-mac#44 */
    "evpool_hit", "evpool_put", "evpool_evicted", "evpool_forgotten", "evpool_busy", "evpool_double_close",
    /* jdrly/gw2-on-mac#45 */
    "lspin_hit", "lspin_miss", "espin_hit", "espin_miss", "sspin_hit", "sspin_miss", "spin_off", "spin_scoped",
    /* jdrly/gw2-on-mac#99 */
    "wait_sleep", "alert", "alert_wake", "lock_5us", "lock_20us", "lock_100us", "lock_1ms", "lock_long",
    "wait_5us", "wait_20us", "wait_100us", "wait_1ms", "wait_long", "event_set", "event_wake", "event_wake_none",
    "wait", "wait_timed", "wait_poll",
};
static const char * const heat_timer_names[HT_NB] =
{
    "server_ms", "yield_ms", "spin_ms",
    "lock_ms", "wait_ms", "wake_ms", "alert_ms",  /* jdrly/gw2-on-mac#99 */
};
static char heat_spin_line[256];  /* the log's line for WINE_HEAT_SPIN, empty without it */

static struct heat_row *heat_rows;
static unsigned int heat_nrows;
static __thread struct heat_row *heat_self;
static mach_timebase_info_data_t heat_timebase;
static char heat_log_path[1024];
static char heat_exe[256];
static unsigned long long heat_started;

static struct heat_row *heat_row(void)
{
    unsigned int i;

    if (heat_self) return heat_self;
    if ((i = __atomic_fetch_add( &heat_nrows, 1, __ATOMIC_RELAXED )) >= HEAT_ROWS) return NULL;
    heat_self = &heat_rows[i];
    __atomic_store_n( &heat_self->start, (ULONG_PTR)get_thread_data()->start, __ATOMIC_RELAXED );
    __atomic_store_n( &heat_self->tid, get_thread_data()->tid, __ATOMIC_RELAXED );
    return heat_self;
}

/* only the row's own thread writes it, but a signal handler on that thread may count in the
 * middle of a count; the dumper reads it at any time */
static inline void heat_bump( unsigned long long *n, unsigned long long by )
{
    __atomic_fetch_add( n, by, __ATOMIC_RELAXED );
}

void heat_add( enum heat_counter c )
{
    struct heat_row *row = heat_row();
    if (row) heat_bump( &row->c[c], 1 );
}

unsigned long long heat_now(void)
{
    return mach_absolute_time() * heat_timebase.numer / heat_timebase.denom;
}

void heat_time( enum heat_counter c, enum heat_timer t, unsigned long long start )
{
    struct heat_row *row = heat_row();

    if (!row) return;
    heat_bump( &row->c[c], 1 );
    heat_bump( &row->t[t], heat_now() - start );
}

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

/***********************************************************************
 *  server requests (WINE_HEAT_COUNT=1, jdrly/gw2-on-mac#60)
 *
 * Each thread's row has a second part, in a mapping of its own: its server requests by code with
 * their time, and three splits read from the request and its reply. set_cursor by flag, and of
 * the handle requests those whose reply names the same handle as the previous one; get_message by
 * its internal flag, by whether the reply had a message, and by the message; set_queue_mask without
 * and with poll_events. Every 5 s and at exit the dumper appends a block to
 * $WINE_HEAT_LOG/heat-requests-<pid>.log for the game thread, and since jdrly/gw2-on-mac#99 one for
 * each of the two other threads with the most server time since the last dump, if they had any.
 * Each block is the change since that thread's last block:
 *
 *   requests thread 01a4 from 115.0 to 120.0 time 1762912345
 *   counters release 1000 lock_sleep 82 server_ms 4.120   the thread's counters and timers that
 *                                              changed, as in the counters file (jdrly/gw2-on-mac#99)
 *   req set_cursor 1010 21.345                 count and ms of the twelve most frequent
 *   set_cursor handle 1010 repeat 1000 count 0 pos 0 clip 0 noclip 0
 *   get_message internal 3 other 150 none 2    none: the reply had no message
 *   message 0x8001 100                         the ten most frequent messages of the replies
 *   message unlisted 0                         those past the first HEAT_MSGS kinds the thread got
 *   set_queue_mask poll 0 nopoll 100
 *
 * from and to are seconds since heat_init, time is the end in seconds since 1970. A thread's first
 * block starts at 0. The game thread is the row with the most semaphore releases, the session
 * report's rule, and its block comes first. The hook runs on the row's own thread with the server's signals blocked, so only
 * the dumper reads a row while it changes.
 */
#define HEAT_REQ_TOP 12
#define HEAT_MSG_TOP 10
#define HEAT_MSGS    32

enum heat_split
{
    HS_CURSOR_HANDLE, HS_CURSOR_REPEAT, HS_CURSOR_COUNT, HS_CURSOR_POS, HS_CURSOR_CLIP, HS_CURSOR_NOCLIP,
    HS_MSG_INTERNAL, HS_MSG_OTHER, HS_MSG_NONE, HS_MSG_UNLISTED,
    HS_POLL, HS_NOPOLL,
    HS_NB
};

struct heat_req_row
{
    unsigned long long n[REQ_NB_REQUESTS];  /* requests by code */
    unsigned long long t[REQ_NB_REQUESTS];  /* their nanoseconds */
    unsigned long long s[HS_NB];
    unsigned int       msgs;                /* kinds of message in msg[] */
    unsigned int       msg[HEAT_MSGS];
    unsigned long long msg_n[HEAT_MSGS];
    /* heat_reqs_prev only */
    double             printed;             /* the end of the thread's last block */
    unsigned long long hc[HC_NB], ht[HT_NB];  /* its heat_row at its last block (jdrly/gw2-on-mac#99) */
    unsigned long long server;              /* its HT_SERVER at the last dump */
};

#define HEAT_REQ_NAME(name) [REQ_##name] = #name,
#define HEAT_REQ_ONE(name) + 1
static const char * const heat_req_names[REQ_NB_REQUESTS] = { HEAT_REQUESTS(HEAT_REQ_NAME) };
C_ASSERT( 0 HEAT_REQUESTS(HEAT_REQ_ONE) == REQ_NB_REQUESTS );

static struct heat_req_row *heat_reqs;       /* by row, as heat_rows */
static struct heat_req_row *heat_reqs_prev;  /* the dumper's: each row at its last block */
static char heat_req_path[1024];

void heat_server_done( const union generic_request *sent, const union generic_reply *reply,
                       unsigned int status, unsigned long long start )
{
    struct heat_row *row = heat_row();
    unsigned long long d = heat_now() - start;
    unsigned int code = sent->request_header.req, flags, msg, i;
    struct heat_req_row *r;

    if (!row) return;
    heat_bump( &row->c[HC_SERVER], 1 );
    heat_bump( &row->t[HT_SERVER], d );
    if (code >= REQ_NB_REQUESTS) return;
    r = &heat_reqs[row - heat_rows];
    heat_bump( &r->n[code], 1 );
    heat_bump( &r->t[code], d );

    switch (code)
    {
    case REQ_set_cursor:
        flags = sent->set_cursor_request.flags;
        if (flags & SET_CURSOR_HANDLE)
        {
            heat_bump( &r->s[HS_CURSOR_HANDLE], 1 );
            if (!status && reply->set_cursor_reply.prev_handle == sent->set_cursor_request.handle)
                heat_bump( &r->s[HS_CURSOR_REPEAT], 1 );
        }
        if (flags & SET_CURSOR_COUNT) heat_bump( &r->s[HS_CURSOR_COUNT], 1 );
        if (flags & SET_CURSOR_POS) heat_bump( &r->s[HS_CURSOR_POS], 1 );
        if (flags & SET_CURSOR_CLIP) heat_bump( &r->s[HS_CURSOR_CLIP], 1 );
        if (flags & SET_CURSOR_NOCLIP) heat_bump( &r->s[HS_CURSOR_NOCLIP], 1 );
        break;
    case REQ_get_message:
        heat_bump( &r->s[sent->get_message_request.internal ? HS_MSG_INTERNAL : HS_MSG_OTHER], 1 );
        if (status)
        {
            heat_bump( &r->s[HS_MSG_NONE], 1 );
            break;
        }
        msg = reply->get_message_reply.msg;
        for (i = 0; i < r->msgs && r->msg[i] != msg; i++) ;
        if (i == r->msgs && i < HEAT_MSGS)
        {
            r->msg[i] = msg;
            __atomic_store_n( &r->msgs, i + 1, __ATOMIC_RELEASE );  /* after the message */
        }
        heat_bump( i < HEAT_MSGS ? &r->msg_n[i] : &r->s[HS_MSG_UNLISTED], 1 );
        break;
    case REQ_set_queue_mask:
        heat_bump( &r->s[sent->set_queue_mask_request.poll_events ? HS_POLL : HS_NOPOLL], 1 );
        break;
    }
}

/* <cur> - <prev> into <d>, and <cur> into <prev> */
static void heat_req_delta( unsigned long long *d, unsigned long long *cur, unsigned long long *prev,
                            unsigned int nb )
{
    unsigned int i;

    for (i = 0; i < nb; i++)
    {
        unsigned long long v = __atomic_load_n( &cur[i], __ATOMIC_RELAXED );
        d[i] = v - prev[i];
        prev[i] = v;
    }
}

/* the index of the largest of <v>, or <nb> if all are 0 */
static unsigned int heat_req_top( const unsigned long long *v, unsigned int nb )
{
    unsigned int i, best = nb;

    for (i = 0; i < nb; i++) if (v[i] && (best == nb || v[i] > v[best])) best = i;
    return best;
}

/* row <row>'s block into <buf>: the change since its last block; returns its length */
static int heat_req_block( char *buf, size_t size, unsigned int row, double now )
{
    unsigned long long n[REQ_NB_REQUESTS], t[REQ_NB_REQUESTS], s[HS_NB], m[HEAT_MSGS], hc[HC_NB], ht[HT_NB];
    struct heat_req_row *cur = &heat_reqs[row], *prev = &heat_reqs_prev[row];
    unsigned int i, k, msgs = __atomic_load_n( &cur->msgs, __ATOMIC_ACQUIRE );
    int len;

    heat_req_delta( n, cur->n, prev->n, REQ_NB_REQUESTS );
    heat_req_delta( t, cur->t, prev->t, REQ_NB_REQUESTS );
    heat_req_delta( s, cur->s, prev->s, HS_NB );
    heat_req_delta( m, cur->msg_n, prev->msg_n, msgs );
    heat_req_delta( hc, heat_rows[row].c, prev->hc, HC_NB );  /* jdrly/gw2-on-mac#99 */
    heat_req_delta( ht, heat_rows[row].t, prev->ht, HT_NB );

    len = snprintf( buf, size, "requests thread %04x from %.1f to %.1f time %lld\ncounters",
                    __atomic_load_n( &heat_rows[row].tid, __ATOMIC_RELAXED ), prev->printed, now,
                    (long long)time( NULL ) );
    prev->printed = now;
    for (i = 0; i < HC_NB && len < size; i++)
        if (hc[i]) len += snprintf( buf + len, size - len, " %s %llu", heat_counter_names[i], hc[i] );
    for (i = 0; i < HT_NB && len < size; i++)
        if (ht[i]) len += snprintf( buf + len, size - len, " %s %.3f", heat_timer_names[i], ht[i] / 1e6 );
    if (len < size) len += snprintf( buf + len, size - len, "\n" );
    for (i = 0; i < HEAT_REQ_TOP && len < size && (k = heat_req_top( n, REQ_NB_REQUESTS )) < REQ_NB_REQUESTS; i++)
    {
        len += snprintf( buf + len, size - len, "req %s %llu %.3f\n", heat_req_names[k], n[k], t[k] / 1e6 );
        n[k] = 0;
    }
    if (len < size)
        len += snprintf( buf + len, size - len,
                         "set_cursor handle %llu repeat %llu count %llu pos %llu clip %llu noclip %llu\n"
                         "get_message internal %llu other %llu none %llu\n",
                         s[HS_CURSOR_HANDLE], s[HS_CURSOR_REPEAT], s[HS_CURSOR_COUNT], s[HS_CURSOR_POS],
                         s[HS_CURSOR_CLIP], s[HS_CURSOR_NOCLIP], s[HS_MSG_INTERNAL], s[HS_MSG_OTHER], s[HS_MSG_NONE] );
    for (i = 0; i < HEAT_MSG_TOP && len < size && (k = heat_req_top( m, msgs )) < msgs; i++)
    {
        len += snprintf( buf + len, size - len, "message %#x %llu\n", cur->msg[k], m[k] );
        m[k] = 0;
    }
    if (len < size)
        len += snprintf( buf + len, size - len, "message unlisted %llu\nset_queue_mask poll %llu nopoll %llu\n",
                         s[HS_MSG_UNLISTED], s[HS_POLL], s[HS_NOPOLL] );
    return len < size ? len : size - 1;
}

/* Under heat_dump_mutex. The game thread's block, then those of the two other threads with the
 * most server time since the last dump (jdrly/gw2-on-mac#99), in one write. */
static void heat_req_dump(void)
{
    static char buf[3 * 8192];
    unsigned long long server, d, most[2] = {0};
    unsigned int i, game = 0, rows = __atomic_load_n( &heat_nrows, __ATOMIC_RELAXED ), busy[2];
    double now = (heat_now() - heat_started) / 1e9;
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

    len = heat_req_block( buf, sizeof(buf), game, now );
    for (i = 0; i < 2 && busy[i] < rows; i++)
        len += heat_req_block( buf + len, sizeof(buf) - len, busy[i], now );

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
 *  the wake skip (WINE_HEAT_WAKE=skip)
 *
 * A semaphore is private while only this process can wait on it: NtCreateSemaphore made it
 * here without a name, a root directory or OBJ_INHERIT, and its handle has not since been
 * closed, duplicated from any source process, or given new handle flags. heat_handles marks such
 * handles. heat_waiters counts, for each msync slot, the threads of this process asleep on it in
 * msync_wait_single(). A release of a marked semaphore whose slot counts no sleeper makes no wake
 * call. The message to the server for waiters registered there (alertable and multi-object
 * waits) is sent as before.
 *
 * The mark belongs to a handle value, and two things escape it: another process duplicating the
 * handle out of this one (DuplicateHandle called elsewhere with this process as the source), and
 * a release that looked up its object through a handle that another thread then closed and
 * reused for a new private semaphore. So the release also reads the object's own reference count
 * in msync's shared memory, which the server raises (msync_grab_object, seq_cst) for each
 * process that looks the object up through a handle, before that process can wait on it. A count
 * above 2 (the server's own reference and this process's) means another process may wait, or
 * this process holds the object through more than one handle, and the release wakes as msync
 * does.
 *
 * Why no wake is lost, on arm64. Before 0.2 the unix side ran under Rosetta and had x86's
 * ordering. Here it is native code, and arm64 may make a store visible after a later load
 * of another address. Both sides write one word and then read the other's:
 *
 *   waiter (heat_sleep_on, then ulock_wait)    releaser (msync_release_semaphore_obj, signal_all)
 *   add 1 to heat_waiters[obj]                 change the semaphore's count (CAS)
 *   full barrier                               full barrier
 *   the kernel compares the count with 0,      read heat_waiters[obj], the handle's mark and the
 *   and sleeps only if it is still 0           object's reference count
 *
 * With a full barrier (__atomic_thread_fence( __ATOMIC_SEQ_CST ), dmb ish) between the write
 * and the read on each side, at least one side sees the other's write: if the releaser reads
 * no sleeper, the waiter's compare comes after the new count and it doesn't sleep. This must
 * not rest on what the CAS or an atomic add orders for a later plain load, nor on what entering
 * the kernel orders: each side has its own barrier. One barrier a release is nothing next to the
 * system call it removes. The mark and the reference count follow the same pattern. Clearing a
 * mark (heat_handle_shared) is followed by a barrier before the duplicate or close goes to the
 * server. Another process's reference is added by the server before its reply, and that process
 * compares the count only after the reply. Either way, a releaser that still reads the old value
 * changed the count before the other side could compare it.
 */
#define HEAT_HANDLES (1 << 22)  /* handle values up to 16M */
#define HEAT_SLOTS   (1 << 22)  /* msync slots up to 4M */
#define HEAT_H_PRIVATE 1        /* a private semaphore (WINE_HEAT_WAKE=skip) */
#define HEAT_H_EVENT   2        /* made by NtCreateEvent (WINE_HEAT_COUNT=1) */

static unsigned char *heat_handles;
static int *heat_waiters;
__thread HANDLE heat_releasing;  /* the handle NtReleaseSemaphore is releasing, while it does */

static inline unsigned int heat_handle_index( HANDLE handle )
{
    return (ULONG)(ULONG_PTR)handle >> 2;
}

void heat_mark( HANDLE handle, unsigned char flag )
{
    unsigned int i = heat_handle_index( handle );
    if (i < HEAT_HANDLES) __atomic_fetch_or( &heat_handles[i], flag, __ATOMIC_RELEASE );
}

void heat_unmark( HANDLE handle, unsigned char flags )
{
    unsigned int i = heat_handle_index( handle );
    unsigned char old;

    if (i >= HEAT_HANDLES || !(__atomic_load_n( &heat_handles[i], __ATOMIC_RELAXED ) & flags)) return;
    old = __atomic_fetch_and( &heat_handles[i], ~flags, __ATOMIC_RELAXED );
    if ((old & flags & HEAT_H_EVENT) && (heat_mode & HEAT_COUNT)) heat_add( HC_EVENT_CLOSE );
    if (old & flags & HEAT_H_PRIVATE) __atomic_thread_fence( __ATOMIC_SEQ_CST );
}

void heat_sleep_on( unsigned int obj )
{
    if (obj >= HEAT_SLOTS) return;
    __atomic_fetch_add( &heat_waiters[obj], 1, __ATOMIC_RELAXED );
    __atomic_thread_fence( __ATOMIC_SEQ_CST );  /* before the kernel compares the count */
}

void heat_woke_on( unsigned int obj )
{
    if (obj < HEAT_SLOTS) __atomic_fetch_sub( &heat_waiters[obj], 1, __ATOMIC_RELAXED );
}

BOOL heat_skip_wake_slow( unsigned int obj, const unsigned short *refcount )
{
    unsigned int i = heat_handle_index( heat_releasing );

    if (i >= HEAT_HANDLES || obj >= HEAT_SLOTS) return FALSE;
    __atomic_thread_fence( __ATOMIC_SEQ_CST );  /* after the count changed */
    if (!(__atomic_load_n( &heat_handles[i], __ATOMIC_RELAXED ) & HEAT_H_PRIVATE)) return FALSE;
    if (__atomic_load_n( refcount, __ATOMIC_RELAXED ) > 2) return FALSE;
    if (__atomic_load_n( &heat_waiters[obj], __ATOMIC_RELAXED )) return FALSE;
    heat_count( HC_WAKE_SKIPPED );
    return TRUE;
}

/***********************************************************************
 *  the event pool (WINE_HEAT_EVPOOL)
 *
 * An event is eligible when NtCreateEvent made it here with EVENT_ALL_ACCESS and without a name,
 * a root directory, a security descriptor or OBJ_INHERIT: no other process can have it, and this
 * one holds it through that handle only. NtClose of an eligible handle parks it in the pool of
 * its kind (auto-reset or manual-reset) instead of closing it, and NtCreateEvent of that kind
 * takes the oldest handle that has been parked for at least 2 ms, then sets or resets it. The
 * handle stays open at the server the whole time, so its value can't go to another object, and
 * its cached in-process sync (sync.c) stays too: neither call makes a server request.
 *
 * The handle's byte in heat_handles carries three bits for this:
 *   HEAT_H_POOL    eligible, and this process's code holds it
 *   HEAT_H_MANUAL  a manual-reset event
 *   HEAT_H_PARKED  in a pool: closed, as far as this process's code knows
 *
 * A handle stops being eligible, and its next close is a real one, once it was duplicated (with
 * any source process handle), given new handle flags, pulsed (an auto-reset event can keep a
 * pulse nobody took, msync.c), or handed to the server, which may signal it later: an
 * asynchronous I/O's event (server_async), a registry notification's, IOCTL_AFD_EVENT_SELECT's.
 * heat_evpool_forget() clears HEAT_H_POOL for that without the pool's mutex, since a duplicate
 * can come from a system APC in a signal handler. A parked handle that loses HEAT_H_POOL stays
 * parked and is closed for real when its turn comes.
 *
 * A close or a duplicate (from this process) of a parked handle returns STATUS_INVALID_HANDLE,
 * as Windows does for a closed handle, and never reaches the server: a parked handle must stay
 * open until the pool closes it. NtCreateEvent keeps HEAT_H_PARKED while it takes a handle out
 * and resets it, and clears it only if HEAT_H_POOL is still set; otherwise it closes the handle
 * for real. The pool's own closes (heat_pool_close) never take the pool path, and clear the
 * handle's bits before and after the server closes it. A second close of that value by this
 * process's code between the two is a real one, and then the pool's close may hit a new object
 * that got the value meanwhile: the same as two closes of one handle without the pool, and only
 * after code closed a handle it had already closed. The 2 ms give a thread that still uses a closed handle time to
 * finish; a parked handle that a thread of this process still waits on or signals in-process when
 * its turn comes is closed for real (heat_inproc_sync_idle). What can't be seen from here:
 * another process that duplicated the handle out of this one without DUPLICATE_CLOSE_SOURCE
 * (with it, the close runs here as a system APC), and a wait through the server that outlives
 * the 2 ms.
 *
 * The guard (jdrly/gw2-on-mac#6). A signal handler may run any of this on a thread that holds
 * the pool's mutex. Instead of blocking signals around the mutex, two system calls each time, a
 * thread sets its heat_pool_busy before it takes the mutex and clears it after it lets go. Code
 * that finds its own thread's flag set doesn't take the mutex and goes to the server: a real
 * create or a real close. The mutex is only tried, never waited for, and a busy one sends the
 * call to the server too: with signals open, the holder can be suspended (SuspendThread is a
 * signal) or killed inside, and a thread that waited for it could wait for its own suspender.
 * WINE_HEAT_EVPOOL=1,mask uses Wine's uninterrupted section instead, as 0.1.x did.
 */
#define HEAT_H_POOL    4
#define HEAT_H_MANUAL  8
#define HEAT_H_PARKED  16
#define HEAT_H_EVPOOL  (HEAT_H_POOL | HEAT_H_MANUAL | HEAT_H_PARKED)

#define HEAT_POOL_SIZE   1024
#define HEAT_POOL_AGE_NS 2000000

struct heat_pool
{
    HANDLE             h[HEAT_POOL_SIZE];
    unsigned long long t[HEAT_POOL_SIZE];  /* when it was parked, heat_now() */
    unsigned int       head, count;
};

static struct heat_pool *heat_pools;  /* [0] auto-reset, [1] manual-reset */
static pthread_mutex_t heat_pool_mutex = PTHREAD_MUTEX_INITIALIZER;
static __thread int heat_pool_busy;
static __thread HANDLE heat_pool_closing;  /* the handle heat_pool_close() is closing */

static BOOL heat_pool_lock( sigset_t *sigset )
{
    if (heat_mode & HEAT_EVPOOL_MASK)
    {
        server_enter_uninterrupted_section( &heat_pool_mutex, sigset );
        return TRUE;
    }
    /* a signal handler on a thread that is in here, or about to be */
    if (__atomic_load_n( &heat_pool_busy, __ATOMIC_RELAXED )) return FALSE;
    __atomic_store_n( &heat_pool_busy, 1, __ATOMIC_RELAXED );
    __atomic_signal_fence( __ATOMIC_SEQ_CST );
    if (!pthread_mutex_trylock( &heat_pool_mutex )) return TRUE;
    __atomic_signal_fence( __ATOMIC_SEQ_CST );
    __atomic_store_n( &heat_pool_busy, 0, __ATOMIC_RELAXED );
    return FALSE;
}

static void heat_pool_unlock( sigset_t *sigset )
{
    if (heat_mode & HEAT_EVPOOL_MASK)
    {
        server_leave_uninterrupted_section( &heat_pool_mutex, sigset );
        return;
    }
    pthread_mutex_unlock( &heat_pool_mutex );
    __atomic_signal_fence( __ATOMIC_SEQ_CST );
    __atomic_store_n( &heat_pool_busy, 0, __ATOMIC_RELAXED );
}

static BOOL heat_evpool_eligible( ACCESS_MASK access, const OBJECT_ATTRIBUTES *attr )
{
    if (access != EVENT_ALL_ACCESS) return FALSE;
    return !attr || (!attr->RootDirectory && (!attr->ObjectName || !attr->ObjectName->Length) &&
                     !attr->SecurityDescriptor && !(attr->Attributes & OBJ_INHERIT));
}

/* a handle the pool took out of use, closed for real and never parked again */
static void heat_pool_close( HANDLE handle )
{
    unsigned char *mark = &heat_handles[heat_handle_index( handle )];

    __atomic_fetch_and( mark, ~HEAT_H_EVPOOL, __ATOMIC_ACQ_REL );
    heat_pool_closing = handle;
    NtClose( handle );
    heat_pool_closing = 0;
    /* an eligible event that got the value after a stale close in between would leave its bits
     * on a value the server has freed */
    __atomic_fetch_and( mark, ~HEAT_H_EVPOOL, __ATOMIC_ACQ_REL );
}

static NTSTATUS heat_evpool_closed_twice(void)
{
    heat_count( HC_EVPOOL_DOUBLE_CLOSE );
    return STATUS_INVALID_HANDLE;
}

BOOL heat_evpool_get_slow( HANDLE *handle, ACCESS_MASK access, const OBJECT_ATTRIBUTES *attr,
                           EVENT_TYPE type, BOOLEAN state )
{
    struct heat_pool *pool = &heat_pools[type == NotificationEvent];
    unsigned char *mark, flags;
    HANDLE h = 0;
    sigset_t sigset;

    if (!heat_evpool_eligible( access, attr ) || !heat_pool_lock( &sigset )) return FALSE;
    if (pool->count && heat_now() - pool->t[pool->head] >= HEAT_POOL_AGE_NS)
    {
        h = pool->h[pool->head];
        pool->head = (pool->head + 1) % HEAT_POOL_SIZE;
        pool->count--;
    }
    heat_pool_unlock( &sigset );
    if (!h) return FALSE;

    /* Out of the pool but still HEAT_H_PARKED, so a stale close or duplicate meanwhile gets
     * STATUS_INVALID_HANDLE and the handle stays open. Only a forget can change the byte now:
     * it clears HEAT_H_POOL, and then the handle isn't handed out. */
    mark = &heat_handles[heat_handle_index( h )];
    flags = __atomic_load_n( mark, __ATOMIC_ACQUIRE );
    if (!heat_inproc_sync_idle( h )) heat_count( HC_EVPOOL_BUSY );
    else if ((flags & HEAT_H_POOL) && !(state ? NtSetEvent( h, NULL ) : NtResetEvent( h, NULL )))
    {
        while (flags & HEAT_H_POOL)
        {
            if (!__atomic_compare_exchange_n( mark, &flags, flags & ~HEAT_H_PARKED, FALSE,
                                              __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE ))
                continue;
            *handle = h;
            heat_count( HC_EVPOOL_HIT );
            return TRUE;
        }
    }
    /* forgotten while parked, or still in use: close it for real */
    heat_pool_close( h );
    return FALSE;
}

void heat_evpool_created_slow( HANDLE handle, ACCESS_MASK access, const OBJECT_ATTRIBUTES *attr, EVENT_TYPE type )
{
    unsigned int i = heat_handle_index( handle );
    unsigned char flags, want = HEAT_H_POOL | (type == NotificationEvent ? HEAT_H_MANUAL : 0);

    if (i >= HEAT_HANDLES || !heat_evpool_eligible( access, attr )) return;
    /* a new value: only a HEAT_H_MANUAL of an earlier object can be left */
    flags = __atomic_load_n( &heat_handles[i], __ATOMIC_RELAXED );
    while (!__atomic_compare_exchange_n( &heat_handles[i], &flags, (flags & ~HEAT_H_EVPOOL) | want, FALSE,
                                         __ATOMIC_RELEASE, __ATOMIC_RELAXED ))
        ;
}

NTSTATUS heat_evpool_close_slow( HANDLE handle )
{
    unsigned int i = heat_handle_index( handle ), slot;
    struct heat_pool *pool;
    unsigned char flags;
    HANDLE evict = 0;
    sigset_t sigset;

    if (i >= HEAT_HANDLES || handle == heat_pool_closing) return STATUS_PENDING;
    flags = __atomic_load_n( &heat_handles[i], __ATOMIC_ACQUIRE );
    if (flags & HEAT_H_PARKED) return heat_evpool_closed_twice();
    if (!(flags & HEAT_H_POOL)) return STATUS_PENDING;
    if (!heat_pool_lock( &sigset )) return heat_evpool_take_slow( handle );

    /* park it, unless a forget or another close of the same handle came first */
    while ((flags & (HEAT_H_POOL | HEAT_H_PARKED)) == HEAT_H_POOL &&
           !__atomic_compare_exchange_n( &heat_handles[i], &flags, flags | HEAT_H_PARKED, FALSE,
                                         __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE ))
        ;
    if ((flags & (HEAT_H_POOL | HEAT_H_PARKED)) != HEAT_H_POOL)
    {
        heat_pool_unlock( &sigset );
        return (flags & HEAT_H_PARKED) ? heat_evpool_closed_twice() : STATUS_PENDING;
    }
    pool = &heat_pools[!!(flags & HEAT_H_MANUAL)];
    if (pool->count == HEAT_POOL_SIZE)
    {
        evict = pool->h[pool->head];
        pool->head = (pool->head + 1) % HEAT_POOL_SIZE;
        pool->count--;
    }
    slot = (pool->head + pool->count) % HEAT_POOL_SIZE;
    pool->h[slot] = handle;
    pool->t[slot] = heat_now();
    pool->count++;
    heat_pool_unlock( &sigset );

    if (evict)
    {
        heat_count( HC_EVPOOL_EVICTED );
        heat_pool_close( evict );
    }
    heat_count( HC_EVPOOL_PUT );
    return STATUS_SUCCESS;
}

/* the handle is about to be closed for real, or duplicated from this process: no longer
 * eligible, or STATUS_INVALID_HANDLE if it is parked */
NTSTATUS heat_evpool_take_slow( HANDLE handle )
{
    unsigned int i = heat_handle_index( handle );
    unsigned char flags;

    if (i >= HEAT_HANDLES) return STATUS_PENDING;
    flags = __atomic_load_n( &heat_handles[i], __ATOMIC_ACQUIRE );
    do
    {
        if (flags & HEAT_H_PARKED) return heat_evpool_closed_twice();
        if (!(flags & HEAT_H_POOL)) return STATUS_PENDING;
    } while (!__atomic_compare_exchange_n( &heat_handles[i], &flags, flags & ~HEAT_H_POOL, FALSE,
                                           __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE ));
    heat_count( HC_EVPOOL_FORGOTTEN );
    return STATUS_PENDING;
}

void heat_evpool_forget_slow( HANDLE handle )
{
    unsigned int i = heat_handle_index( handle );

    if (i < HEAT_HANDLES && (__atomic_fetch_and( &heat_handles[i], ~HEAT_H_POOL, __ATOMIC_ACQ_REL ) & HEAT_H_POOL))
        heat_count( HC_EVPOOL_FORGOTTEN );
}

/***********************************************************************
 *  the spins (WINE_HEAT_SPIN, jdrly/gw2-on-mac#45)
 *
 * Three kinds of sleep may spin first, each up to its own time, read from the clock every 16
 * turns:
 *
 *   lock   NtWaitForAlertByThreadId (contended critical sections, SRW locks, condition variables,
 *          WaitOnAddress), outside the x86 emulator only, so that FEX's own waits keep athei's
 *          path (sync.c)
 *   event  a wait-any whose objects are all events, until one is signaled or pulsed or the alert
 *          object is set (msync.c)
 *   sem    a single, non-alertable wait on a semaphore, until its count is above 0 (msync.c).
 *          The thread joins WINE_HEAT_WAKE=skip's count of sleepers only when it stops spinning,
 *          so a release during the spin makes no wake call.
 *
 * A wait spins once, before its first sleep, and only when it has to sleep. Per thread and kind,
 * after 4 misses in a row the thread doesn't spin for its next WINE_HEAT_SPIN_PAUSE waits of that
 * kind (spin_off); a hit resets the count. A kind's scope leaves out the threads whose priority is
 * one it names (spin_scoped): a thread's priority is the base priority it last set for itself
 * through NtSetInformationThread (heat_priority), 0 until it does.
 *
 * The lock's protocol. A thread's alert word (sync.c) holds the sources of its pending alerts as
 * bits 1 and 2. NtAlertThreadByThreadId ORs its bit in and makes the wake call only when the word
 * was 0. A spinner sets a third bit, HEAT_ALERT_SPIN, by compare-and-swap from 0, spins while the
 * word is exactly that bit, and then exchanges the word with 0: alert bits in what it took are the
 * alerts that came, and NtWaitForAlertByThreadId returns STATUS_ALERTED; without any it sleeps as
 * before, on a word that is 0 again. An alerter that found the bit made no wake call, and its bit
 * is in the word until the spinner's exchange takes it. One that comes after the exchange finds 0
 * and wakes the sleeper as before. Every access to the word is an atomic read-modify-write on that
 * one word (the spin's plain loads only decide when to stop), so the order of the two sides'
 * accesses is the word's own order, and no other memory is part of the protocol: arm64's weaker
 * ordering has nothing to reorder here. The exchange is seq_cst, an acquire of what the alerter
 * wrote before its OR, as the sleeper's exchange is.
 *
 * Taking the alerts at sync.c's sleeper defers the program's alert when the wait is inside the
 * emulator; the spin never runs there, so that rule has nothing to do. A wait inside the emulator
 * on this thread's word can only start in a signal handler that interrupts the spin. It finds the
 * bit, takes it with whatever alerts are in the word and returns, as after any spurious wakeup;
 * the spin then sees the word changed, takes what is left, and sleeps if that is no alert. With
 * WINE_HEAT_SPIN unset nothing here runs and the bit is never written.
 *
 * A pulse during an event spin. msync takes back an auto-reset event's pulse when its wake call
 * found no thread asleep (msync_pulse_event_obj), so a spinner that saw the pulse could find it
 * gone when it tries to take it, and sleep: a lost pulse. So a spinner registers on each of its
 * events' slots in heat_spinners for the length of the spin, and a pulse made while a spinner was
 * registered keeps its pulse. The spinner takes it inside the spin when it sees the pulse word
 * change, or after the spin in its sleep's own check of the pulse word.
 *
 * A slot's word holds the number of spinners in its low half and, in its high half, how many
 * spins ever registered there. The pulser reads the word before its CAS on the pulse word and
 * again after its wake call, and keeps the pulse when a spinner was registered at the first read
 * or the second read differs from the first. Every access on both sides is seq_cst, so they fall
 * in one order. If the pulse's CAS comes between a spinner's registration and its end: either the
 * registration came before the first read, which then sees the spinner, since its end comes after
 * the CAS and so after that read; or it came after the first read, and the second read, after the
 * CAS, sees the new count of registrations. A spin that ends by its budget therefore can't let a
 * pulse made during it be taken back, whether its end or the pulser's second read comes first.
 * The spinner puts a full barrier after it registers, before it reads a pulse word, and after it
 * ends, before its sleep reads one, so that it sees every pulse that came before those points.
 *
 * What stays open is msync's own: a pulse made after the wait read its pulse word but before the
 * spin registered, or after the spin ended but before the thread sleeps, is taken back as it is
 * from msync's waiter in the same place. A pulse kept for a spinner that then returns through
 * another object, or for one registered after the pulse, stays untaken, as when a woken waiter
 * of a wait-any takes another object.
 */
#define HEAT_ALERT_SPIN 4
#define HEAT_SPIN_MISSES 4
#define HEAT_PRIORITY_BIT(p) (1u << ((p) + 15))  /* base priorities -15 to 15 */

static unsigned long long heat_spin_ns[HEAT_SPIN_KINDS];  /* the most a spin takes, 0 for off */
static unsigned int heat_spin_not[HEAT_SPIN_KINDS];       /* priorities left out, HEAT_PRIORITY_BIT */
static unsigned int heat_spin_pause = 32;
static const char * const heat_spin_names[HEAT_SPIN_KINDS] = { "lock", "event", "sem" };
__thread LONG heat_priority;
/* per msync slot: the spinners of this process on it, and in the high half its registrations */
static unsigned long long *heat_spinners;
#define HEAT_SPINNERS_ONE  0x100000001ull  /* one more spinner, one more registration */
#define HEAT_SPINNERS_MASK 0xffffffffull
static __thread unsigned int heat_spin_misses[HEAT_SPIN_KINDS];  /* this thread's misses in a row */
static __thread unsigned int heat_spin_skip[HEAT_SPIN_KINDS];    /* its waits left without a spin */

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

/* how long this thread may spin before this sleep (ns), 0 for not at all */
unsigned long long heat_spin_budget( enum heat_spin_kind kind )
{
    LONG prio = heat_priority;

    if (prio >= -15 && prio <= 15 && (heat_spin_not[kind] & HEAT_PRIORITY_BIT( prio )))
    {
        heat_count( HC_SPIN_SCOPED );
        return 0;
    }
    if (heat_spin_skip[kind])
    {
        heat_spin_skip[kind]--;
        heat_count( HC_SPIN_OFF );
        return 0;
    }
    return heat_spin_ns[kind];
}

void heat_spin_done( enum heat_spin_kind kind, BOOL hit, unsigned long long start )
{
    static const enum heat_counter hits[HEAT_SPIN_KINDS] = { HC_LSPIN_HIT, HC_ESPIN_HIT, HC_SSPIN_HIT };

    if (heat_mode & HEAT_COUNT) heat_time( hit ? hits[kind] : hits[kind] + 1, HT_SPIN, start );
    if (hit) heat_spin_misses[kind] = 0;
    else if (++heat_spin_misses[kind] >= HEAT_SPIN_MISSES)
    {
        heat_spin_misses[kind] = 0;
        heat_spin_skip[kind] = heat_spin_pause;
    }
}

/* an event spin on <objs> begins (TRUE) or ends (FALSE); FALSE when it may not begin */
BOOL heat_spin_register( const int *objs, int count, BOOL begin )
{
    int i;

    for (i = 0; i < count; i++) if ((unsigned int)objs[i] >= HEAT_SLOTS) return FALSE;
    for (i = 0; i < count; i++)
    {
        if (begin) __atomic_fetch_add( &heat_spinners[objs[i]], HEAT_SPINNERS_ONE, __ATOMIC_SEQ_CST );
        else __atomic_fetch_sub( &heat_spinners[objs[i]], 1, __ATOMIC_SEQ_CST );
    }
    /* after registering, before reading a pulse word; after ending, before the sleep reads one */
    __atomic_thread_fence( __ATOMIC_SEQ_CST );
    return TRUE;
}

/* msync_pulse_event_obj, before it changes the pulse word: the slot's word to hand to
 * heat_pulse_spinners_slow() */
unsigned long long heat_pulse_begin_slow( unsigned int obj )
{
    return obj < HEAT_SLOTS ? __atomic_load_n( &heat_spinners[obj], __ATOMIC_SEQ_CST ) : 0;
}

/* after the pulse and its wake call: TRUE when a spinner was registered on <obj> at <before>, or
 * one registered since */
BOOL heat_pulse_spinners_slow( unsigned int obj, unsigned long long before )
{
    if (obj >= HEAT_SLOTS) return FALSE;
    __atomic_thread_fence( __ATOMIC_SEQ_CST );
    return (before & HEAT_SPINNERS_MASK) || __atomic_load_n( &heat_spinners[obj], __ATOMIC_SEQ_CST ) != before;
}

BOOL heat_spin_lock_slow( LONG *futex, const ULONGLONG *end )
{
    unsigned long long budget = heat_spin_budget( HEAT_SPIN_KIND_LOCK ), start;
    LONG expected = 0, alerts;
    LARGE_INTEGER now;
    unsigned int turn;

    if (!budget) return FALSE;
    if (end)
    {
        NtQuerySystemTime( &now );
        if ((LONGLONG)(*end - now.QuadPart) <= 0) return FALSE;
        if (*end - now.QuadPart < budget / 100) budget = (*end - now.QuadPart) * 100;
    }
    if (!__atomic_compare_exchange_n( futex, &expected, HEAT_ALERT_SPIN, FALSE, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST ))
        return FALSE;  /* an alert came: the sleeper's loop takes it */
    start = heat_now();
    for (turn = 1; __atomic_load_n( futex, __ATOMIC_RELAXED ) == HEAT_ALERT_SPIN; turn++)
    {
        heat_pause();
        if (!(turn & 15) && heat_now() - start >= budget) break;
    }
    alerts = __atomic_exchange_n( futex, 0, __ATOMIC_SEQ_CST ) & ~HEAT_ALERT_SPIN;
    heat_spin_done( HEAT_SPIN_KIND_LOCK, alerts != 0, start );
    return alerts != 0;
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
    /* jdrly/gw2-on-mac#44 */
    if ((p = getenv( "WINE_HEAT_MSYNC_UNREG" )) && !strcmp( p, "1" )) mode |= HEAT_MSYNC_UNREG;
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
    }
    if (mode & (HEAT_COUNT | HEAT_PROF))
    {
        mach_timebase_info( &heat_timebase );
        heat_started = heat_now();
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
