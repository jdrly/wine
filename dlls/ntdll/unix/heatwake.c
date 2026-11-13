/*
 * GW2 on Mac: the wake skip (WINE_HEAT_WAKE=skip)
 *
 *   WINE_HEAT_WAKE=one    a semaphore release of count n makes at most n wake-one calls and
 *                         stops at the first that finds no waiter. msync wakes every waiter on
 *                         every release. Events and mutexes keep that (msync.c).
 *   WINE_HEAT_WAKE=skip   a release of a private semaphore with no thread of this process
 *                         asleep on it makes no wake call. "one,skip" for both.
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

unsigned char *heat_handles;
int *heat_waiters;
__thread HANDLE heat_releasing;  /* the handle NtReleaseSemaphore is releasing, while it does */

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
