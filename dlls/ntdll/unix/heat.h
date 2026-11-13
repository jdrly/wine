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

/* The bits of heat_mode, by number in one enum: a change that adds a switch adds its line at the
 * end, so two changes that each add one conflict in git instead of both taking the same value, as
 * two #defines of 0x10 once did. */
enum heat_mode_bit
{
    HEAT_BIT_COUNT,        /* 0x1    WINE_HEAT_COUNT=1 */
    HEAT_BIT_WAKE_ONE,     /* 0x2    WINE_HEAT_WAKE=one */
    HEAT_BIT_WAKE_SKIP,    /* 0x4    WINE_HEAT_WAKE=skip */
    HEAT_BIT_UNUSED,       /* 0x8    never used */
    HEAT_BIT_MSYNC_UNREG,  /* 0x10   WINE_HEAT_MSYNC_UNREG=1, jdrly/gw2-on-mac#44 */
    HEAT_BIT_EVPOOL,       /* 0x20   WINE_HEAT_EVPOOL=1, jdrly/gw2-on-mac#42 */
    HEAT_BIT_EVPOOL_MASK,  /* 0x40   WINE_HEAT_EVPOOL=1,mask, jdrly/gw2-on-mac#42 */
    HEAT_BIT_SPIN_LOCK,    /* 0x80   WINE_HEAT_SPIN=lock=<us>, jdrly/gw2-on-mac#45 */
    HEAT_BIT_SPIN_EVENT,   /* 0x100  WINE_HEAT_SPIN=event=<us> */
    HEAT_BIT_SPIN_SEM,     /* 0x200  WINE_HEAT_SPIN=sem=<us> */
    HEAT_BIT_PROF,         /* 0x400  WINE_HEAT_PROF=1, jdrly/gw2-on-mac#43 */
    HEAT_BITS
};
C_ASSERT( HEAT_BITS <= 32 );  /* heat_mode is an unsigned int */

#define HEAT_COUNT       (1u << HEAT_BIT_COUNT)
#define HEAT_WAKE_ONE    (1u << HEAT_BIT_WAKE_ONE)
#define HEAT_WAKE_SKIP   (1u << HEAT_BIT_WAKE_SKIP)
#define HEAT_MSYNC_UNREG (1u << HEAT_BIT_MSYNC_UNREG)

/* jdrly/gw2-on-mac#42 */
#define HEAT_EVPOOL      (1u << HEAT_BIT_EVPOOL)
#define HEAT_EVPOOL_MASK (1u << HEAT_BIT_EVPOOL_MASK)

/* jdrly/gw2-on-mac#45 */
#define HEAT_SPIN_LOCK  (1u << HEAT_BIT_SPIN_LOCK)
#define HEAT_SPIN_EVENT (1u << HEAT_BIT_SPIN_EVENT)
#define HEAT_SPIN_SEM   (1u << HEAT_BIT_SPIN_SEM)
#define HEAT_SPIN_ANY   (HEAT_SPIN_LOCK | HEAT_SPIN_EVENT | HEAT_SPIN_SEM)

/* jdrly/gw2-on-mac#43 */
#define HEAT_PROF       (1u << HEAT_BIT_PROF)  /* heatprof.c */

