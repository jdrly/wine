/*
 * GW2 on Mac: the wake skip (WINE_HEAT_WAKE=skip)
 *
 *   WINE_HEAT_WAKE=one    a semaphore release of count n makes at most n wake-one calls and
 *                         stops at the first that finds no waiter. msync wakes every waiter on
 *                         every release. Events and mutexes keep that (msync.c).
 *   WINE_HEAT_WAKE=skip   a release of a semaphore that only this process has looked up, with
 *                         no thread of it asleep on it, makes no wake call. "one,skip" for both.
 *
 * A release makes no wake call when two things hold after the count changed: the object's
 * reference count in msync's shared memory is at most 2 (the server's own reference and this
 * process's), and heat_waiters, which counts by msync slot the threads of this process asleep in
 * msync_wait_single(), counts none on its slot. The message to the server for waiters registered
 * there (alertable and multi-object waits) is sent as before. Up to jdrly/gw2-on-mac#249 the
 * release also needed a mark on its handle, which NtCreateSemaphore set and a close or a
 * duplicate cleared.
 *
 * Why the count says enough:
 * - A thread of this process that sleeps on a semaphore does so in msync_wait_single(), which
 *   counts it in heat_waiters by slot, whatever handle it used.
 * - An alertable or multi-object wait registers with the server and raises multiple_waiters,
 *   which signal_all() reads after the skip decision and always honours.
 * - A thread of another process reaches the slot only through get_inproc_sync_fd on a handle of
 *   its own, and the server raises the count (msync_grab_object) before it replies. That holds
 *   for a name opened there, a handle duplicated in either direction, and inheritance.
 * - A process's reference lasts as long as one of its threads uses the object, even after the
 *   handle was closed: a wait and a release each hold a reference on the cache entry
 *   (inproc_wait, inproc_release_semaphore), and release_inproc_sync() sends msync_close only
 *   when that drops to 0.
 * - The server drops references on its message thread, late. That only over-counts, which costs
 *   a wake call and never loses one.
 * A second handle of this process to the semaphore takes the count to 3 on its first lookup, so
 * that semaphore stops skipping too. wake_shared counts the releases refused by the count.
 *
 * What the count doesn't see. The server drops its own reference when the object's last handle
 * anywhere is closed. Take this state: no handle to the semaphore is left in any process, a
 * thread of another process is still asleep on it, and a thread of mine is still inside a
 * release or a SignalObjectAndWait on it, holding its cache entry. The count reads 2, mine and
 * the sleeper's process's, my release makes no wake call, and the other process's thread sleeps
 * on. The lost wake hits that other process (Steam, the overlay, CefHost.exe), never a thread of
 * this one: heat_waiters counts my own sleepers. On Windows the same close against the same
 * release already loses that wake when the close wins, and the waiter got there by waiting on a
 * handle that was closed, which is undefined. I accept it (jdrly/gw2-on-mac#249). The mark on
 * the handle covered most of it until then. The wait-all's put-back in msync_wait_objs() has no
 * Windows counterpart and holds its entries for the whole wait, so it passes 0 to signal_all()
 * and never asks the skip.
 *
 * Why no wake is lost, on arm64. Before 0.2 the unix side ran under Rosetta and had x86's
 * ordering. Here it is native code, and arm64 may make a store visible after a later load
 * of another address. Both sides write one word and then read the other's:
 *
 *   waiter (heat_sleep_on, then ulock_wait)    releaser (msync_release_semaphore_obj, signal_all)
 *   add 1 to heat_waiters[obj]                 change the semaphore's count (CAS)
 *   full barrier                               full barrier
 *   the kernel compares the count with 0,      read heat_waiters[obj] and the object's
 *   and sleeps only if it is still 0           reference count
 *
 * With a full barrier (__atomic_thread_fence( __ATOMIC_SEQ_CST ), dmb ish) between the write
 * and the read on each side, at least one side sees the other's write: if the releaser reads
 * no sleeper, the waiter's compare comes after the new count and it doesn't sleep. This must
 * not rest on what the CAS or an atomic add orders for a later plain load, nor on what entering
 * the kernel orders: each side has its own barrier. One barrier a release is nothing next to the
 * system call it removes. The reference count follows the same pattern. Another process's
 * reference is added by the server before its reply, and that process compares the count only
 * after the reply. A releaser that still reads the old value changed the count before the other
 * side could compare it.
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
    if (obj >= HEAT_SLOTS) return FALSE;
    __atomic_thread_fence( __ATOMIC_SEQ_CST );  /* after the count changed */
    if (__atomic_load_n( refcount, __ATOMIC_RELAXED ) > 2)
    {
        heat_count( HC_WAKE_SHARED );
        return FALSE;
    }
    if (__atomic_load_n( &heat_waiters[obj], __ATOMIC_RELAXED )) return FALSE;
    heat_count( HC_WAKE_SKIPPED );
    return TRUE;
}
