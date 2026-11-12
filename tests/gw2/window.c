/* gw2 (jdrly/gw2-on-mac#57): does the Mac driver start on this machine? The smallest program that
 * needs a graphics driver: it makes a window, shows it, takes its messages for a quarter second
 * and destroys it. win32u loads the driver for the first window. winemac.drv's DllMain fails when
 * the unix side's macdrv_init does, which it does without a graphics session
 * (dlls/winemac.drv/macdrv_main.c), and win32u then takes its null driver. So winemac.drv is
 * loaded in this process after the window exists only if the Mac driver started.
 *
 * Prints whether it did, and "gw2 test window: passed" when the window could be made, shown and
 * destroyed either way; names what failed and exits 1 otherwise. The answer decides whether the
 * Mac driver's window tests run on GitHub's runner or on my Mac. */
#include <stdio.h>
#include <windows.h>

static int fail( const char *what )
{
    printf( "gw2 test window: %s failed (error %lu)\n", what, GetLastError() );
    return 1;
}

int main( void )
{
    WNDCLASSW cls = { .lpfnWndProc = DefWindowProcW, .lpszClassName = L"gw2 test window" };
    DWORD start;
    HWND hwnd;
    MSG msg;

    cls.hInstance = GetModuleHandleW( NULL );
    if (!RegisterClassW( &cls )) return fail( "RegisterClass" );
    if (!(hwnd = CreateWindowExW( 0, cls.lpszClassName, L"gw2 test", WS_OVERLAPPEDWINDOW, 100, 100, 320, 240,
                                  NULL, NULL, cls.hInstance, NULL )))
        return fail( "CreateWindowEx" );
    ShowWindow( hwnd, SW_SHOWNORMAL );
    if (!UpdateWindow( hwnd )) return fail( "UpdateWindow" );
    for (start = GetTickCount(); GetTickCount() - start < 250;)
    {
        while (PeekMessageW( &msg, NULL, 0, 0, PM_REMOVE ))
        {
            TranslateMessage( &msg );
            DispatchMessageW( &msg );
        }
        Sleep( 10 );
    }
    printf( "gw2 test window: %s\n", GetModuleHandleW( L"winemac.drv" ) ? "the Mac driver started"
                                                                      : "no Mac driver, the null driver drew" );
    if (!DestroyWindow( hwnd )) return fail( "DestroyWindow" );
    printf( "gw2 test window: passed\n" );
    return 0;
}
