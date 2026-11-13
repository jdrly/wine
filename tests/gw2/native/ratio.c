/* gw2 (jdrly/gw2-on-mac#159): win32u's make_ratio(), built natively and run without Wine. The
 * workflow cuts the function, and ratio_error() or the gcd() before it, out of
 * dlls/win32u/sysparams.c into make_ratio.c, which this file includes, so what runs is the file's
 * own code. Under the engine's code (gcd() and two asserts) the first case aborts.
 *
 * - 214464/365, the height term of a 1168x730 mode on a 3456x2234 display at 192 DPI, gives
 *   64633/110, and no fraction with both terms below 65536 is closer. 365/214464 gives 110/64633.
 * - The same for the modes my issue names, on a 3840x2160, a 3456x2234 and a 3024x1964 display,
 *   and for their reciprocals, where the denominator is the term that doesn't fit.
 * - A ratio whose lowest terms fit comes back in lowest terms.
 * - Every mode from 320x200 up to the display's size, on seven displays at five DPIs, as
 *   monitor_get_dpi() asks for them (the DPI times the physical size, over the mode's): lowest
 *   terms where they fit, otherwise within 1/65535 of the ratio, and no assert.
 *
 * Prints "gw2 test ratio: passed" and exits 0, or names each case that failed and exits 1. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

typedef unsigned int UINT;
typedef uint64_t UINT64;
typedef int64_t INT64;

/* server/protocol.def's */
struct ratio
{
    unsigned short num;
    unsigned short den;
};

#define min(a,b)   (((a) < (b)) ? (a) : (b))  /* include/windef.h's */

#include "make_ratio.c"

static int failed;

static UINT64 gcd64( UINT64 a, UINT64 b )
{
    while (b)
    {
        UINT64 t = a % b;
        a = b;
        b = t;
    }
    return a;
}

/* |p/q - num/den| * q * den */
static unsigned __int128 error_of( UINT num, UINT den, UINT p, UINT q )
{
    INT64 d = (INT64)p * den - (INT64)num * q;
    return d < 0 ? -d : d;
}

/* whether p1/q1 is at least as close to num/den as p2/q2 */
static int not_farther( UINT num, UINT den, UINT p1, UINT q1, UINT p2, UINT q2 )
{
    return error_of( num, den, p1, q1 ) * q2 <= error_of( num, den, p2, q2 ) * q1;
}

/* the closest fraction to num/den with both terms below 65536, by trying every denominator */
static void closest( UINT num, UINT den, UINT *bp, UINT *bq )
{
    UINT q, p, i;

    *bp = 0;
    *bq = 1;
    for (q = 1; q <= 0xffff; q++)
    {
        p = (UINT)((UINT64)num * q / den);
        for (i = 0; i < 2; i++, p++)
            if (p <= 0xffff && !not_farther( num, den, *bp, *bq, p, q ))
            {
                *bp = p;
                *bq = q;
            }
    }
}

/* make_ratio(num, den) is want, and no fraction with 16-bit terms is closer */
static void exact( const char *what, UINT num, UINT den, UINT want_num, UINT want_den )
{
    struct ratio r = make_ratio( num, den );
    UINT p, q;

    if (r.num != want_num || r.den != want_den)
    {
        printf( "gw2 test ratio: %s: %u/%u gave %u/%u, expected %u/%u\n", what, num, den, r.num, r.den, want_num, want_den );
        failed++;
        return;
    }
    closest( num, den, &p, &q );
    if (!not_farther( num, den, r.num, r.den, p, q ))
    {
        printf( "gw2 test ratio: %s: %u/%u gave %u/%u, but %u/%u is closer\n", what, num, den, r.num, r.den, p, q );
        failed++;
    }
}

/* make_ratio(num, den) is the closest fraction with 16-bit terms */
static void best( const char *what, UINT num, UINT den )
{
    struct ratio r = make_ratio( num, den );
    UINT p, q;

    closest( num, den, &p, &q );
    if (!r.den || !not_farther( num, den, r.num, r.den, p, q ))
    {
        printf( "gw2 test ratio: %s: %u/%u gave %u/%u, but %u/%u is closer\n", what, num, den, r.num, r.den, p, q );
        failed++;
    }
}

