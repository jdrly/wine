/*
 * GW2 on Mac: what heat.c, heatreq.c, heatwake.c, heatpool.c and heatspin.c share
 *
 * They were one file, heat.c, until jdrly/gw2-on-mac#106, and each function's code is what it was
 * there but two, both on the dumper's path. Two things keep it so:
 *
 * - A variable that more than one of them uses is declared here with hidden visibility. A file
 *   that reached a variable defined in another without it would load the address from the GOT
 *   first, where a static took one adrp and one add.
 * - clang inlined heat.c's functions into each other at -O2. heat_row() and heat_bump() are static
 *   inline here, and heat_add(), heat_now() and heat_time() GNU extern inline: heat.c defines
 *   HEAT_INLINE empty before it includes this file, so it has their one copy that other files
 *   call, and every file here gets the same bodies to inline. heat_spin_parse() and
 *   heat_spin_describe() stay in heat.c with heat_init(), and heat_req_dump() with the dumper:
 *   each has that one caller, which inlines it.
 *
 * The two are heat_dump_locked() and heat_req_block(). Every file counts through heat_row(), so
 * heat_rows can't be static any more, and clang no longer assumes that a store through another
 * pointer leaves it alone: heat_req_block() loads it once more, and heat_dump_locked() has other
 * registers. Dropping the static from heat_rows in the one-file heat.c changes the same two.
 *
 * heat.h stays the one header of these that the rest of ntdll includes.
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

#ifndef __NTDLL_UNIX_HEAT_PRIVATE_H
#define __NTDLL_UNIX_HEAT_PRIVATE_H

#include <mach/mach_time.h>
#include "heatfex.h"

#ifndef HEAT_INLINE
#define HEAT_INLINE extern inline __attribute__((gnu_inline))
#endif
#define HEAT_HIDDEN __attribute__((visibility("hidden")))

extern unsigned int heat_mode HEAT_HIDDEN;

/***********************************************************************
 *  the counters' rows (heat.c has the rules)
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

extern struct heat_row *heat_rows HEAT_HIDDEN;
extern unsigned int heat_nrows HEAT_HIDDEN;
extern __thread struct heat_row *heat_self HEAT_HIDDEN;
extern mach_timebase_info_data_t heat_timebase HEAT_HIDDEN;

static inline struct heat_row *heat_row(void)
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

HEAT_INLINE void heat_add( enum heat_counter c )
{
    struct heat_row *row = heat_row();
    if (row) heat_bump( &row->c[c], 1 );
}

HEAT_INLINE unsigned long long heat_now(void)
{
    return mach_absolute_time() * heat_timebase.numer / heat_timebase.denom;
}

HEAT_INLINE void heat_time( enum heat_counter c, enum heat_timer t, unsigned long long start )
{
    struct heat_row *row = heat_row();

    if (!row) return;
    heat_bump( &row->c[c], 1 );
    heat_bump( &row->t[t], heat_now() - start );
}

/***********************************************************************
 *  the rows' second part, the server requests (heatreq.c has the rules)
 */
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
    unsigned long long fex[HEAT_FEX_FIELDS];  /* its FEX counters at its last line (jdrly/gw2-on-mac#111) */
};

extern struct heat_req_row *heat_reqs HEAT_HIDDEN;
extern struct heat_req_row *heat_reqs_prev HEAT_HIDDEN;
extern int heat_req_block( char *buf, size_t size, unsigned int row, double now,
                           const unsigned char *fex, size_t fex_size ) HEAT_HIDDEN;

/***********************************************************************
 *  the handle marks: the wake skip's and the event counter's (heatwake.c has the rules), and the
 *  event pool's (heatpool.c)
 */
#define HEAT_HANDLES (1 << 22)  /* handle values up to 16M */
#define HEAT_SLOTS   (1 << 22)  /* msync slots up to 4M */
#define HEAT_H_PRIVATE 1        /* a private semaphore (WINE_HEAT_WAKE=skip) */
#define HEAT_H_EVENT   2        /* made by NtCreateEvent (WINE_HEAT_COUNT=1) */
#define HEAT_H_POOL    4
#define HEAT_H_MANUAL  8
#define HEAT_H_PARKED  16
#define HEAT_H_EVPOOL  (HEAT_H_POOL | HEAT_H_MANUAL | HEAT_H_PARKED)

extern unsigned char *heat_handles HEAT_HIDDEN;
extern int *heat_waiters HEAT_HIDDEN;

static inline unsigned int heat_handle_index( HANDLE handle )
{
    return (ULONG)(ULONG_PTR)handle >> 2;
}

/***********************************************************************
 *  the event pool (heatpool.c has the rules)
 */
#define HEAT_POOL_SIZE   1024

struct heat_pool
{
    HANDLE             h[HEAT_POOL_SIZE];
    unsigned long long t[HEAT_POOL_SIZE];  /* when it was parked, heat_now() */
    unsigned int       head, count;
};

extern struct heat_pool *heat_pools HEAT_HIDDEN;

/***********************************************************************
 *  the spins (heatspin.c has the rules; heat.c parses WINE_HEAT_SPIN)
 */
#define HEAT_PRIORITY_BIT(p) (1u << ((p) + 15))  /* base priorities -15 to 15 */

extern unsigned long long heat_spin_ns[HEAT_SPIN_KINDS] HEAT_HIDDEN;
extern unsigned int heat_spin_not[HEAT_SPIN_KINDS] HEAT_HIDDEN;
extern unsigned int heat_spin_pause HEAT_HIDDEN;
extern unsigned long long *heat_spinners HEAT_HIDDEN;

#endif /* __NTDLL_UNIX_HEAT_PRIVATE_H */
