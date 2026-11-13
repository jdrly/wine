/* gw2 (jdrly/gw2-on-mac#155): what arcdps's crash handler does in the game. An x64 program starts
 * "rundll32.exe <dir>\addon\d3d11.dll,c_exceptionerrormsg <file>", where addon\d3d11.dll (next to
 * this program, built from tests/gw2/addon) is a stand-in for arcdps. The entry point writes "ran"
 * into <file>.
 *
 * With Wine's default load order, rundll32 loads Wine's own d3d11 in place of that file, finds no
 * c_exceptionerrormsg and returns, and this program fails. The app's compatdb rule
 * exe=rundll32.exe;dll_overrides=d3d11,dxgi=n,b, in WINE_COMPATDB_FILE, makes rundll32 load the
 * file, and the program passes. compatdb.so reads that variable from the Unix environment, which a
 * Windows program can't change for its child, so the workflow runs it once each way. matrix.zsh
 * doesn't run it.
 *
 * Prints "gw2 test rundll: passed" and exits 0, or names what failed and exits 1. */
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include <windows.h>

static int fail( const char *what )
{
    printf( "gw2 test rundll: %s failed (error %lu)\n", what, GetLastError() );
    return 1;
}

int main( void )
{
    WCHAR dll[MAX_PATH], file[MAX_PATH], cmdline[3 * MAX_PATH], *slash;
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    DWORD len, code, got = 0;
    char text[8] = {0};
    HANDLE marker;

    len = GetModuleFileNameW( NULL, dll, MAX_PATH - 32 );
    if (!len || len >= MAX_PATH - 32) return fail( "GetModuleFileName" );
    if (!(slash = wcsrchr( dll, '\\' ))) return fail( "finding the program's folder" );
    lstrcpyW( slash + 1, L"addon\\d3d11.dll" );
    if (GetFileAttributesW( dll ) == INVALID_FILE_ATTRIBUTES) return fail( "finding addon\\d3d11.dll" );

    if (!GetTempPathW( MAX_PATH - 32, file )) return fail( "GetTempPath" );
    lstrcatW( file, L"gw2-test-rundll.txt" );
    DeleteFileW( file );

    lstrcpyW( cmdline, L"rundll32.exe \"" );
    lstrcatW( cmdline, dll );
    lstrcatW( cmdline, L"\",c_exceptionerrormsg " );
    lstrcatW( cmdline, file );
    printf( "gw2 test rundll: %ls\n", cmdline );
    if (!CreateProcessW( NULL, cmdline, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi )) return fail( "CreateProcess" );
    if (WaitForSingleObject( pi.hProcess, INFINITE ) != WAIT_OBJECT_0) return fail( "waiting for rundll32" );
    if (!GetExitCodeProcess( pi.hProcess, &code )) return fail( "GetExitCodeProcess" );
    CloseHandle( pi.hThread );
    CloseHandle( pi.hProcess );

    marker = CreateFileW( file, GENERIC_READ, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL );
    if (marker == INVALID_HANDLE_VALUE)
    {
        printf( "gw2 test rundll: the entry point didn't run (rundll32 exited %lu)\n", code );
        return 1;
    }
    ReadFile( marker, text, sizeof(text) - 1, &got, NULL );
    CloseHandle( marker );
    DeleteFileW( file );
    if (strcmp( text, "ran" ))
    {
        printf( "gw2 test rundll: the entry point's file holds \"%s\", not \"ran\"\n", text );
        return 1;
    }
    printf( "gw2 test rundll: passed\n" );
    return 0;
}