/* one term as monitor_get_dpi() asks for it: lowest terms where they fit, else within 1/65535 */
static int sweep_one( UINT dpi, UINT physical, UINT mode )
{
    UINT num = dpi * physical, den = mode;
    UINT64 g = gcd64( num, den );
    struct ratio r = make_ratio( num, den );

    if (num / g <= 0xffff && den / g <= 0xffff)
    {
        if (r.num == num / g && r.den == den / g) return 0;
    }
    else if (r.den && error_of( num, den, r.num, r.den ) * 0xffff <= (unsigned __int128)num * r.den) return 1;
    if (failed++ < 20)
        printf( "gw2 test ratio: %u DPI, %u over %u: %u/%u gave %u/%u\n", dpi, physical, mode, num, den, r.num, r.den );
    return 0;
}

int main( void )
{
    static const struct { const char *what; UINT width, height, dpi, mode_width, mode_height; } issue[] =
    {
        { "1366x768 on 3840x2160", 3840, 2160, 192, 1366, 768 },
        { "1366x768 on 3456x2234", 3456, 2234, 192, 1366, 768 },
        { "1366x768 on 3024x1964", 3024, 1964, 192, 1366, 768 },
        { "1512x982 on 3840x2160", 3840, 2160, 192, 1512, 982 },
        { "1728x1117 on 3840x2160", 3840, 2160, 192, 1728, 1117 },
        { "1800x1169 on 3840x2160", 3840, 2160, 192, 1800, 1169 },
        { "2056x1329 on 3840x2160", 3840, 2160, 192, 2056, 1329 },
    };
    static const UINT displays[][2] =
    {
        { 3840, 2160 }, { 3456, 2234 }, { 3024, 1964 }, { 2560, 1600 }, { 1920, 1080 }, { 5120, 2880 }, { 6016, 3384 },
    };
    static const UINT dpis[] = { 96, 120, 144, 192, 288 };
    unsigned int i, j, w, h, terms = 0, approximated = 0;

    setvbuf( stdout, NULL, _IONBF, 0 );

    exact( "1168x730 on 3456x2234, height", 214464, 365, 64633, 110 );
    exact( "the same unreduced", 192 * 2234, 730, 64633, 110 );
    exact( "its reciprocal", 365, 214464, 110, 64633 );
    for (i = 0; i < sizeof(issue) / sizeof(issue[0]); i++)
    {
        best( issue[i].what, issue[i].dpi * issue[i].width, issue[i].mode_width );
        best( issue[i].what, issue[i].dpi * issue[i].height, issue[i].mode_height );
        best( issue[i].what, issue[i].mode_width, issue[i].dpi * issue[i].width );
        best( issue[i].what, issue[i].mode_height, issue[i].dpi * issue[i].height );
    }

    exact( "the physical mode", 192 * 3840, 3840, 192, 1 );
    exact( "1920x1080 on 3840x2160", 192 * 3840, 1920, 384, 1 );
    exact( "1440x900 on 2560x1600", 96 * 2560, 1440, 512, 3 );
    exact( "1728x1117 on 3456x2234", 192 * 2234, 1117, 384, 1 );
    exact( "6/4", 6, 4, 3, 2 );
    exact( "0/5", 0, 5, 0, 1 );
    exact( "65535/1", 65535, 1, 65535, 1 );
    exact( "1/65535", 1, 65535, 1, 65535 );
    exact( "65535/65534 times 3", 65535 * 3, 65534 * 3, 65535, 65534 );

    for (i = 0; i < sizeof(displays) / sizeof(displays[0]); i++)
        for (j = 0; j < sizeof(dpis) / sizeof(dpis[0]); j++)
        {
            for (w = 320; w <= displays[i][0]; w++, terms++) approximated += sweep_one( dpis[j], displays[i][0], w );
            for (h = 200; h <= displays[i][1]; h++, terms++) approximated += sweep_one( dpis[j], displays[i][1], h );
        }
    printf( "gw2 test ratio: %u of %u mode terms don't fit 16 bits in lowest terms, approximated\n", approximated, terms );
    if (!approximated)
    {
        printf( "gw2 test ratio: the sweep approximated nothing, so it didn't test the approximation\n" );
        failed++;
    }

    if (failed) return 1;
    printf( "gw2 test ratio: passed\n" );
    return 0;
}
