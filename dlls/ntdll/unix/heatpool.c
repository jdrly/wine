/*
 * GW2 on Mac: the event pool (WINE_HEAT_EVPOOL, jdrly/gw2-on-mac#42)
 *
 *   WINE_HEAT_EVPOOL=1    NtClose of an event that only this process can hold parks the handle,
 *                         and NtCreateEvent takes one that has been parked for 2 ms: neither asks
 *                         the server (jdrly/gw2-on-mac#42). "1,mask" blocks signals around the
 *                         pool's mutex, as 0.1.x did, instead of the per-thread flag.
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
#include "heat_private.h"

/* heat.h's, which heat_init() sets. It is defined here and not in heat.c, since clang moves
 * heat_evpool_close_slow()'s store of the time a handle was parked past its load of heat_mode
 * only where it sees the definition, and with it the size (jdrly/gw2-on-mac#106). */
unsigned int heat_mode;

#define HEAT_POOL_AGE_NS 2000000

struct heat_pool *heat_pools;  /* [0] auto-reset, [1] manual-reset */
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
