/* gw2 (jdrly/gw2-on-mac#88): moves and clicks the pointer for tests/gw2/assist.c, built natively
 * for macOS and run without Wine. The events go into the HID stream (CGEventPost), so the window
 * server and AppKit see them as a hand's.
 *
 *   pointer move <x> <y>      moves the pointer to the point
 *   pointer click <x> <y>     moves to the point, then a left click there
 *   pointer rclick <x> <y>    a right click at the point
 *
 * The workflow waits for the driver's line after each move before the next. Points are in the
 * global display space, the main display's top left at 0,0, which is Windows' screen space under
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
}

static void click( CGPoint point, CGEventType down, CGEventType up, CGMouseButton button )
{
    post( down, point, button );
    usleep( 100000 );
    post( up, point, button );
}

int main( int argc, char **argv )
{
    CGPoint point;

    if (argc != 4 || (strcmp( argv[1], "move" ) && strcmp( argv[1], "click" ) && strcmp( argv[1], "rclick" )))
    {
        fprintf( stderr, "usage: pointer move|click|rclick <x> <y>\n" );
        return 2;
    }
    if (!CGPreflightPostEventAccess())
    {
        printf( "pointer: this process may not post events (CGPreflightPostEventAccess)\n" );
        return 1;
    }
    point = CGPointMake( atof( argv[2] ), atof( argv[3] ) );
    if (!strcmp( argv[1], "rclick" ))
        click( point, kCGEventRightMouseDown, kCGEventRightMouseUp, kCGMouseButtonRight );
    else
    {
        post( kCGEventMouseMoved, point, kCGMouseButtonLeft );
        if (!strcmp( argv[1], "click" ))
        {
            usleep( 100000 );
            click( point, kCGEventLeftMouseDown, kCGEventLeftMouseUp, kCGMouseButtonLeft );
        }
    }
    return 0;
}
