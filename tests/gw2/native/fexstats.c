/* gw2 (jdrly/gw2-on-mac#111): the requests log's fex line (dlls/ntdll/unix/heatfex.h), built
 * natively and run without Wine on stats objects this program lays out the way FEX does: a 64-byte
 * header, then slots. Version 2 with 112-byte slots, as the engine's FEX makes, gives twelve
 * fields; version 3 with 144-byte slots, as my fork makes since jdrly/gw2-on-mac#117, sixteen; a
 * larger slot of either reads the same fields.
 * Each line holds the change since the thread's last one, a value that went down is printed as it
 * is, a thread without a slot gets "fex has no slot of <n>", and a version or slot size the reader
 * doesn't know gets "fex stats version <v> slot <bytes>" and nothing read.
 *
 * The tick rate (jdrly/gw2-on-mac#156): heat_fex_ms() turns a Mach rate and two readings of both
 * clocks into milliseconds per FEX tick, for a counter at Mach's 24 MHz and one at 1 GHz. Then on
 * this machine: the research note's check (CNTFRQ_EL0, and both counters over a 1 s sleep), and a
 * 100 ms sleep timed the way FEX times a scope, which the fex line has to print within 10% of its
 * length. No bound depends on how long the runner lets a sleep run: Mach's clock, read just inside
 * and just outside FEX's two reads, gives the lower and the upper one.
 *
 * Prints "gw2 test fexstats: passed" and exits 0, or names each case that failed and exits 1. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>

#include "../../../dlls/ntdll/unix/heatfex.h"

#define TID   0x1a4
#define OTHER 0x2b8
#define MS    0.001  /* ms per tick: 1000 ticks print as 1.000 */

static int failed;
static unsigned char obj[64 + 8 * 160];

static void expect( const char *what, const char *got, const char *want )
{
    if (!strcmp( got, want )) return;
    printf( "gw2 test fexstats: %s:\n  got  %s  want %s", what, got, want );
    failed++;
}

/* a header of <version> and <slot>, <slots> slots in use, all free */
static void layout( unsigned char version, unsigned short slot, unsigned int slots )
{
    unsigned int size = HEAT_FEX_STATS_HEADER + slots * slot;

    memset( obj, 0, sizeof(obj) );
    obj[HEAT_FEX_STATS_H_VERSION] = version;
    memcpy( obj + HEAT_FEX_STATS_H_SLOT, &slot, sizeof(slot) );
    memcpy( obj + HEAT_FEX_STATS_H_SIZE, &size, sizeof(size) );
}

static unsigned char *slot_at( unsigned short slot, unsigned int i )
{
    return obj + HEAT_FEX_STATS_HEADER + i * slot;
}

/* slot <i> of <tid>, each field <at> holding base + at */
static void fill( unsigned short slot, unsigned int i, unsigned int tid, unsigned long long base )
{
    unsigned char *s = slot_at( slot, i );
    unsigned long long v;
    unsigned int at;

    memcpy( s + HEAT_FEX_STATS_TID, &tid, sizeof(tid) );
    for (at = 8; at + 8 <= slot; at += 8)
    {
        v = base + at;
        memcpy( s + at, &v, sizeof(v) );
    }
}

static void set( unsigned short slot, unsigned int i, unsigned int at, unsigned long long v )
{
    memcpy( slot_at( slot, i ) + at, &v, sizeof(v) );
}

static const char *line( unsigned int tid, unsigned long long *prev )
{
    static char buf[1024];
    heat_fex_line( buf, sizeof(buf), obj, sizeof(obj), tid, prev, MS );
    return buf;
}

static unsigned long long cntvct( void )
{
    unsigned long long v;
    __asm__ volatile( "isb; mrs %0, CNTVCT_EL0" : "=r"(v) );
    return v;
}

static void sleep_ms( long ms )
{
    struct timespec t = { ms / 1000, ms % 1000 * 1000000 };
    while (nanosleep( &t, &t ) == -1 && errno == EINTR);
}

