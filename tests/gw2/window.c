/* gw2 (jdrly/gw2-on-mac#57): does the Mac driver start on this machine? The smallest program that
 * needs a graphics driver: it makes a window, shows it, takes its messages for a quarter second
 * and destroys it. win32u loads the driver for the first window. winemac.drv's DllMain fails when
 * the unix side's macdrv_init does, which it does without a graphics session
 * (dlls/winemac.drv/macdrv_main.c), and win32u then takes its null driver, which refuses a
 * top-level window (nodrv_CreateWindow). So after the first CreateWindowEx, winemac.drv is loaded
 * in this process only if the Mac driver started.
 *
 * Prints whether it did. Without the driver that is all, and it passes. With it, the window has to
 * be made, shown and destroyed. Prints "gw2 test window: passed", or names what failed and exits 1.
 * The answer decides whether the Mac driver's window tests run on GitHub's runner or on my Mac. */
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
    hwnd = CreateWindowExW( 0, cls.lpszClassName, L"gw2 test", WS_OVERLAPPEDWINDOW, 100, 100, 320, 240,
                            NULL, NULL, cls.hInstance, NULL );
    if (!GetModuleHandleW( L"winemac.drv" ))
    {
        printf( "gw2 test window: no Mac driver, the window was %s\n", hwnd ? "made by the null driver" : "refused" );
        if (hwnd) DestroyWindow( hwnd );
        printf( "gw2 test window: passed\n" );
        return 0;
    }
    printf( "gw2 test window: the Mac driver started\n" );
    if (!hwnd) return fail( "CreateWindowEx" );
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
    if (!DestroyWindow( hwnd )) return fail( "DestroyWindow" );
    printf( "gw2 test window: passed\n" );
    return 0;
}
