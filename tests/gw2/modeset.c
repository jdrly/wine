/* gw2 (jdrly/gw2-on-mac#159): display mode changes that the engine's win32u.so can't survive or
 * gets wrong, under emulated mode changes (EmulateModeset Y) at 192 DPI.
 *
 * The program writes EmulateModeset=Y (HKCU\Software\Wine\X11 Driver, where win32u reads it on
 * the Mac too) and LogPixels=192 (HKCU\Control Panel\Desktop) and starts itself again, since
 * win32u reads both when a process starts. The second process, system DPI aware:
 * 1. Asks for the registry's mode, which rebuilds the display cache with its settings. The modes
 *    are then win32u's virtual ones, and the display's own mode is the physical one.
 * 2. Lists the sizes offered and picks one whose ratio to the display at this DPI has a term of
 *    16 bits or more in lowest terms, as monitor_get_dpi() asks make_ratio() for it: 1366x768 when
 *    it is such a size, or else the first. The engine's make_ratio() asserts on that and the process ends.
 *    It changes to that size and checks that the mode is the current one. Without such a size it
 *    says so and goes on.
 * 3. Changes to a size with another aspect ratio than the display's, which is shown letterboxed:
 *    640x480 when it is one, or else the first. Clearing the cursor clip has to leave it on the mode, 0,0 to its size.
 *    The engine's win32u.so gives the server the whole display as the monitor's raw rect, so the
 *    cleared clip there takes in the bars.
 * 4. Goes back to the registry's mode and checks it is the one it started with.
 * The first process reports how the second ended. Nothing here waits on a clock.
 *
 * Prints "gw2 test modeset: passed" and exits 0, or names what failed and exits 1. Under the
 * engine's win32u.so it fails with "the mode change ended the process" in step 2, or in step 3
 * with "the cleared clip is …, not the mode's 0,0-…" when the display offers no size for step 2. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#define DPI 192
#define DONE 0x600d  /* the second process's exit code when every step passed */

#define FAIL(...) do { \
        printf( "gw2 test modeset: failed, line %d: ", __LINE__ ); \
        printf( __VA_ARGS__ ); \
        printf( "\n" ); \
        exit( 1 ); \
    } while (0)

static DWORD gcd( DWORD a, DWORD b )
{
    while (b)
    {
        DWORD t = a % b;
        a = b;
        b = t;
    }
    return a;
}

/* whether make_ratio( dpi * physical, mode ) has a term of 16 bits or more in lowest terms */
static BOOL too_big( DWORD dpi, DWORD physical, DWORD mode )
{
    DWORD num = dpi * physical, g = gcd( num, mode );
    return num / g > 0xffff || mode / g > 0xffff;
}

static void current( DEVMODEW *mode )
{
    memset( mode, 0, sizeof(*mode) );
    mode->dmSize = sizeof(*mode);
    if (!EnumDisplaySettingsW( NULL, ENUM_CURRENT_SETTINGS, mode )) FAIL( "EnumDisplaySettings: error %lu", GetLastError() );
}

static void change( const DEVMODEW *mode )
{
    DEVMODEW now, copy = *mode;
    LONG ret = ChangeDisplaySettingsExW( NULL, &copy, NULL, 0, NULL );

    if (ret != DISP_CHANGE_SUCCESSFUL) FAIL( "changing to %lux%lu returned %ld", mode->dmPelsWidth, mode->dmPelsHeight, ret );
    current( &now );
    if (now.dmPelsWidth != mode->dmPelsWidth || now.dmPelsHeight != mode->dmPelsHeight)
        FAIL( "changed to %lux%lu, but the current mode is %lux%lu", mode->dmPelsWidth, mode->dmPelsHeight,
              now.dmPelsWidth, now.dmPelsHeight );
}

static LONG WINAPI ended( EXCEPTION_POINTERS *ptrs )
{
    printf( "gw2 test modeset: exception %#lx at %p\n", ptrs->ExceptionRecord->ExceptionCode,
            ptrs->ExceptionRecord->ExceptionAddress );
    ExitProcess( 2 );
}

