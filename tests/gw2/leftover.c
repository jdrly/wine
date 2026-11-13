/* gw2 (jdrly/gw2-on-mac#128): a program that leaves a process behind, for run.zsh's check after
 * a run. It starts "perl -e sleep gw2-test-leftover", which sleeps until it is killed, through
 * ntdll's __wine_unix_spawnvp, and exits. The wineserver doesn't know that process, so stopping it
 * leaves the process running. env takes GW2_TEST_RUN out of its environment first, so run.zsh
 * can't find it by that variable, as it can't find Wine's own processes by it on GitHub's runner.
 * run.zsh has to fail the run and name the process; the workflow's step checks it is gone.
 * matrix.zsh doesn't run it, since it fails its run on purpose.
 *
 * Prints "gw2 test leftover: passed" and exits 0 once the process started, or names what failed
 * and exits 1. */
#include <stdio.h>
#include <windows.h>

static LONG (WINAPI *p__wine_unix_spawnvp)( char * const argv[], int wait );

int main( void )
{
    char *argv[] = { "/usr/bin/env", "-u", "GW2_TEST_RUN", "/usr/bin/perl", "-e", "sleep", "gw2-test-leftover", NULL };
    LONG status;

    if (!(p__wine_unix_spawnvp = (void *)GetProcAddress( GetModuleHandleA( "ntdll.dll" ), "__wine_unix_spawnvp" )))
    {
        printf( "gw2 test leftover: ntdll.dll has no __wine_unix_spawnvp\n" );
        return 1;
    }
    /* wait 0: it returns once the process started, which Wine runs from a fork of its own */
    if ((status = p__wine_unix_spawnvp( argv, 0 )))
    {
        printf( "gw2 test leftover: __wine_unix_spawnvp failed (status %#lx)\n", (unsigned long)status );
        return 1;
    }
    printf( "gw2 test leftover: passed\n" );
    return 0;
}
