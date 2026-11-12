/* gw2 (jdrly/gw2-on-mac#62): the Mac driver's event pipe functions (dlls/winemac.drv/cocoa_calls.h),
 * built natively for macOS and run without Wine.
 *
 * 1. WINE_MACDRV_DRAIN=fionread: event_pipe_bytes against poll's answer for an empty pipe, one
 *    byte, 100 bytes, and a closed write end with a byte left and without. A closed write end with
 *    nothing left is the one case where they differ: poll reports the end of the pipe, FIONREAD 0
 *    bytes. The driver's queue closes both ends together (-[WineEventQueue dealloc]), so its pump
 *    never meets it.
 * 2. WINE_MACDRV_CALL=quiet: event_pipe_settle after a synchronous call. With no event queued the
 *    pipe ends empty, from the call's one byte, from 100 left over, and from none. With one queued
 *    it ends with one byte: from the call's, from that and the event's own, and from none, when
 *    the wait loop read both.
 *
 * Prints "gw2 test pipe: passed" and exits 0, or names each case that failed and exits 1. */
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "../../../dlls/winemac.drv/cocoa_calls.h"

static int failed;

/* a pipe set up as -[WineEventQueue initWithEventHandler:] sets up its own */
static void make_pipe( int fds[2] )
{
    if (pipe( fds ) || fcntl( fds[0], F_SETFL, O_NONBLOCK ) == -1 || fcntl( fds[1], F_SETFL, O_NONBLOCK ) == -1)
    {
        perror( "gw2 test pipe: pipe" );
        _exit( 1 );
    }
}

static void put( int fd, int count )
{
    char buf[128];

    memset( buf, 1, sizeof(buf) );
    if (write( fd, buf, count ) != count)
    {
        perror( "gw2 test pipe: write" );
        _exit( 1 );
    }
}

static int poll_revents( int fd )
{
    struct pollfd pfd = {.fd = fd, .events = POLLIN};
    return poll( &pfd, 1, 0 ) > 0 ? pfd.revents : 0;
}

/* <count> bytes in a pipe, its write end closed if <closed>: FIONREAD gives the count, and says
   "a byte waits" exactly when poll says the read end is readable, but for <eof_differs> */
static void bytes_case( const char *what, int count, int closed, int eof_differs )
{
    int fds[2], bytes, revents;

    make_pipe( fds );
    if (count) put( fds[1], count );
    if (closed) close( fds[1] );
    bytes = event_pipe_bytes( fds[0] );
    revents = poll_revents( fds[0] );
    printf( "gw2 test pipe: %s: FIONREAD %d, poll %#x\n", what, bytes, revents );
    if (bytes != count)
    {
        printf( "gw2 test pipe: %s: FIONREAD gave %d bytes, not %d\n", what, bytes, count );
        failed++;
    }
    if (!eof_differs && (bytes > 0) != (revents != 0))
    {
        printf( "gw2 test pipe: %s: FIONREAD and poll disagree\n", what );
        failed++;
    }
    if (eof_differs && !revents)
    {
        printf( "gw2 test pipe: %s: poll doesn't report the end of the pipe\n", what );
        failed++;
    }
    close( fds[0] );
    if (!closed) close( fds[1] );
}

/* <count> bytes in the pipe after a call, <pending> events queued: event_pipe_settle leaves <want> */
static void settle_case( const char *what, int count, int pending, int want )
{
    int fds[2], left;

    make_pipe( fds );
    if (count) put( fds[1], count );
    event_pipe_settle( fds, pending );
    if ((left = event_pipe_bytes( fds[0] )) != want)
    {
        printf( "gw2 test pipe: %s: %d bytes left, not %d\n", what, left, want );
        failed++;
    }
    close( fds[0] );
    close( fds[1] );
}

int main( void )
{
    bytes_case( "an empty pipe", 0, 0, 0 );
    bytes_case( "one byte", 1, 0, 0 );
    bytes_case( "100 bytes", 100, 0, 0 );
    bytes_case( "one byte, the write end closed", 1, 1, 0 );
    bytes_case( "nothing, the write end closed", 0, 1, 1 );

    settle_case( "the call's byte, no event", 1, 0, 0 );
    settle_case( "100 bytes, no event", 100, 0, 0 );
    settle_case( "no byte, no event", 0, 0, 0 );
    settle_case( "the call's byte, one event", 1, 1, 1 );
    settle_case( "the call's byte and the event's, one event", 2, 1, 1 );
    settle_case( "no byte, one event", 0, 1, 1 );

    if (failed)
    {
        printf( "gw2 test pipe: %d failed\n", failed );
        return 1;
    }
    printf( "gw2 test pipe: passed\n" );
    return 0;
}
