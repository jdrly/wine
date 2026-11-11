/*
 * GW2 on Mac: counters and the semaphore wake policy for one process (WINE_HEAT_*), heat.c
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

#ifndef __NTDLL_UNIX_HEAT_H
#define __NTDLL_UNIX_HEAT_H

#define HEAT_COUNT     0x1  /* WINE_HEAT_COUNT=1 */
#define HEAT_WAKE_ONE  0x2  /* WINE_HEAT_WAKE=one */
#define HEAT_WAKE_SKIP 0x4  /* WINE_HEAT_WAKE=skip */

enum heat_counter
{
    HC_RELEASE,      /* semaphore releases */
    HC_WAKE_SKIPPED, /* releases of a private semaphore with no waiter here: no wake call */
    HC_WAKE_ONE,     /* wake-one calls of WINE_HEAT_WAKE=one that woke a thread */
    HC_WAKE_NONE,    /* wake calls that found no waiter */
    HC_WOKEN,        /* single waits woken */
    HC_SPURIOUS,     /* ... that found nothing to take and slept again */
    HC_EVENT_CREATE, /* NtCreateEvent */
    HC_CLOSE,        /* NtClose, of any object */
    HC_SERVER,       /* server calls, with HT_SERVER */
    HC_LOCK_SLEEP,   /* sleeps in NtWaitForAlertByThreadId: contended locks, WaitOnAddress */
    HC_QPC,          /* NtQueryPerformanceCounter */
    HC_YIELD,        /* NtYieldExecution, with HT_YIELD and by how long the yield took: */
    HC_YIELD_5US, HC_YIELD_20US, HC_YIELD_100US, HC_YIELD_1MS, HC_YIELD_LONG,
    HC_NB
};

enum heat_timer { HT_SERVER, HT_YIELD, HT_NB };

/* Decided once per process by heat_init(), before any PE code runs, and never changed. In every
 * process that isn't WINE_HEAT_EXE it stays 0, and each probe below is that one load and a
 * branch. */
extern unsigned int heat_mode;

extern void heat_init(void);
extern void heat_add( enum heat_counter c );
extern unsigned long long heat_now(void);
extern void heat_time( enum heat_counter c, enum heat_timer t, unsigned long long start );
extern void heat_yield_done( unsigned long long start );
extern void heat_private_semaphore( HANDLE handle );
extern void heat_forget_handle( HANDLE handle );
extern void heat_sleep_on( unsigned int obj );
extern void heat_woke_on( unsigned int obj );
extern BOOL heat_skip_wake_slow( unsigned int obj );
extern __thread HANDLE heat_releasing;

static inline void heat_count( enum heat_counter c )
{
    if (heat_mode & HEAT_COUNT) heat_add( c );
}

/* the start of a timed call; no clock read unless counting */
static inline unsigned long long heat_start(void)
{
    return (heat_mode & HEAT_COUNT) ? heat_now() : 0;
}

/* NtCreateSemaphore made it: private if unnamed, without a root directory and not inheritable */
static inline void heat_semaphore_created( HANDLE handle, const OBJECT_ATTRIBUTES *attr )
{
    if (!(heat_mode & HEAT_WAKE_SKIP)) return;
    if (attr && (attr->RootDirectory || (attr->ObjectName && attr->ObjectName->Length) ||
                 (attr->Attributes & OBJ_INHERIT)))
        return;
    heat_private_semaphore( handle );
}

/* closed, duplicated or given new handle flags: no longer private */
static inline void heat_handle_shared( HANDLE handle )
{
    if (heat_mode & HEAT_WAKE_SKIP) heat_forget_handle( handle );
}

/* signal_all, after the count changed: TRUE when the release may make no wake call */
static inline BOOL heat_skip_wake( unsigned int obj )
{
    return (heat_mode & HEAT_WAKE_SKIP) && heat_releasing && heat_skip_wake_slow( obj );
}

#endif /* __NTDLL_UNIX_HEAT_H */
