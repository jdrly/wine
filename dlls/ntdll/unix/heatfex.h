/*
 * GW2 on Mac: FEX's counters (FEX_PROFILESTATS=1) as heatreq.c and heatprof.c read them
 *
 * No Wine header and nothing of the process: tests/gw2/native/fexstats.c builds it natively and
 * feeds it stats objects of its own.
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

#ifndef __NTDLL_UNIX_HEATFEX_H
#define __NTDLL_UNIX_HEATFEX_H

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* FEX_PROFILESTATS=1: the shared memory object fex-<pid>-stats
 * (Source/Windows/UnixLib/FEXUnixLib.cpp:154-157), a ThreadStatsHeader and then ThreadStats slots
 * (FEXCore/include/FEXCore/Utils/SHMStats.h:45-78). A slot with TID 0 is free. Times are in
 * CNTVCT_EL0 ticks, the counter mach_absolute_time() reads. My FEX fork asserts each of these
 * against FEX's own types (Source/Windows/Common/GW2ContractStats.h). The object's Size grows as
 * slots are given out, so the walk to Size reads only the part in use.
 *
 * Two versions, and FEX only appends to a slot (SHMStats.h:45), so the reader takes a slot at least
 * as large as its version's and reads the fields it knows by offset:
 *   2  the engine's FEX: 112-byte slots, twelve fields
 *   3  my fork since jdrly/gw2-on-mac#117: 144-byte slots, the twelve and four more
 * Another version, or a smaller slot, it doesn't read at all. */
#define HEAT_FEX_STATS_VERSION   2
#define HEAT_FEX_STATS_VERSION_3 3
#define HEAT_FEX_STATS_HEADER    64   /* sizeof(ThreadStatsHeader) */
#define HEAT_FEX_STATS_SLOT      112  /* sizeof(ThreadStats) at 9d4fa4b46b1, also ThreadStatsHeader::ThreadStatsSize */
#define HEAT_FEX_STATS_H_VERSION 0    /* uint8_t Version */
#define HEAT_FEX_STATS_H_SLOT    2    /* uint16_t ThreadStatsSize */
#define HEAT_FEX_STATS_H_SIZE    56   /* uint32_t Size: the bytes in use */
#define HEAT_FEX_STATS_TID       4    /* uint32_t TID: the Windows thread id */
#define HEAT_FEX_STATS_JIT_TIME  8    /* AccumulatedJITTime: compiling */
#define HEAT_FEX_STATS_SIG_TIME  16   /* AccumulatedSignalTime */
#define HEAT_FEX_STATS_SIGBUS    24   /* AccumulatedSIGBUSCount */
#define HEAT_FEX_STATS_SMC       32   /* AccumulatedSMCCount: writes to pages with translated code */
#define HEAT_FEX_STATS_FLOAT     40   /* AccumulatedFloatFallbackCount */
#define HEAT_FEX_STATS_MISS      48   /* AccumulatedCacheMissCount: lookup cache misses */
#define HEAT_FEX_STATS_RLOCK     56   /* AccumulatedCacheReadLockTime */
#define HEAT_FEX_STATS_WLOCK     64   /* AccumulatedCacheWriteLockTime */
#define HEAT_FEX_STATS_JIT_COUNT 72   /* AccumulatedJITCount: blocks compiled */
#define HEAT_FEX_STATS_DC_HIT    80   /* AccumulatedDiskCacheHitCount */
#define HEAT_FEX_STATS_DC_MISS   88   /* AccumulatedDiskCacheMissCount */
#define HEAT_FEX_STATS_DC_TIME   96   /* AccumulatedDiskCacheLookupTime */
/* version 3 (jdrly/gw2-on-mac#117): four counts after Padding (at 104), in a slot of 144 bytes */
#define HEAT_FEX_STATS_SLOT_3    144
#define HEAT_FEX_STATS_TSO       112  /* hardware TSO switches */
#define HEAT_FEX_STATS_EXITS     120  /* exits into native code */
#define HEAT_FEX_STATS_CODEBUF   128  /* moves to a new code buffer */
#define HEAT_FEX_STATS_L1        136  /* L1 resizes */

/* The fields of the requests log's fex line (jdrly/gw2-on-mac#111), in its order. The first nine
 * are the sampler's fex thread line in the heat log, names and order the same; the last four only
 * in version 3. */
#define HEAT_FEX_FIELDS     16
#define HEAT_FEX_FIELDS_112 12
#define HEAT_FEX_FIELDS_OLD 9

struct heat_fex_field
{
    const char    *name;
    unsigned short at;  /* in the slot */
    unsigned short ms;  /* a time in ticks, printed in ms */
};

static const struct heat_fex_field heat_fex_fields[HEAT_FEX_FIELDS] =
{
    { "jit_ms", HEAT_FEX_STATS_JIT_TIME, 1 }, { "jit", HEAT_FEX_STATS_JIT_COUNT, 0 },
    { "miss", HEAT_FEX_STATS_MISS, 0 }, { "rlock_ms", HEAT_FEX_STATS_RLOCK, 1 },
    { "wlock_ms", HEAT_FEX_STATS_WLOCK, 1 }, { "smc", HEAT_FEX_STATS_SMC, 0 },
    { "sigbus", HEAT_FEX_STATS_SIGBUS, 0 }, { "float", HEAT_FEX_STATS_FLOAT, 0 },
    { "signal_ms", HEAT_FEX_STATS_SIG_TIME, 1 },
    { "dcache_hit", HEAT_FEX_STATS_DC_HIT, 0 }, { "dcache_miss", HEAT_FEX_STATS_DC_MISS, 0 },
    { "dcache_ms", HEAT_FEX_STATS_DC_TIME, 1 },
    { "tso_switches", HEAT_FEX_STATS_TSO, 0 }, { "native_exits", HEAT_FEX_STATS_EXITS, 0 },
    { "code_buffer_moves", HEAT_FEX_STATS_CODEBUF, 0 }, { "l1_resizes", HEAT_FEX_STATS_L1, 0 },
};

