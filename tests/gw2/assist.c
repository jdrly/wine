/* gw2 (jdrly/gw2-on-mac#88): a window the pointer goes onto and off, for the Mac driver's Cursor
 * Location Assistance. With GW2_TEST_ASSIST=1 and the Mac driver it shows a window and prints
 * "gw2 test assist: ready <on-x> <on-y> <off-x> <off-y>": the middle of its client area and a point
 * 100 pixels left of the window, in screen coordinates. Then it takes its messages until a right
 * click reaches the window. tests/gw2/native/pointer.c moves and clicks the pointer, and the
 * workflow counts the driver's lines. Without GW2_TEST_ASSIST, or without the Mac driver, it says
 * so and passes: the matrix runs every program, and nothing moves the pointer there.
 *
 * Prints "gw2 test assist: passed" and exits 0, or names what failed and exits 1, also when no
 * right click came within 120 seconds of "ready": the workflow waits up to 5 s for each of its 21
 * moves. */
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>

static int fail( const char *what )
{
    printf( "gw2 test assist: %s failed (error %lu)\n", what, GetLastError() );
    return 1;
}

int main( void )
{
    WNDCLASSW cls = { .lpfnWndProc = DefWindowProcW, .lpszClassName = L"gw2 test assist" };
    POINT on;
    unsigned int moves = 0;
    BOOL clicked = FALSE;
    DWORD start;
    HWND hwnd;
    RECT rect;
    MSG msg;

    setvbuf( stdout, NULL, _IONBF, 0 );
    if (!getenv( "GW2_TEST_ASSIST" ))
    {
        printf( "gw2 test assist: GW2_TEST_ASSIST is unset, nothing moves the pointer\n" );
        printf( "gw2 test assist: passed\n" );
        return 0;
    }
    cls.hInstance = GetModuleHandleW( NULL );
    if (!RegisterClassW( &cls )) return fail( "RegisterClass" );
    hwnd = CreateWindowExW( 0, cls.lpszClassName, L"gw2 test", WS_OVERLAPPEDWINDOW, 400, 200, 320, 240,
                            NULL, NULL, cls.hInstance, NULL );
    if (!GetModuleHandleW( L"winemac.drv" ))
    {
        printf( "gw2 test assist: no Mac driver, nothing to count\n" );
        if (hwnd) DestroyWindow( hwnd );
        printf( "gw2 test assist: passed\n" );
        return 0;
    }
    if (!hwnd) return fail( "CreateWindowEx" );
    ShowWindow( hwnd, SW_SHOWNORMAL );
    if (!UpdateWindow( hwnd )) return fail( "UpdateWindow" );
    if (!GetClientRect( hwnd, &rect )) return fail( "GetClientRect" );
    on.x = rect.right / 2;
    on.y = rect.bottom / 2;
    if (!ClientToScreen( hwnd, &on )) return fail( "ClientToScreen" );
    if (!GetWindowRect( hwnd, &rect )) return fail( "GetWindowRect" );
    printf( "gw2 test assist: ready %ld %ld %ld %ld\n", on.x, on.y, rect.left - 100, on.y );

    for (start = GetTickCount(); !clicked && GetTickCount() - start < 120000;)
    {
        while (PeekMessageW( &msg, NULL, 0, 0, PM_REMOVE ))
        {
            if (msg.message == WM_MOUSEMOVE) moves++;
            if (msg.message == WM_RBUTTONDOWN) clicked = TRUE;
            TranslateMessage( &msg );
            DispatchMessageW( &msg );
        }
        Sleep( 10 );
    }
    printf( "gw2 test assist: %u WM_MOUSEMOVE\n", moves );
    if (!clicked)
    {
        printf( "gw2 test assist: no right click within 120 s\n" );
        return 1;
    }
    if (!DestroyWindow( hwnd )) return fail( "DestroyWindow" );
    printf( "gw2 test assist: passed\n" );
    return 0;
}