enum heat_counter
{
    HC_RELEASE,      /* semaphore releases */
    HC_WAKE_SKIPPED, /* releases of a private semaphore with no waiter here: no wake call */
    HC_WAKE_ONE,     /* wake-one calls of WINE_HEAT_WAKE=one that woke a thread */
    HC_WAKE_NONE,    /* wake calls on a semaphore or mutex that found no waiter (events: HC_EVENT_WAKE_NONE) */
    HC_WOKEN,        /* single waits woken */
    HC_SPURIOUS,     /* ... that found nothing to take and slept again */
    HC_EVENT_CREATE, /* NtCreateEvent */
    HC_EVENT_CLOSE,  /* NtClose of a handle NtCreateEvent made */
    HC_CLOSE,        /* NtClose, of any object */
    HC_SERVER,       /* server calls, with HT_SERVER */
    HC_LOCK_SLEEP,   /* sleeps in NtWaitForAlertByThreadId: contended locks, WaitOnAddress */
    HC_QPC,          /* NtQueryPerformanceCounter */
    HC_YIELD,        /* NtYieldExecution, with HT_YIELD and by how long the yield took: */
    HC_YIELD_5US, HC_YIELD_20US, HC_YIELD_100US, HC_YIELD_1MS, HC_YIELD_LONG,
    /* jdrly/gw2-on-mac#44 */
    HC_UNREG,        /* multiple-object waits that unregistered after finding an object signaled */
    /* WINE_HEAT_EVPOOL */
    HC_EVPOOL_HIT,          /* NtCreateEvent served by the pool */
    HC_EVPOOL_PUT,          /* NtClose that parked the handle */
    HC_EVPOOL_EVICTED,      /* parked handles closed for real because the pool was full */
    HC_EVPOOL_FORGOTTEN,    /* handles that stopped being eligible: duplicated, handed to the server... */
    HC_EVPOOL_BUSY,         /* parked handles a thread still used when their turn came: closed for real */
    HC_EVPOOL_DOUBLE_CLOSE, /* a close or a duplicate of a parked handle: STATUS_INVALID_HANDLE */
    /* jdrly/gw2-on-mac#45, a spin ends in a hit when what it waits for came while it spun */
    HC_LSPIN_HIT, HC_LSPIN_MISS,  /* WINE_HEAT_SPIN=lock */
    HC_ESPIN_HIT, HC_ESPIN_MISS,  /* WINE_HEAT_SPIN=event */
    HC_SSPIN_HIT, HC_SSPIN_MISS,  /* WINE_HEAT_SPIN=sem */
    HC_SPIN_OFF,     /* waits that didn't spin: the thread's spins of that kind missed 4 times in a row */
    HC_SPIN_SCOPED,  /* waits that didn't spin: the scope leaves out the thread's priority */
    /* jdrly/gw2-on-mac#99 */
    HC_WAIT_SLEEP,   /* sleeps in msync_wait_single and msync_wait_multiple, with HT_WAIT */
    HC_ALERT,        /* NtAlertThreadByThreadId */
    HC_ALERT_WAKE,   /* ... that made the wake call, with HT_ALERT */
    HC_LOCK_5US, HC_LOCK_20US, HC_LOCK_100US, HC_LOCK_1MS, HC_LOCK_LONG,  /* lock sleeps by length */
    HC_WAIT_5US, HC_WAIT_20US, HC_WAIT_100US, HC_WAIT_1MS, HC_WAIT_LONG,  /* wait sleeps by length */
    HC_EVENT_SET,       /* msync_set_event_obj */
    HC_EVENT_WAKE,      /* wake calls on an event, a set's or a pulse's, that woke a thread */
    HC_EVENT_WAKE_NONE, /* ... that found none */
    HC_WAIT,         /* msync_wait_objs */
    HC_WAIT_TIMED,   /* ... with a timeout that is neither infinite nor zero */
    HC_WAIT_POLL,    /* ... with a zero timeout */
    HC_NB
};

enum heat_timer
{
    HT_SERVER, HT_YIELD,
    HT_SPIN,  /* jdrly/gw2-on-mac#45 */
    HT_LOCK, HT_WAIT, HT_WAKE, HT_ALERT,  /* jdrly/gw2-on-mac#99 */
    HT_NB
};

/* Decided once per process by heat_init(), before any PE code runs, and never changed. In every
 * process that isn't WINE_HEAT_EXE it stays 0, and each probe below is that one load and a
 * branch. */
extern unsigned int heat_mode;

extern void heat_init(void);
extern void heat_add( enum heat_counter c );
extern unsigned long long heat_now(void);
extern void heat_time( enum heat_counter c, enum heat_timer t, unsigned long long start );
extern void heat_yield_done( unsigned long long start );
/* jdrly/gw2-on-mac#99 */
extern void heat_lock_slept( unsigned long long start );
extern void heat_wait_slept( unsigned long long start );
extern void heat_wake_done( BOOL event, BOOL found, unsigned long long start );
extern void heat_add_time( enum heat_timer t, unsigned long long start );
extern void heat_wait_counted( const LARGE_INTEGER *timeout );
extern void heat_named( const char *name );
extern void heat_mark( HANDLE handle, unsigned char flag );
extern void heat_unmark( HANDLE handle, unsigned char flags );
extern void heat_sleep_on( unsigned int obj );
extern void heat_woke_on( unsigned int obj );
extern BOOL heat_skip_wake_slow( unsigned int obj, const unsigned short *refcount );
extern __thread HANDLE heat_releasing;

