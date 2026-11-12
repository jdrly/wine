/* gw2 (jdrly/gw2-on-mac#57): a layered window drawn the way the GW2 launcher draws its own. It is
 * shown, then UpdateLayeredWindow gives it 600 frames, every third with new pixels and the two
 * after it equal to it. Each call flushes the window surface once, so with WINEDEBUG=+macsurface
 * the Mac driver prints its line at the 600th: "600 flushes, 400 skipped" under
 * WINE_MACDRV_SURFACE=copy,skip, "0 skipped" without it. The workflow reads that line in the log.
 *
 * Prints "gw2 test layered: passed" and exits 0 when every call succeeded, or names what failed
 * and exits 1. */
#include <stdio.h>
#include <windows.h>

#define FRAMES 600
#define WIDTH 256
#define HEIGHT 128

static int fail( const char *what )
{
    printf( "gw2 test layered: %s failed (error %lu)\n", what, GetLastError() );
    return 1;
}

static void pump( void )
{
    MSG msg;

    while (PeekMessageW( &msg, NULL, 0, 0, PM_REMOVE ))
    {
        TranslateMessage( &msg );
        DispatchMessageW( &msg );
    }
}

int main( void )
{
    /* no background brush: nothing but UpdateLayeredWindow draws into the surface */
    WNDCLASSW cls = { .lpfnWndProc = DefWindowProcW, .lpszClassName = L"gw2 test layered" };
    BITMAPINFO info = { .bmiHeader = { .biSize = sizeof(info.bmiHeader), .biWidth = WIDTH, .biHeight = -HEIGHT,
                                       .biPlanes = 1, .biBitCount = 32, .biCompression = BI_RGB } };
    BLENDFUNCTION blend = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    POINT pos = { 100, 100 }, src = { 0, 0 };
    SIZE size = { WIDTH, HEIGHT };
    DWORD *bits;
    HBITMAP dib;
    HDC mem;
    HWND hwnd;
    int i, p;

    cls.hInstance = GetModuleHandleW( NULL );
    if (!RegisterClassW( &cls )) return fail( "RegisterClass" );
    if (!(hwnd = CreateWindowExW( WS_EX_LAYERED | WS_EX_TOOLWINDOW, cls.lpszClassName, L"gw2 test", WS_POPUP,
                                  pos.x, pos.y, WIDTH, HEIGHT, NULL, NULL, cls.hInstance, NULL )))
        return fail( "CreateWindowEx" );
    if (!(mem = CreateCompatibleDC( NULL ))) return fail( "CreateCompatibleDC" );
    if (!(dib = CreateDIBSection( mem, &info, DIB_RGB_COLORS, (void **)&bits, NULL, 0 ))) return fail( "CreateDIBSection" );
    SelectObject( mem, dib );
    ShowWindow( hwnd, SW_SHOWNOACTIVATE );
    pump();

    for (i = 0; i < FRAMES; i++)
    {
        if (!(i % 3))  /* new pixels: an opaque gradient whose colour moves with the frame, premultiplied */
            for (p = 0; p < WIDTH * HEIGHT; p++)
                bits[p] = 0xff000000 | ((p + i) & 0xff) << 16 | ((p / WIDTH + i / 3) & 0xff) << 8 | (i & 0xff);
        GdiFlush();
        if (!UpdateLayeredWindow( hwnd, NULL, &pos, &size, mem, &src, 0, &blend, ULW_ALPHA ))
        {
            printf( "gw2 test layered: frame %d\n", i );
            return fail( "UpdateLayeredWindow" );
        }
        pump();
    }

    if (!DestroyWindow( hwnd )) return fail( "DestroyWindow" );
    DeleteDC( mem );
    DeleteObject( dib );
    printf( "gw2 test layered: passed\n" );
    return 0;
}
