/* gw2 (jdrly/gw2-on-mac#59): the Mac driver's speed factor for the GameController mouse's counts
 * (dlls/winemac.drv/gcmouse_scale.h), built natively for macOS and run without Wine. The driver
 * scales each report before it adds it to the sum not yet posted and takes the whole counts out of
 * that sum when it posts, so no fraction of a scaled count gets lost.
 *
 * Prints "gw2 test mouse_scale: passed" and exits 0, or names each case that failed and exits 1. */
#include <stdio.h>
#include <string.h>

#include "../../../dlls/winemac.drv/gcmouse_scale.h"

double gcmouse_scale = 1;

static int failed;

static void fail( const char *what )
{
    printf( "gw2 test mouse_scale: %s\n", what );
    failed++;
}

/* <n> reports of <dx>, <dy> at <scale>, posted after every <every>-th report and at the end, as
   cocoa_gcmouse.m posts them; returns the counts posted and leaves the sums' fractions */
static void move( int n, float dx, float dy, double scale, int every, long *x, long *y,
                  double *sum_x, double *sum_y )
{
    int i;

    *x = *y = 0;
    *sum_x = *sum_y = 0;
    for (i = 1; i <= n; i++)
    {
        gcmouse_add_movement( sum_x, sum_y, dx, dy, scale );
        if (i % every && i < n) continue;
        *x += gcmouse_take_counts( sum_x );
        *y += gcmouse_take_counts( sum_y );
    }
}

static void check_sum( int every )
{
    double sum_x, sum_y;
    long x, y;
    char what[160];

    /* test 5: 1,000 reports of 0.3 counts at 1.5 are 450 counts, whichever reports the posts
       fall after */
    move( 1000, 0.3f, 0.3f, 1.5, every, &x, &y, &sum_x, &sum_y );
    if (x == 450 && y == -450 && sum_x >= 0 && sum_x < 1 && sum_y <= 0 && sum_y > -1) return;
    snprintf( what, sizeof(what), "1,000 reports of 0.3 at 1.5, a post every %d: %ld, %ld counts posted, "
              "%g, %g left, expected 450, -450", every, x, y, sum_x, sum_y );
    fail( what );
}

/* athei's cocoa_gcmouse.m before the factor (wine 53def89293a, :70-74 and :105-107), the
   reference for the sums at 1 */
static void engine_add( double *sum_x, double *sum_y, float delta_x, float delta_y )
{
    *sum_x += delta_x;
    *sum_y -= delta_y;
}

static int engine_take( double *sum )
{
    int counts = *sum;

    *sum -= counts;
    return counts;
}

static void check_one( void )
{
    double sum_x = 0, sum_y = 0, engine_x = 0, engine_y = 0;
    unsigned int seed = 59;
    int i;

    for (i = 0; i < 100000; i++)
    {
        float dx, dy;

        /* deltas of every size GameController reports, both signs, fractions included */
        seed = seed * 1103515245 + 12345;
        dx = (float)(int)(seed >> 8 & 0xffff) / 997 - 32;
        seed = seed * 1103515245 + 12345;
        dy = (float)(int)(seed >> 8 & 0xffff) / 61 - 500;
        if (i % 3 == 0) dx /= 4096;
        gcmouse_add_movement( &sum_x, &sum_y, dx, dy, 1 );
        engine_add( &engine_x, &engine_y, dx, dy );
        if (memcmp( &sum_x, &engine_x, sizeof(double) ) || memcmp( &sum_y, &engine_y, sizeof(double) ))
        {
            fail( "at 1 a sum differs from the engine's" );
            return;
        }
        if (i % 5) continue;
        if (gcmouse_take_counts( &sum_x ) != engine_take( &engine_x ) ||
            gcmouse_take_counts( &sum_y ) != engine_take( &engine_y ) ||
            memcmp( &sum_x, &engine_x, sizeof(double) ) || memcmp( &sum_y, &engine_y, sizeof(double) ))
        {
            fail( "at 1 the counts posted or the fraction left differ from the engine's" );
            return;
        }
    }
}

static void check_parse( const char *str, bool ok, double want )
{
    double scale = 1;
    char what[160];

    if (gcmouse_parse_scale( str, &scale ) == ok && scale == (ok ? want : 1)) return;
    snprintf( what, sizeof(what), "\"%s\": %s, %g, expected %s", str, ok ? "refused" : "taken", scale,
              ok ? "taken" : "refused, the factor kept" );
    fail( what );
}

int main( void )
{
    static const int every[] = { 1, 2, 3, 7, 1000 };
    int i;

    for (i = 0; i < sizeof(every) / sizeof(every[0]); i++) check_sum( every[i] );
    check_one();

    check_parse( "1.5", true, 1.5 );
    check_parse( "1", true, 1 );
    check_parse( "0.25", true, 0.25 );
    check_parse( "100", true, 100 );
    check_parse( "", false, 0 );
    check_parse( "0", false, 0 );
    check_parse( "-1.5", false, 0 );
    check_parse( "100.5", false, 0 );
    check_parse( "1e3", false, 0 );
    check_parse( "1,5", false, 0 );
    check_parse( "1.5x", false, 0 );
    check_parse( "fast", false, 0 );
    check_parse( "nan", false, 0 );
    check_parse( "inf", false, 0 );

    if (failed) return 1;
    printf( "gw2 test mouse_scale: passed\n" );
    return 0;
}