/* Mach at 24 MHz, 125/3 ns a tick: FEX's counter at the same rate, at 1 GHz, and not moved */
static void tick_rate( void )
{
    static const double mach_ms = 125.0 / 3 / 1e6;
    struct heat_fex_clock start = { 1000, 5000 }, now;
    double got;

    now.mach = start.mach + 24000000;
    now.cnt = start.cnt + 24000000;
    got = heat_fex_ms( &start, &now, mach_ms );
    if (got < mach_ms * 0.999999 || got > mach_ms * 1.000001)
    {
        printf( "gw2 test fexstats: a 24 MHz counter: %.9g ms a tick, want %.9g\n", got, mach_ms );
        failed++;
    }
    now.cnt = start.cnt + 1000000000;
    got = heat_fex_ms( &start, &now, mach_ms );
    if (got < 1e-6 * 0.999999 || got > 1e-6 * 1.000001)
    {
        printf( "gw2 test fexstats: a 1 GHz counter: %.9g ms a tick, want 1e-06\n", got );
        failed++;
    }
    now.cnt = start.cnt;
    if ((got = heat_fex_ms( &start, &now, mach_ms )))
    {
        printf( "gw2 test fexstats: a counter that didn't move: %.9g ms a tick, want 0\n", got );
        failed++;
    }
}

/* this machine's counters, and a sleep in a scope timed as FEX's AccumulationBlock times it
 * (SHMStats.h:97), read back through the fex line with the rate measured since <start> */
static void this_machine( const struct heat_fex_clock *start )
{
    unsigned long long frq, c0, c1, m0, m1, in0, in1, prev[HEAT_FEX_FIELDS] = { 0 };
    struct heat_fex_clock now;
    mach_timebase_info_data_t tb;
    double mach_ms, jit_ms, inner_ms, outer_ms;
    char buf[1024];

    mach_timebase_info( &tb );
    mach_ms = (double)tb.numer / tb.denom / 1e6;
    __asm__ volatile( "mrs %0, CNTFRQ_EL0" : "=r"(frq) );
    c0 = cntvct();
    m0 = mach_absolute_time();
    sleep_ms( 1000 );
    c1 = cntvct();
    m1 = mach_absolute_time();
    printf( "gw2 test fexstats: CNTFRQ_EL0 %llu Hz, CNTVCT_EL0 %llu ticks and mach_absolute_time %llu in a 1 s sleep, "
            "timebase %u/%u, ratio %.3f\n", frq, c1 - c0, m1 - m0, tb.numer, tb.denom, (double)(c1 - c0) / (m1 - m0) );

    /* Mach's reads around each of FEX's: the scope is at least the inner span and at most the outer */
    m0 = mach_absolute_time();
    c0 = cntvct();
    in0 = mach_absolute_time();
    sleep_ms( 100 );
    in1 = mach_absolute_time();
    c1 = cntvct();
    m1 = mach_absolute_time();
    inner_ms = (in1 - in0) * mach_ms;
    outer_ms = (m1 - m0) * mach_ms;
    layout( 3, 144, 1 );
    fill( 144, 0, TID, 0 );
    set( 144, 0, HEAT_FEX_STATS_JIT_TIME, c1 - c0 );
    heat_fex_clock_read( &now );
    heat_fex_line( buf, sizeof(buf), obj, sizeof(obj), TID, prev, heat_fex_ms( start, &now, mach_ms ) );
    if (sscanf( buf, "fex jit_ms %lf", &jit_ms ) != 1)
    {
        printf( "gw2 test fexstats: a 100 ms sleep: no jit_ms in %s", buf );
        failed++;
        return;
    }
    printf( "gw2 test fexstats: a 100 ms sleep in a timed scope: jit_ms %.3f, Mach's spans %.3f inside and %.3f "
            "outside it\n", jit_ms, inner_ms, outer_ms );
    if (jit_ms < inner_ms * 0.9 || jit_ms > outer_ms * 1.1)
    {
        printf( "gw2 test fexstats: a 100 ms sleep: jit_ms %.3f, want %.3f to %.3f\n", jit_ms, inner_ms * 0.9, outer_ms * 1.1 );
        failed++;
    }
}

