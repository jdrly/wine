/*
 * GW2 on Mac: the server requests (WINE_HEAT_COUNT=1, jdrly/gw2-on-mac#60)
 *
 *   WINE_HEAT_COUNT=1     per-thread counters (enum heat_counter), which a thread of their own
 *                         adds up and writes to $WINE_HEAT_LOG/heat-<pid>.log (default /tmp)
 *                         every 5 seconds and at exit. No clock is read while it is off.
 *                         The server requests of the game thread and two more go to
 *                         heat-requests-<pid>.log (jdrly/gw2-on-mac#60).
 *                         With FEX_PROFILESTATS=1 each of those blocks has its thread's FEX
 *                         counters (jdrly/gw2-on-mac#111).
 *
 * heat.c has the rows and the dumper: heat_req_dump() calls heat_req_block() for each block.
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
 *   fex jit_ms 812.375 jit 20411 miss 5120 ... dcache_ms 0.000   with FEX_PROFILESTATS=1, the
 *                                              thread's FEX counters (jdrly/gw2-on-mac#111): the
 *                                              change since its last block's line, or "fex has no
 *                                              slot of <n>", or "fex stats version <v> slot <bytes>"
 *                                              for a stats object this file doesn't read (heatfex.h)
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
#include "heatreq.h"
#include "heat_private.h"

#define HEAT_REQ_TOP 12
#define HEAT_MSG_TOP 10

#define HEAT_REQ_NAME(name) [REQ_##name] = #name,
#define HEAT_REQ_ONE(name) + 1
static const char * const heat_req_names[REQ_NB_REQUESTS] = { HEAT_REQUESTS(HEAT_REQ_NAME) };
C_ASSERT( 0 HEAT_REQUESTS(HEAT_REQ_ONE) == REQ_NB_REQUESTS );

struct heat_req_row *heat_reqs;       /* by row, as heat_rows */
struct heat_req_row *heat_reqs_prev;  /* the dumper's: each row at its last block */

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

/* row <row>'s block into <buf>: the change since its last block, with the fex line when <fex> is
 * FEX's stats object (<fex_size> bytes); returns its length */
int heat_req_block( char *buf, size_t size, unsigned int row, double now,
                    const unsigned char *fex, size_t fex_size )
{
    DWORD tid = __atomic_load_n( &heat_rows[row].tid, __ATOMIC_RELAXED );
    unsigned long long n[REQ_NB_REQUESTS], t[REQ_NB_REQUESTS], s[HS_NB], m[HEAT_MSGS], hc[HC_NB], ht[HT_NB];
    struct heat_req_row *cur = &heat_reqs[row], *prev = &heat_reqs_prev[row];
    unsigned int i, k, msgs = __atomic_load_n( &cur->msgs, __ATOMIC_ACQUIRE );
    struct heat_fex_clock clock;
    int len;

    heat_req_delta( n, cur->n, prev->n, REQ_NB_REQUESTS );
    heat_req_delta( t, cur->t, prev->t, REQ_NB_REQUESTS );
    heat_req_delta( s, cur->s, prev->s, HS_NB );
    heat_req_delta( m, cur->msg_n, prev->msg_n, msgs );
    heat_req_delta( hc, heat_rows[row].c, prev->hc, HC_NB );  /* jdrly/gw2-on-mac#99 */
    heat_req_delta( ht, heat_rows[row].t, prev->ht, HT_NB );

    len = snprintf( buf, size, "requests thread %04x from %.1f to %.1f time %lld\n", tid, prev->printed, now,
                    (long long)time( NULL ) );
    prev->printed = now;
    if (fex && len < size)
    {
        heat_fex_clock_read( &clock );
        len += heat_fex_line( buf + len, size - len, fex, fex_size, tid, prev->fex,
                              heat_fex_ms( &heat_fex_started, &clock, (double)heat_timebase.numer / heat_timebase.denom / 1e6 ) );
    }
    if (len < size) len += snprintf( buf + len, size - len, "counters" );
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
