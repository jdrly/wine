/*
 * Mac driver: a speed factor for the GameController mouse's counts
 *
 * GW2 on Mac (jdrly/gw2-on-mac#59). WINE_MACDRV_MOUSE_SCALE multiplies the movement a
 * GameController mouse reports before cocoa_gcmouse.m adds it to the sum not yet posted, so the
 * fractions of the scaled counts carry over to the next post as the unscaled ones do. This header
 * has no Wine or Cocoa type, so tests/gw2/native/mouse_scale.c builds it into a native test.
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

#ifndef __WINE_MACDRV_GCMOUSE_SCALE_H
#define __WINE_MACDRV_GCMOUSE_SCALE_H

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

/* WINE_MACDRV_MOUSE_SCALE, read once by setup_options (macdrv_main.c); 1 when unset */
extern double gcmouse_scale;

/* <str> as a factor: a decimal number above 0 and up to 100, digits and a point only, so no sign,
   exponent, hex, space, inf or nan. strtod reads it with Wine's LC_NUMERIC, which ntdll sets to
   "C", so the point is always "." */
static inline bool gcmouse_parse_scale(const char *str, double *scale)
{
    char *end;
    double value;

    if (str[strspn(str, "0123456789.")]) return false;
    value = strtod(str, &end);
    if (end == str || *end || !(value > 0 && value <= 100)) return false;
    *scale = value;
    return true;
}

/* adds one report to the sum not yet posted, scaled first. At 1 the product is the delta itself,
   so the sum is the one without a factor. */
static inline void gcmouse_add_movement(double *sum_x, double *sum_y, float delta_x, float delta_y,
                                        double scale)
{
    *sum_x += delta_x * scale;
    *sum_y -= delta_y * scale;  /* GameController's y axis points up */
}

/* takes the whole counts out of <sum> and leaves the fraction for the next post */
static inline int gcmouse_take_counts(double *sum)
{
    int counts = *sum;

    *sum -= counts;
    return counts;
}

#endif /* __WINE_MACDRV_GCMOUSE_SCALE_H */
