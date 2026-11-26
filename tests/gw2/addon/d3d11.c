/* gw2 (jdrly/gw2-on-mac#155): a stand-in for arcdps's d3d11.dll, for rundll.exe. Like arcdps it
 * exports c_exceptionerrormsg, which arcdps's crash handler has rundll32 call for its crash window,
 * and its version block (d3d11.rc) has no CompanyName. The entry point writes "ran" into the file
 * its command line names. */
#include <windows.h>

__declspec(dllexport) void CALLBACK c_exceptionerrormsg( HWND hwnd, HINSTANCE instance, LPSTR cmdline, int show )
{
    HANDLE file = CreateFileA( cmdline, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL );
    DWORD written;

    if (file == INVALID_HANDLE_VALUE) return;
    WriteFile( file, "ran", 3, &written, NULL );
    CloseHandle( file );
}
