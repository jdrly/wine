/*
 * GW2 on Mac: the spins (WINE_HEAT_SPIN, jdrly/gw2-on-mac#45)
 *
 *   WINE_HEAT_SPIN=lock=<us>,event=<us>,sem=<us>  spin up to that long (1 to 1000 us, each kind
 *                         optional) before a lock sleep, a sleep on events only, or a single
 *                         wait's sleep on a semaphore (jdrly/gw2-on-mac#45).
 *                         A kind may end in a scope: ":not=<p>" leaves out the threads whose
 *                         priority is p, and repeats; ":game" is ":not=1", GW2's ABOVE_NORMAL
 *                         workers; ":all", the default, leaves out none.
 *   WINE_HEAT_SPIN_PAUSE=<n>  waits without a spin after 4 misses in a row (default 32)
 *
 * heat.c parses both, in heat_spin_parse() and heat_init().
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

#define HEAT_ALERT_SPIN 4
#define HEAT_SPIN_MISSES 4

unsigned long long heat_spin_ns[HEAT_SPIN_KINDS];  /* the most a spin takes, 0 for off */
unsigned int heat_spin_not[HEAT_SPIN_KINDS];       /* priorities left out, HEAT_PRIORITY_BIT */
unsigned int heat_spin_pause = 32;
__thread LONG heat_priority;
/* per msync slot: the spinners of this process on it, and in the high half its registrations */
unsigned long long *heat_spinners;
#define HEAT_SPINNERS_ONE  0x100000001ull  /* one more spinner, one more registration */
#define HEAT_SPINNERS_MASK 0xffffffffull
static __thread unsigned int heat_spin_misses[HEAT_SPIN_KINDS];  /* this thread's misses in a row */
static __thread unsigned int heat_spin_skip[HEAT_SPIN_KINDS];    /* its waits left without a spin */

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
