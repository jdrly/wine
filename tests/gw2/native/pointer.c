/* gw2 (jdrly/gw2-on-mac#88): moves and clicks the pointer for tests/gw2/assist.c, built natively
 * for macOS and run without Wine. The events go into the HID stream (CGEventPost), so the window
 * server and AppKit see them as a hand's.
 *
 *   pointer click <x> <y>                     moves to the point, then a left click there
 *   pointer trips <n> <x1> <y1> <x2> <y2>     <n> times: to (x1,y1), then to (x2,y2)
 *   pointer rclick <x> <y>                    a right click at the point
 *
 * Each event is 100 ms after the one before, so AppKit handles every move on its own. Points are in
 * the global display space, the main display's top left at 0,0, which is Windows' screen space under
 * Wine's Mac driver without Retina mode.
 *
 * Exits 1 when this process may not post events, and 2 on bad arguments. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <CoreGraphics/CoreGraphics.h>

static void post( CGEventType type, CGPoint point, CGMouseButton button )
{
    CGEventRef event = CGEventCreateMouseEvent( NULL, type, point, button );

    CGEventPost( kCGHIDEventTap, event );
    CFRelease( event );
    usleep( 100000 );
}

static void click( CGPoint point, CGEventType down, CGEventType up, CGMouseButton button )
{
    post( down, point, button );
    post( up, point, button );
}

int main( int argc, char **argv )
{
    CGPoint a, b;
    int i, n;

    if (!CGPreflightPostEventAccess())
    {
        printf( "pointer: this process may not post events (CGPreflightPostEventAccess)\n" );
        return 1;
    }
    if (argc == 4 && !strcmp( argv[1], "click" ))
    {
        a = CGPointMake( atof( argv[2] ), atof( argv[3] ) );
        post( kCGEventMouseMoved, a, kCGMouseButtonLeft );
        click( a, kCGEventLeftMouseDown, kCGEventLeftMouseUp, kCGMouseButtonLeft );
    }
    else if (argc == 7 && !strcmp( argv[1], "trips" ))
    {
        n = atoi( argv[2] );
        a = CGPointMake( atof( argv[3] ), atof( argv[4] ) );
        b = CGPointMake( atof( argv[5] ), atof( argv[6] ) );
        for (i = 0; i < n; i++)
        {
            post( kCGEventMouseMoved, a, kCGMouseButtonLeft );
            post( kCGEventMouseMoved, b, kCGMouseButtonLeft );
        }
    }
    else if (argc == 4 && !strcmp( argv[1], "rclick" ))
    {
        a = CGPointMake( atof( argv[2] ), atof( argv[3] ) );
        click( a, kCGEventRightMouseDown, kCGEventRightMouseUp, kCGMouseButtonRight );
    }
    else
    {
        fprintf( stderr, "usage: pointer click <x> <y> | trips <n> <x1> <y1> <x2> <y2> | rclick <x> <y>\n" );
        return 2;
    }
    return 0;
}