int main( void )
{
    struct heat_fex_clock start;
    unsigned long long prev[HEAT_FEX_FIELDS] = { 0 }, other[HEAT_FEX_FIELDS] = { 0 };
    static const char *old[HEAT_FEX_FIELDS_OLD] = { "jit_ms", "jit", "miss", "rlock_ms", "wlock_ms", "smc",
                                                   "sigbus", "float", "signal_ms" };
    char small[24];
    unsigned int i;
    int len;

    heat_fex_clock_read( &start );

    /* the heat log's fex thread line has the first nine, in this order */
    for (i = 0; i < HEAT_FEX_FIELDS_OLD; i++) expect( "the heat log's field", heat_fex_fields[i].name, old[i] );

    /* 112-byte slots: the thread in the third of three, after another thread and a free one */
    layout( 2, 112, 3 );
    fill( 112, 0, OTHER, 5000 );
    fill( 112, 2, TID, 1000 );
    expect( "112, the first line: everything since the slot was given",
            line( TID, prev ),
            "fex jit_ms 1.008 jit 1072 miss 1048 rlock_ms 1.056 wlock_ms 1.064 smc 1032 sigbus 1024 float 1040 "
            "signal_ms 1.016 dcache_hit 1080 dcache_miss 1088 dcache_ms 1.096\n" );
    fill( 112, 2, TID, 3000 );
    set( 112, 2, HEAT_FEX_STATS_SMC, 1032 );  /* unchanged */
    expect( "112, the next line: the change",
            line( TID, prev ),
            "fex jit_ms 2.000 jit 2000 miss 2000 rlock_ms 2.000 wlock_ms 2.000 smc 0 sigbus 2000 float 2000 "
            "signal_ms 2.000 dcache_hit 2000 dcache_miss 2000 dcache_ms 2.000\n" );
    /* FEX gave the slot to a new thread with the same id, and zeroed it */
    fill( 112, 2, TID, 0 );
    expect( "112, a slot given again: the values as they are",
            line( TID, prev ),
            "fex jit_ms 0.008 jit 72 miss 48 rlock_ms 0.056 wlock_ms 0.064 smc 32 sigbus 24 float 40 "
            "signal_ms 0.016 dcache_hit 80 dcache_miss 88 dcache_ms 0.096\n" );
    expect( "112, the other thread's first line",
            line( OTHER, other ),
            "fex jit_ms 5.008 jit 5072 miss 5048 rlock_ms 5.056 wlock_ms 5.064 smc 5032 sigbus 5024 float 5040 "
            "signal_ms 5.016 dcache_hit 5080 dcache_miss 5088 dcache_ms 5.096\n" );
    expect( "112, a thread without a slot", line( 0x3cc, prev ), "fex has no slot of 3\n" );
    expect( "112, thread id 0 isn't a free slot's", line( 0, prev ), "fex has no slot of 3\n" );

    /* a slot past the bytes in use, which FEX's Size says, isn't read */
    layout( 2, 112, 2 );
    fill( 112, 2, TID, 1000 );
    expect( "112, a slot past Size", line( TID, prev ), "fex has no slot of 2\n" );
    /* nor one past the bytes mapped, when Size says more: FEX grew the object after the map */
    {
        char buf[256];
        layout( 2, 112, 3 );
        fill( 112, 1, TID, 1000 );
        heat_fex_line( buf, sizeof(buf), obj, HEAT_FEX_STATS_HEADER + 112, TID, prev, MS );
        expect( "112, a slot past the mapped bytes", buf, "fex has no slot of 1\n" );
    }

    /* version 3, 144-byte slots (jdrly/gw2-on-mac#117): sixteen fields */
    memset( prev, 0, sizeof(prev) );
    layout( 3, 144, 4 );
    fill( 144, 1, OTHER, 100 );
    fill( 144, 3, TID, 1000 );
    expect( "version 3, the first line",
            line( TID, prev ),
            "fex jit_ms 1.008 jit 1072 miss 1048 rlock_ms 1.056 wlock_ms 1.064 smc 1032 sigbus 1024 float 1040 "
            "signal_ms 1.016 dcache_hit 1080 dcache_miss 1088 dcache_ms 1.096 tso_switches 1112 native_exits 1120 "
            "code_buffer_moves 1128 l1_resizes 1136\n" );
    set( 144, 3, HEAT_FEX_STATS_TSO, 1112 + 7 );
    set( 144, 3, HEAT_FEX_STATS_L1, 1136 + 1 );
    expect( "version 3, the change in two of the four",
            line( TID, prev ),
            "fex jit_ms 0.000 jit 0 miss 0 rlock_ms 0.000 wlock_ms 0.000 smc 0 sigbus 0 float 0 signal_ms 0.000 "
            "dcache_hit 0 dcache_miss 0 dcache_ms 0.000 tso_switches 7 native_exits 0 code_buffer_moves 0 l1_resizes 1\n" );
    expect( "version 3, a thread without a slot", line( 0x3cc, prev ), "fex has no slot of 4\n" );

    /* a larger slot of the same version: the sixteen it knows, by offset */
    memset( prev, 0, sizeof(prev) );
    layout( 3, 160, 2 );
    fill( 160, 1, TID, 0 );
    expect( "version 3, 160-byte slots: the sixteen fields it knows",
            line( TID, prev ),
            "fex jit_ms 0.008 jit 72 miss 48 rlock_ms 0.056 wlock_ms 0.064 smc 32 sigbus 24 float 40 "
            "signal_ms 0.016 dcache_hit 80 dcache_miss 88 dcache_ms 0.096 tso_switches 112 native_exits 120 "
            "code_buffer_moves 128 l1_resizes 136\n" );

    /* version 2 with a larger slot: still the twelve, the four belong to version 3 */
    memset( prev, 0, sizeof(prev) );
    layout( 2, 144, 1 );
    fill( 144, 0, TID, 0 );
    expect( "version 2, 144-byte slots: the twelve",
            line( TID, prev ),
            "fex jit_ms 0.008 jit 72 miss 48 rlock_ms 0.056 wlock_ms 0.064 smc 32 sigbus 24 float 40 "
            "signal_ms 0.016 dcache_hit 80 dcache_miss 88 dcache_ms 0.096\n" );

    /* what it doesn't read: another version, or a slot under its version's; prev stays */
    memset( prev, 0, sizeof(prev) );
    layout( 4, 144, 2 );
    fill( 144, 0, TID, 1000 );
    expect( "version 4", line( TID, prev ), "fex stats version 4 slot 144\n" );
    layout( 3, 112, 2 );
    fill( 112, 0, TID, 1000 );
    expect( "version 3, a 112-byte slot", line( TID, prev ), "fex stats version 3 slot 112\n" );
    layout( 2, 104, 2 );
    fill( 104, 0, TID, 1000 );
    expect( "version 2, a 104-byte slot", line( TID, prev ), "fex stats version 2 slot 104\n" );
    for (i = 0; i < HEAT_FEX_FIELDS; i++)
        if (prev[i]) expect( "nothing read from a header it doesn't know", "a value", "none" );

    /* a buffer too small: cut, terminated, and the values taken all the same */
    layout( 2, 112, 1 );
    fill( 112, 0, TID, 1000 );
    len = heat_fex_line( small, sizeof(small), obj, sizeof(obj), TID, prev, MS );
    if (len != sizeof(small) - 1 || strlen( small ) != sizeof(small) - 1)
    {
        printf( "gw2 test fexstats: a short buffer: length %d, string %zu, want %zu\n", len, strlen( small ), sizeof(small) - 1 );
        failed++;
    }
    if (prev[HEAT_FEX_FIELDS_112 - 1] != 1000 + HEAT_FEX_STATS_DC_TIME)
        expect( "a short buffer still takes the values", "not taken", "taken" );

    /* WINE_HEAT_PROF_FEX as the app writes it from my FEX's VERSION (the build of 2026-11-12), and
     * the engine's own id in the same form: the GUID's bytes in file order, as the sampler compares */
    {
        static const unsigned char mine[16] = { 0xf7, 0x98, 0xdb, 0xfa, 0xa1, 0x1b, 0x3b, 0xc4,
                                                0x4c, 0x4c, 0x44, 0x20, 0x50, 0x44, 0x42, 0x2e };
        static const unsigned char engine[16] = { 0x65, 0x6e, 0xe1, 0xa2, 0x70, 0x27, 0x23, 0x3b,
                                                  0x4c, 0x4c, 0x44, 0x20, 0x50, 0x44, 0x42, 0x2e };
        static const char * const bad[] =
        {
            "", "f798dbfaa11b3bc44c4c44205044422:1:0x426000",     /* 31 digits */
            "f798dbfaa11b3bc44c4c44205044422e0:1:0x426000",       /* 33 */
            "f798dbfaa11b3bc44c4c44205044422g:1:0x426000",        /* not hex */
            "f798dbfaa11b3bc44c4c44205044422e", "f798dbfaa11b3bc44c4c44205044422e:1",
            "f798dbfaa11b3bc44c4c44205044422e:1:", "f798dbfaa11b3bc44c4c44205044422e::0x426000",
            "f798dbfaa11b3bc44c4c44205044422e:-1:0x426000", "f798dbfaa11b3bc44c4c44205044422e:0x1:0x426000",
            "f798dbfaa11b3bc44c4c44205044422e:1:0x426000 ", "f798dbfaa11b3bc44c4c44205044422e:1:0",
            "f798dbfaa11b3bc44c4c44205044422e:1:-0x426000", "f798dbfaa11b3bc44c4c44205044422e:1:0x100000000",
            "f798dbfaa11b3bc44c4c44205044422e 1 0x426000", "f798dbfaa11b3bc44c4c44205044422e:1: 0x426000",
            "f798dbfaa11b3bc44c4c44205044422e:1:+0x426000", "f798dbfaa11b3bc44c4c44205044422e: 1:0x426000",
        };
        unsigned char guid[16];
        unsigned int age, image;

        if (!heat_fex_parse_id( "f798dbfaa11b3bc44c4c44205044422e:1:0x426000", guid, &age, &image ) ||
            memcmp( guid, mine, 16 ) || age != 1 || image != 0x426000)
            expect( "WINE_HEAT_PROF_FEX, my FEX's VERSION", "not read as f798dbfa... age 1 0x426000", "read" );
        if (!heat_fex_parse_id( "656EE1A27027233B4C4C44205044422E:1:4349952", guid, &age, &image ) ||
            memcmp( guid, engine, 16 ) || age != 1 || image != 0x426000)
            expect( "WINE_HEAT_PROF_FEX, upper case and a decimal size", "not read", "read" );
        for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
        {
            memset( guid, 0xaa, 16 );
            age = image = 7;
            if (heat_fex_parse_id( bad[i], guid, &age, &image ) || guid[0] != 0xaa || age != 7 || image != 7)
            {
                printf( "gw2 test fexstats: WINE_HEAT_PROF_FEX=\"%s\" was taken, or half of it\n", bad[i] );
                failed++;
            }
        }
    }

    tick_rate();
    this_machine( &start );

    if (failed) return 1;
    printf( "gw2 test fexstats: passed\n" );
    return 0;
}