/* <tid>'s slot in the stats object at <base>, <bytes> long and at least HEAT_FEX_STATS_HEADER.
 * 1 and *slot: found. 0: no slot of that thread. -1: a version or slot size this reader doesn't
 * read. *slot_size is the header's, *slots the slots in the bytes in use. */
static inline int heat_fex_find( const unsigned char *base, size_t bytes, unsigned int tid,
                                 const unsigned char **slot, unsigned int *slot_size, unsigned int *slots )
{
    unsigned char version = base[HEAT_FEX_STATS_H_VERSION];
    unsigned short ss;
    unsigned int used, off, t;

    memcpy( &ss, base + HEAT_FEX_STATS_H_SLOT, sizeof(ss) );
    memcpy( &used, base + HEAT_FEX_STATS_H_SIZE, sizeof(used) );
    *slot_size = ss;
    *slots = 0;
    if (!(version == HEAT_FEX_STATS_VERSION && ss >= HEAT_FEX_STATS_SLOT) &&
        !(version == HEAT_FEX_STATS_VERSION_3 && ss >= HEAT_FEX_STATS_SLOT_3))
        return -1;
    if (used > bytes) used = bytes;
    if (used > HEAT_FEX_STATS_HEADER) *slots = (used - HEAT_FEX_STATS_HEADER) / ss;
    for (off = HEAT_FEX_STATS_HEADER; tid && off + ss <= used; off += ss)
    {
        memcpy( &t, base + off + HEAT_FEX_STATS_TID, sizeof(t) );
        if (t != tid) continue;
        *slot = base + off;
        return 1;
    }
    return 0;
}

/* The requests log's line for <tid> (jdrly/gw2-on-mac#111), one of
 *
 *   fex jit_ms 812.375 jit 20411 miss 5120 ... dcache_ms 0.000   each field's change since <prev>
 *   fex has no slot of 36                                      the slots in use
 *   fex stats version 4 slot 176                               a header it doesn't read
 *
 * <prev> holds the thread's values at its last line, 0 before the first, and gets the new ones. A
 * value smaller than before is printed as it is: FEX gave the slot to another thread with the same
 * id, and zeroed it. <ms> is milliseconds per tick. Returns the length, at most size - 1. */
static inline int heat_fex_line( char *buf, int size, const unsigned char *base, size_t bytes, unsigned int tid,
                                 unsigned long long *prev, double ms )
{
    const unsigned char *slot = NULL;
    unsigned long long v, d[HEAT_FEX_FIELDS];
    unsigned int slot_size, slots, n, i;
    int len = 0;

    if (size <= 0) return 0;
    switch (heat_fex_find( base, bytes, tid, &slot, &slot_size, &slots ))
    {
    case -1:
        len = snprintf( buf, size, "fex stats version %u slot %u\n", base[HEAT_FEX_STATS_H_VERSION], slot_size );
        break;
    case 0:
        len = snprintf( buf, size, "fex has no slot of %u\n", slots );
        break;
    default:
        n = base[HEAT_FEX_STATS_H_VERSION] == HEAT_FEX_STATS_VERSION_3 ? HEAT_FEX_FIELDS : HEAT_FEX_FIELDS_112;
        for (i = 0; i < n; i++)
        {
            memcpy( &v, slot + heat_fex_fields[i].at, sizeof(v) );
            d[i] = v >= prev[i] ? v - prev[i] : v;
            prev[i] = v;
        }
        len = snprintf( buf, size, "fex" );
        for (i = 0; i < n && len < size; i++)
        {
            if (heat_fex_fields[i].ms) len += snprintf( buf + len, size - len, " %s %.3f", heat_fex_fields[i].name, d[i] * ms );
            else len += snprintf( buf + len, size - len, " %s %llu", heat_fex_fields[i].name, d[i] );
        }
        if (len < size) len += snprintf( buf + len, size - len, "\n" );
        break;
    }
    return len < size ? len : size - 1;
}

/* WINE_HEAT_PROF_FEX (jdrly/gw2-on-mac#111): "<GUID>:<age>:<SizeOfImage>", as my FEX's VERSION
 * gives them on its line "libarm64ecfex.dll: build id <GUID> age <age>, SizeOfImage <size>": the
 * CodeView record's 16 GUID bytes as 32 hex digits in file order, the age in decimal, SizeOfImage
 * in hex with 0x (decimal works too). TRUE and the three values if <s> is exactly that; FALSE and
 * nothing written otherwise. */
static inline int heat_fex_parse_id( const char *s, unsigned char guid[16], unsigned int *age, unsigned int *size )
{
    unsigned char g[16];
    unsigned long a, n;
    char hex[3] = { 0 }, *end;
    int i;

    if (strspn( s, "0123456789abcdefABCDEF" ) != 32 || s[32] != ':' || !(s[33] >= '0' && s[33] <= '9')) return 0;
    for (i = 0; i < 16; i++)
    {
        memcpy( hex, s + 2 * i, 2 );
        g[i] = strtoul( hex, NULL, 16 );
    }
    a = strtoul( s + 33, &end, 10 );
    if (*end != ':' || !(end[1] >= '0' && end[1] <= '9')) return 0;
    n = strtoul( end + 1, &end, 0 );
    if (*end || !n || a > 0xffffffff || n > 0xffffffff) return 0;
    memcpy( guid, g, 16 );
    *age = a;
    *size = n;
    return 1;
}

#endif /* __NTDLL_UNIX_HEATFEX_H */