/* jdrly/gw2-on-mac#60: a server call ended, <status> from the reply. Counts it as HC_SERVER with
 * HT_SERVER, and by its request code. <sent> is the request as it was before the reply overwrote it. */
union generic_request;
union generic_reply;
extern void heat_server_done( const union generic_request *sent, const union generic_reply *reply,
                              unsigned int status, unsigned long long start );

static inline void heat_count( enum heat_counter c )
{
    if (heat_mode & HEAT_COUNT) heat_add( c );
}

/* the start of a timed call; no clock read unless counting */
static inline unsigned long long heat_start(void)
{
    return (heat_mode & HEAT_COUNT) ? heat_now() : 0;
}

/* msync_wait_objs, on entry, with the caller's timeout (jdrly/gw2-on-mac#99) */
static inline void heat_wait_begun( const LARGE_INTEGER *timeout )
{
    if (heat_mode & HEAT_COUNT) heat_wait_counted( timeout );
}

/* set_native_thread_name named this thread <name> (jdrly/gw2-on-mac#99) */
static inline void heat_thread_named( const char *name )
{
    if (heat_mode & HEAT_COUNT) heat_named( name );
}

/* NtCreateSemaphore made it: private if unnamed, without a root directory and not inheritable */
static inline void heat_semaphore_created( HANDLE handle, const OBJECT_ATTRIBUTES *attr )
{
    if (!(heat_mode & HEAT_WAKE_SKIP)) return;
    if (attr && (attr->RootDirectory || (attr->ObjectName && attr->ObjectName->Length) ||
                 (attr->Attributes & OBJ_INHERIT)))
        return;
    heat_mark( handle, 1 /* HEAT_H_PRIVATE */ );
}

static inline void heat_event_created( HANDLE handle )
{
    if (!(heat_mode & HEAT_COUNT)) return;
    heat_add( HC_EVENT_CREATE );
    heat_mark( handle, 2 /* HEAT_H_EVENT */ );
}

/* duplicated or given new handle flags: a semaphore is no longer private */
static inline void heat_handle_shared( HANDLE handle )
{
    if (heat_mode & HEAT_WAKE_SKIP) heat_unmark( handle, 1 );
}

/* before the server closes it, and can give its value to a new object */
static inline void heat_handle_closed( HANDLE handle )
{
    if (!heat_mode) return;
    heat_count( HC_CLOSE );
    heat_unmark( handle, 1 | 2 );
}

/* signal_all, after the count changed: TRUE when the release may make no wake call. <refcount>
 * is the semaphore's reference count in msync's shared memory. */
static inline BOOL heat_skip_wake( unsigned int obj, const unsigned short *refcount )
{
    return (heat_mode & HEAT_WAKE_SKIP) && heat_releasing && heat_skip_wake_slow( obj, refcount );
}

/***********************************************************************
 *  the event pool (WINE_HEAT_EVPOOL), heat.c has the rules
 */
extern BOOL heat_evpool_get_slow( HANDLE *handle, ACCESS_MASK access, const OBJECT_ATTRIBUTES *attr,
                                  EVENT_TYPE type, BOOLEAN state );
extern void heat_evpool_created_slow( HANDLE handle, ACCESS_MASK access, const OBJECT_ATTRIBUTES *attr,
                                      EVENT_TYPE type );
extern NTSTATUS heat_evpool_close_slow( HANDLE handle );
extern NTSTATUS heat_evpool_take_slow( HANDLE handle );
extern void heat_evpool_forget_slow( HANDLE handle );
extern BOOL heat_inproc_sync_idle( HANDLE handle );  /* sync.c */

/* NtCreateEvent, before the server: TRUE when a parked handle of that kind now is *handle */
static inline BOOL heat_evpool_get( HANDLE *handle, ACCESS_MASK access, const OBJECT_ATTRIBUTES *attr,
                                    EVENT_TYPE type, BOOLEAN state )
{
    return (heat_mode & HEAT_EVPOOL) && heat_evpool_get_slow( handle, access, attr, type, state );
}

