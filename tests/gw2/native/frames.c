/* gw2 (jdrly/gw2-on-mac#57): the Mac driver's decision "is this frame the one Cocoa has"
 * (dlls/winemac.drv/surface_frame.h), built natively for macOS and run without Wine. A frame is
 * the one shown only with the same bytes, alpha mode and surface rect, the shape unchanged, and a
 * frame shown before it.
 *
 * Prints "gw2 test frames: passed" and exits 0, or names each case that failed and exits 1. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../../dlls/winemac.drv/surface_frame.h"

#define WIDTH 64
#define HEIGHT 32
#define SIZE (WIDTH * HEIGHT * 4)

static int failed;

static void expect( const char *what, bool got, bool want )
{
    if (got == want) return;
    printf( "gw2 test frames: %s: %s, expected %s\n", what, got ? "shown" : "not shown", want ? "shown" : "not shown" );
    failed++;
}

int main( void )
{
    unsigned char *shown_bits = malloc( SIZE ), *bits = malloc( SIZE );
    struct surface_frame first, shown, frame;
    int i;

    if (!shown_bits || !bits) return 1;
    for (i = 0; i < SIZE; i++) shown_bits[i] = i * 7;
    memcpy( bits, shown_bits, SIZE );
    shown = (struct surface_frame){ .bits = shown_bits, .size = SIZE, .alpha = 2, .left = 0, .top = 0,
                                    .right = WIDTH, .bottom = HEIGHT };
    frame = shown;
    frame.bits = bits;  /* equal bytes in another buffer, as the surface's own memory is */

    expect( "equal bytes, alpha mode and rect", surface_frame_is_shown( &shown, &frame, false ), true );
    expect( "the same with the shape changed", surface_frame_is_shown( &shown, &frame, true ), false );

    bits[SIZE - 1] ^= 1;
    expect( "one changed byte, the last", surface_frame_is_shown( &shown, &frame, false ), false );
    bits[SIZE - 1] ^= 1;
    bits[0] ^= 0x80;
    expect( "one changed byte, the first", surface_frame_is_shown( &shown, &frame, false ), false );
    bits[0] ^= 0x80;

    frame.alpha = 6;
    expect( "another alpha mode", surface_frame_is_shown( &shown, &frame, false ), false );
    frame.alpha = shown.alpha;

    frame.left += 80;
    frame.right += 80;  /* the same size, moved, as for a letterboxed full-screen window */
    expect( "the surface rect moved", surface_frame_is_shown( &shown, &frame, false ), false );
    frame = shown;
    frame.bits = bits;
    for (i = 0; i < 4; i++)  /* each edge alone */
    {
        int *edge[] = { &frame.left, &frame.top, &frame.right, &frame.bottom };
        static const char *name[] = { "another left edge", "another top edge", "another right edge", "another bottom edge" };

        *edge[i] += 1;
        expect( name[i], surface_frame_is_shown( &shown, &frame, false ), false );
        *edge[i] -= 1;
    }

    frame.size = SIZE / 2;
    expect( "another size", surface_frame_is_shown( &shown, &frame, false ), false );
    frame.size = SIZE;

    first = shown;
    first.bits = NULL;  /* as a new surface has it */
    expect( "the first frame, nothing shown before", surface_frame_is_shown( &first, &frame, false ), false );
    expect( "equal again after every case", surface_frame_is_shown( &shown, &frame, false ), true );

    if (failed) return 1;
    printf( "gw2 test frames: passed\n" );
    return 0;
}
