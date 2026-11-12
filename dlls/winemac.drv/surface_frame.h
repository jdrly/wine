/*
 * Mac driver: is a window surface's frame the one Cocoa has?
 *
 * GW2 on Mac (jdrly/gw2-on-mac#57). WINE_MACDRV_SURFACE=copy makes the flush give Cocoa a copy of
 * the bits; copy,skip also keeps that copy, and a frame equal to it is not handed over again. This
 * header has no Wine or Cocoa type, so tests/gw2/frames.c builds it into a native test.
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

#ifndef __WINE_MACDRV_SURFACE_FRAME_H
#define __WINE_MACDRV_SURFACE_FRAME_H

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

/* WINE_MACDRV_SURFACE, read once by setup_options (macdrv_main.c) */
extern bool surface_copy;
extern bool surface_skip;

/* what a flush hands to Cocoa: the bits, their alpha mode and where the surface goes in the
   window (the flush moves it for a letterboxed full-screen window) */
struct surface_frame
{
    const void   *bits;     /* NULL before the first frame */
    size_t        size;
    unsigned int  alpha;
    int           left, top, right, bottom;
};

/* whether <frame> is <shown>, the one Cocoa has: the same bytes, alpha mode and surface rect,
   and the window's shape unchanged since */
static inline bool surface_frame_is_shown(const struct surface_frame *shown, const struct surface_frame *frame,
                                          bool shape_changed)
{
    return !shape_changed && shown->bits && shown->size == frame->size && shown->alpha == frame->alpha &&
           shown->left == frame->left && shown->top == frame->top && shown->right == frame->right &&
           shown->bottom == frame->bottom && !memcmp(shown->bits, frame->bits, frame->size);
}

#endif /* __WINE_MACDRV_SURFACE_FRAME_H */