/* NtCreateEvent, after the server made the event */
static inline void heat_evpool_created( HANDLE handle, ACCESS_MASK access, const OBJECT_ATTRIBUTES *attr,
                                        EVENT_TYPE type )
{
    if (heat_mode & HEAT_EVPOOL) heat_evpool_created_slow( handle, access, attr, type );
}

/* NtClose: STATUS_PENDING to close it for real, otherwise what NtClose returns */
static inline NTSTATUS heat_evpool_close( HANDLE handle )
{
    return (heat_mode & HEAT_EVPOOL) ? heat_evpool_close_slow( handle ) : STATUS_PENDING;
}

/* NtDuplicateObject: STATUS_PENDING to go on, STATUS_INVALID_HANDLE for a parked handle of this
 * process */
static inline NTSTATUS heat_evpool_dup( HANDLE process, HANDLE source )
{
    if (!(heat_mode & HEAT_EVPOOL)) return STATUS_PENDING;
    if (process == NtCurrentProcess()) return heat_evpool_take_slow( source );
    /* a real handle, maybe to this process: forgetting another process's value costs nothing */
    heat_evpool_forget_slow( source );
    return STATUS_PENDING;
}

/* the handle was given new flags, pulsed, or handed to the server, which may signal it later */
static inline void heat_evpool_forget( HANDLE handle )
{
    if ((heat_mode & HEAT_EVPOOL) && handle) heat_evpool_forget_slow( handle );
}

/***********************************************************************
 *  the spins (WINE_HEAT_SPIN, jdrly/gw2-on-mac#45), heat.c has the rules
 */
enum heat_spin_kind { HEAT_SPIN_KIND_LOCK, HEAT_SPIN_KIND_EVENT, HEAT_SPIN_KIND_SEM, HEAT_SPIN_KINDS };
/* a kind's switch is HEAT_SPIN_LOCK << kind */
C_ASSERT( HEAT_BIT_SPIN_EVENT == HEAT_BIT_SPIN_LOCK + HEAT_SPIN_KIND_EVENT );
C_ASSERT( HEAT_BIT_SPIN_SEM == HEAT_BIT_SPIN_LOCK + HEAT_SPIN_KIND_SEM );

extern __thread LONG heat_priority;
extern unsigned long long heat_spin_budget( enum heat_spin_kind kind );
extern void heat_spin_done( enum heat_spin_kind kind, BOOL hit, unsigned long long start );
extern BOOL heat_spin_lock_slow( LONG *futex, const ULONGLONG *end );
extern BOOL heat_spin_register( const int *objs, int count, BOOL begin );
extern unsigned long long heat_pulse_begin_slow( unsigned int obj );
extern BOOL heat_pulse_spinners_slow( unsigned int obj, unsigned long long before );

/* msync_pulse_event_obj, before it changes the pulse word */
static inline unsigned long long heat_pulse_begin( unsigned int obj )
{
    return (heat_mode & HEAT_SPIN_EVENT) ? heat_pulse_begin_slow( obj ) : 0;
}

/* msync_pulse_event_obj, after the wake call found no sleeper: TRUE to keep the pulse for a
 * thread that was spinning on the event while it pulsed. <before> is heat_pulse_begin()'s. */
static inline BOOL heat_pulse_spinners( unsigned int obj, unsigned long long before )
{
    return (heat_mode & HEAT_SPIN_EVENT) && heat_pulse_spinners_slow( obj, before );
}

/* NtSetInformationThread: the base priority this thread just set for itself */
static inline void heat_thread_priority( LONG priority )
{
    if (heat_mode & HEAT_SPIN_ANY) heat_priority = priority;
}

/* NtWaitForAlertByThreadId, before its first sleep: TRUE when an alert came during the spin and
 * was taken. <end> is the wait's deadline in system time, NULL for none. */
static inline BOOL heat_spin_lock( LONG *futex, const ULONGLONG *end )
{
    return (heat_mode & HEAT_SPIN_LOCK) && !__atomic_load_n( futex, __ATOMIC_RELAXED ) &&
           heat_spin_lock_slow( futex, end );
}

/* one turn of a spin: a bare yield on arm64, without YieldProcessor()'s store barrier */
static inline void heat_pause(void)
{
#ifdef __aarch64__
    __asm__ __volatile__( "yield" ::: "memory" );
#else
    YieldProcessor();
#endif
}

#endif /* __NTDLL_UNIX_HEAT_H */
