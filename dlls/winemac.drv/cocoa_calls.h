/*
 * Mac driver: fewer calls to the Cocoa main thread
 *
 * GW2 on Mac (jdrly/gw2-on-mac#62). Four switches, read once by setup_options (macdrv_main.c), each
 * off while its variable is unset:
 * - WINE_MACDRV_VIEW=changes: a client surface's update sends its view's frame and superview to the
 *   main thread only when they differ from what that surface last sent (window.c).
 * - WINE_MACDRV_CLIP=changes: ClipCursor doesn't call the main thread for the rectangle it applied
 *   last, while nothing has changed the clipping since (mouse.c).
 * - WINE_MACDRV_CALL=quiet: a synchronous call to the main thread takes the byte it left in the
 *   caller's event pipe back out when the caller's queue holds no event (cocoa_event.m).
 * - WINE_MACDRV_DRAIN=fionread: ProcessEvents asks the event pipe for its byte count instead of
 *   polling it (event.c).
 * The pipe functions have no Wine or Cocoa type, so tests/gw2/native/pipe.c builds them natively.
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

#ifndef __WINE_MACDRV_COCOA_CALLS_H
#define __WINE_MACDRV_COCOA_CALLS_H

#include <errno.h>
#include <stdbool.h>
#include <sys/ioctl.h>
#include <unistd.h>

extern bool view_changes;   /* WINE_MACDRV_VIEW=changes */
extern bool clip_changes;   /* WINE_MACDRV_CLIP=changes */
extern bool call_quiet;     /* WINE_MACDRV_CALL=quiet */
extern bool drain_fionread; /* WINE_MACDRV_DRAIN=fionread */

/* Bumped wherever the cursor clipping changes other than through ClipCursor's own call: a handler
   of cocoa_cursorclipping.m changing its state, and a display change (event.c). Atomic. */
extern unsigned int clip_serial;

/* the bytes waiting in the pipe's read end <fd>, or -1 if it can't say */
static inline int event_pipe_bytes(int fd)
{
    int count;

    return ioctl(fd, FIONREAD, &count) == -1 ? -1 : count;
}

/* After a synchronous call: with no event <pending> in the queue, read every byte out of the pipe's
   non-blocking read end <fd>; with one, leave the pipe as it is. The caller holds the queue's lock,
   so an event posted meanwhile writes its own byte after this. */
static inline void event_pipe_settle(int fd, bool pending)
{
    char buf[64];
    ssize_t rc;

    if (pending) return;
    do rc = read(fd, buf, sizeof(buf));
    while (rc == (ssize_t)sizeof(buf) || (rc < 0 && errno == EINTR));
}

#endif /* __WINE_MACDRV_COCOA_CALLS_H */