static int second( void )
{
    DEVMODEW start, mode, *modes = NULL, *pick = NULL, *letterbox = NULL;
    DWORD i, count = 0, dpi;
    char sizes[4096] = "";
    RECT clip;
    HDC dc;

    setvbuf( stdout, NULL, _IONBF, 0 );
    SetErrorMode( SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX );
    SetUnhandledExceptionFilter( ended );
    SetProcessDPIAware();
    dc = GetDC( NULL );
    dpi = GetDeviceCaps( dc, LOGPIXELSX );
    ReleaseDC( NULL, dc );
    if (dpi != DPI) FAIL( "the system DPI is %lu, not the %u this program set", dpi, DPI );

    /* 1. the display cache rebuilt with EmulateModeset Y */
    if (ChangeDisplaySettingsExW( NULL, NULL, NULL, 0, NULL ) != DISP_CHANGE_SUCCESSFUL)
        FAIL( "going to the registry's mode failed" );
    current( &start );
    printf( "gw2 test modeset: the display is %lux%lu at %lu bits, %lu DPI\n", start.dmPelsWidth,
            start.dmPelsHeight, start.dmBitsPerPel, dpi );

    /* 2. the sizes at the current depth, each once */
    for (i = 0;; i++)
    {
        memset( &mode, 0, sizeof(mode) );
        mode.dmSize = sizeof(mode);
        if (!EnumDisplaySettingsW( NULL, i, &mode )) break;
        if (mode.dmBitsPerPel != start.dmBitsPerPel) continue;
        if (!(modes = realloc( modes, (count + 1) * sizeof(*modes) ))) FAIL( "out of memory" );
        modes[count++] = mode;
    }
    for (i = 0; i < count; i++)
    {
        DEVMODEW *m = &modes[i];
        DWORD j;

        for (j = 0; j < i; j++)
            if (modes[j].dmPelsWidth == m->dmPelsWidth && modes[j].dmPelsHeight == m->dmPelsHeight) break;
        if (j < i) continue;
        snprintf( sizes + strlen( sizes ), sizeof(sizes) - strlen( sizes ), " %lux%lu", m->dmPelsWidth, m->dmPelsHeight );
        if ((too_big( dpi, start.dmPelsWidth, m->dmPelsWidth ) || too_big( dpi, start.dmPelsHeight, m->dmPelsHeight )) &&
            (!pick || (m->dmPelsWidth == 1366 && m->dmPelsHeight == 768)))
            pick = m;
        if (m->dmPelsWidth * start.dmPelsHeight != m->dmPelsHeight * start.dmPelsWidth &&
            (!letterbox || (m->dmPelsWidth == 640 && m->dmPelsHeight == 480)))
            letterbox = m;
    }
    printf( "gw2 test modeset: %lu modes at %lu bits, sizes%s\n", count, start.dmBitsPerPel, sizes );

    if (pick)
    {
        printf( "gw2 test modeset: changing to %lux%lu, whose ratio to %lux%lu at %lu DPI the engine's make_ratio can't hold\n",
                pick->dmPelsWidth, pick->dmPelsHeight, start.dmPelsWidth, start.dmPelsHeight, dpi );
        change( pick );
        printf( "gw2 test modeset: %lux%lu is the current mode, and the process is alive\n", pick->dmPelsWidth, pick->dmPelsHeight );
    }
    else printf( "gw2 test modeset: no size here whose ratio to %lux%lu at %lu DPI the engine's make_ratio can't hold\n",
                 start.dmPelsWidth, start.dmPelsHeight, dpi );

    /* 3. a letterboxed mode's cleared clip */
    if (!letterbox) FAIL( "no size with another aspect ratio than %lux%lu", start.dmPelsWidth, start.dmPelsHeight );
    change( letterbox );
    if (!ClipCursor( NULL )) FAIL( "ClipCursor: error %lu", GetLastError() );
    if (!GetClipCursor( &clip )) FAIL( "GetClipCursor: error %lu", GetLastError() );
    printf( "gw2 test modeset: at %lux%lu the cleared clip is %ld,%ld-%ld,%ld\n", letterbox->dmPelsWidth,
            letterbox->dmPelsHeight, clip.left, clip.top, clip.right, clip.bottom );
    if (clip.left || clip.top || clip.right != (LONG)letterbox->dmPelsWidth || clip.bottom != (LONG)letterbox->dmPelsHeight)
        FAIL( "the cleared clip is %ld,%ld-%ld,%ld, not the mode's 0,0-%lu,%lu", clip.left, clip.top, clip.right,
              clip.bottom, letterbox->dmPelsWidth, letterbox->dmPelsHeight );

    /* 4. back */
    if (ChangeDisplaySettingsExW( NULL, NULL, NULL, 0, NULL ) != DISP_CHANGE_SUCCESSFUL)
        FAIL( "going back to the registry's mode failed" );
    current( &mode );
    if (mode.dmPelsWidth != start.dmPelsWidth || mode.dmPelsHeight != start.dmPelsHeight)
        FAIL( "back at %lux%lu, not %lux%lu", mode.dmPelsWidth, mode.dmPelsHeight, start.dmPelsWidth, start.dmPelsHeight );
    printf( "gw2 test modeset: back at %lux%lu\n", mode.dmPelsWidth, mode.dmPelsHeight );
    free( modes );
    return DONE;
}

static void set( const WCHAR *key, const WCHAR *name, DWORD type, const void *data, DWORD size )
{
    LSTATUS ret = RegSetKeyValueW( HKEY_CURRENT_USER, key, name, type, data, size );
    if (ret) FAIL( "writing HKCU\\%ls\\%ls: error %ld", key, name, ret );
}

int main( int argc, char **argv )
{
    STARTUPINFOW si = { .cb = sizeof(si) };
    PROCESS_INFORMATION pi;
    WCHAR cmd[MAX_PATH + 16] = L"\"", path[MAX_PATH];
    DWORD dpi = DPI, code, len;

    if (argc > 1 && !strcmp( argv[1], "second" )) return second();

    setvbuf( stdout, NULL, _IONBF, 0 );
    set( L"Software\\Wine\\X11 Driver", L"EmulateModeset", REG_SZ, L"Y", sizeof(L"Y") );
    set( L"Control Panel\\Desktop", L"LogPixels", REG_DWORD, &dpi, sizeof(dpi) );

    if (!(len = GetModuleFileNameW( NULL, path, MAX_PATH )) || len == MAX_PATH)
        FAIL( "GetModuleFileName: error %lu", GetLastError() );
    wcscat( wcscat( cmd, path ), L"\" second" );
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle( STD_INPUT_HANDLE );
    si.hStdOutput = GetStdHandle( STD_OUTPUT_HANDLE );
    si.hStdError = GetStdHandle( STD_ERROR_HANDLE );
    if (!CreateProcessW( NULL, cmd, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi ))
        FAIL( "CreateProcess: error %lu", GetLastError() );
    WaitForSingleObject( pi.hProcess, INFINITE );
    if (!GetExitCodeProcess( pi.hProcess, &code )) FAIL( "GetExitCodeProcess: error %lu", GetLastError() );
    CloseHandle( pi.hThread );
    CloseHandle( pi.hProcess );
    if (code == 1) FAIL( "the second process failed" );
    if (code != DONE) FAIL( "the mode change ended the process (exit code %#lx)", code );
    printf( "gw2 test modeset: passed\n" );
    return 0;
}
