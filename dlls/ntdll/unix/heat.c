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
 *   WINE_HEAT_WAKE=one    a semaphore release of count n makes at most n wake-one calls and
 *                         stops at the first that finds no waiter. msync wakes every waiter on
 *                         every release. Events and mutexes keep that.
 *   WINE_HEAT_WAKE=skip   a release of a private semaphore with no thread of this process
 *                         asleep on it makes no wake call. "one,skip" for both.
 *
 * See jdrly/gw2-on-mac#40 and, for 0.1.x's version of the same switches, gw2-on-mac's
 * docs/research/heat-locks-2026-10-29.md.
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
#include <unistd.h>
#include <mach/mach_time.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winternl.h"
#include "unix_private.h"
#include "heat.h"

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
};

static const char * const heat_counter_names[HC_NB] =
{
    "release", "wake_skipped", "wake_one", "wake_none", "woken", "spurious", "event_create",
    "event_close", "close", "server", "lock_sleep", "qpc", "yield", "yield_5us", "yield_20us", "yield_100us",
    "yield_1ms", "yield_long",
};
static const char * const heat_timer_names[HT_NB] = { "server_ms", "yield_ms" };

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

void heat_yield_done( unsigned long long start )
{
    struct heat_row *row = heat_row();
    unsigned long long d = heat_now() - start;
    enum heat_counter bucket;

    if (!row) return;
    if (d < 5000) bucket = HC_YIELD_5US;
    else if (d < 20000) bucket = HC_YIELD_20US;
    else if (d < 100000) bucket = HC_YIELD_100US;
    else if (d < 1000000) bucket = HC_YIELD_1MS;
    else bucket = HC_YIELD_LONG;
    heat_bump( &row->c[HC_YIELD], 1 );
    heat_bump( &row->c[bucket], 1 );
    heat_bump( &row->t[HT_YIELD], d );
}

static int heat_print_row( char *buf, size_t size, const unsigned long long *c, const unsigned long long *t )
{
    int i, len = 0;

    for (i = 0; i < HC_NB && len < size; i++)
        len += snprintf( buf + len, size - len, " %s %llu", heat_counter_names[i], c[i] );
    for (i = 0; i < HT_NB && len < size; i++)
        len += snprintf( buf + len, size - len, " %s %.3f", heat_timer_names[i], t[i] / 1e6 );
    if (len < size) len += snprintf( buf + len, size - len, "\n" );
    return len < size ? len : size - 1;
}

/* the whole file again: a header, the totals, then one line per thread */
static void heat_dump(void)
{
    static char buf[1 << 20];
    unsigned long long total_c[HC_NB] = {0}, total_t[HT_NB] = {0}, c[HC_NB], t[HT_NB];
    unsigned int i, j, n = __atomic_load_n( &heat_nrows, __ATOMIC_RELAXED );
    int len, fd;

    if (n > HEAT_ROWS) n = HEAT_ROWS;
    for (i = 0; i < n; i++)
    {
        for (j = 0; j < HC_NB; j++) total_c[j] += __atomic_load_n( &heat_rows[i].c[j], __ATOMIC_RELAXED );
        for (j = 0; j < HT_NB; j++) total_t[j] += __atomic_load_n( &heat_rows[i].t[j], __ATOMIC_RELAXED );
    }
    len = snprintf( buf, sizeof(buf), "heat pid %d exe %s mode%s%s%s seconds %.1f threads %u\ntotal",
                    getpid(), heat_exe, heat_mode & HEAT_COUNT ? " count" : "",
                    heat_mode & HEAT_WAKE_ONE ? " one" : "", heat_mode & HEAT_WAKE_SKIP ? " skip" : "",
                    (heat_now() - heat_started) / 1e9, n );
    len += heat_print_row( buf + len, sizeof(buf) - len, total_c, total_t );
    for (i = 0; i < n && len < sizeof(buf) - 1; i++)
    {
        for (j = 0; j < HC_NB; j++) c[j] = __atomic_load_n( &heat_rows[i].c[j], __ATOMIC_RELAXED );
        for (j = 0; j < HT_NB; j++) t[j] = __atomic_load_n( &heat_rows[i].t[j], __ATOMIC_RELAXED );
        len += snprintf( buf + len, sizeof(buf) - len, "thread %04x",
                         __atomic_load_n( &heat_rows[i].tid, __ATOMIC_RELAXED ) );
        if (len < sizeof(buf) - 1) len += heat_print_row( buf + len, sizeof(buf) - len, c, t );
    }
    if (len > sizeof(buf) - 1) len = sizeof(buf) - 1;
    if ((fd = open( heat_log_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644 )) == -1) return;
    write( fd, buf, len );
    close( fd );
}

static pthread_mutex_t heat_dump_mutex = PTHREAD_MUTEX_INITIALIZER;

static void heat_dump_locked(void)
{
    pthread_mutex_lock( &heat_dump_mutex );
    heat_dump();
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
    if (!mode) return;
    if (!exe || !*exe) exe = "Gw2-64.exe";
    if (!peb || !peb->ProcessParameters || !heat_is_exe( exe )) return;

    if (!(heat_handles = heat_map( HEAT_HANDLES ))) return;
    if ((mode & HEAT_WAKE_SKIP) && !(heat_waiters = heat_map( HEAT_SLOTS * sizeof(int) )))
        mode &= ~HEAT_WAKE_SKIP;
    if ((mode & HEAT_COUNT) && !(heat_rows = heat_map( HEAT_ROWS * sizeof(*heat_rows) )))
        mode &= ~HEAT_COUNT;
    if (mode & HEAT_COUNT)
    {
        mach_timebase_info( &heat_timebase );
        heat_started = heat_now();
        snprintf( heat_exe, sizeof(heat_exe), "%s", exe );
        snprintf( heat_log_path, sizeof(heat_log_path), "%s/heat-%d.log", dir && *dir ? dir : "/tmp", getpid() );
    }
    heat_mode = mode;
    if (mode & HEAT_COUNT)
    {
        /* the dumper isn't a Wine thread: no signal of Wine's may land on it */
        sigfillset( &all );
        pthread_sigmask( SIG_SETMASK, &all, &old );
        if (!pthread_create( &thread, NULL, heat_dumper, NULL )) pthread_detach( thread );
        pthread_sigmask( SIG_SETMASK, &old, NULL );
        atexit( heat_dump_locked );
    }
}
