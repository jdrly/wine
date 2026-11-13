/* gw2 (jdrly/gw2-on-mac#128): a program that leaves a process behind, for run.zsh's check after
 * a run. Through ntdll's __wine_unix_spawnvp it starts env, which takes GW2_TEST_RUN out of the
 * environment and execs "perl -e <script> <ready file> gw2-test-leftover". Perl creates the ready
 * file and sleeps until it is killed. The program waits for that file, so the process it leaves
 * is perl without the variable, and exits. run.zsh can't find that process by the variable, as it
 * can't find Wine's own processes by it on GitHub's runner, and the wineserver doesn't know it, so
 * stopping the wineserver leaves it running. run.zsh has to fail the run and name the process; the
 * workflow's step checks it is gone. matrix.zsh doesn't run it, since it fails its run on purpose.
 *
 * Prints "gw2 test leftover: passed" and exits 0 once perl runs, or names what failed and exits 1.
 * If perl never creates the file, run.zsh's time limit ends the program. */
#include <stdio.h>
#include <windows.h>

static LONG (WINAPI *p__wine_unix_spawnvp)( char * const argv[], int wait );
static char * (CDECL *pwine_get_unix_file_name)( const WCHAR *path );

int main( void )
{
    WCHAR ready[MAX_PATH];
    char *argv[] = { "/usr/bin/env", "-u", "GW2_TEST_RUN", "/usr/bin/perl", "-e",
                     "open(my $f, '>', shift) or die \"$!\\n\"; close $f; sleep", NULL, "gw2-test-leftover", NULL };
    LONG status;

    if (!(p__wine_unix_spawnvp = (void *)GetProcAddress( GetModuleHandleA( "ntdll.dll" ), "__wine_unix_spawnvp" )))
    {
        printf( "gw2 test leftover: ntdll.dll has no __wine_unix_spawnvp\n" );
        return 1;
    }
    if (!(pwine_get_unix_file_name = (void *)GetProcAddress( GetModuleHandleA( "kernel32.dll" ), "wine_get_unix_file_name" )))
    {
        printf( "gw2 test leftover: kernel32.dll has no wine_get_unix_file_name\n" );
        return 1;
    }
    if (!GetTempPathW( MAX_PATH - 32, ready ))
    {
        printf( "gw2 test leftover: GetTempPath failed (error %lu)\n", GetLastError() );
        return 1;
    }
    lstrcatW( ready, L"gw2-test-leftover.ready" );
    DeleteFileW( ready );
    if (!(argv[6] = pwine_get_unix_file_name( ready )))
    {
        printf( "gw2 test leftover: no Unix path for the ready file\n" );
        return 1;
    }
    /* wait 0: it returns once env runs, from a fork of Wine's own */
    if ((status = p__wine_unix_spawnvp( argv, 0 )))
    {
        printf( "gw2 test leftover: __wine_unix_spawnvp failed (status %#lx)\n", (unsigned long)status );
        return 1;
    }
    while (GetFileAttributesW( ready ) == INVALID_FILE_ATTRIBUTES) Sleep( 10 );
    printf( "gw2 test leftover: passed\n" );
    return 0;
}
