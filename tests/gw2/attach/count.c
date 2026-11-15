/* gw2 (jdrly/gw2-on-mac#174): the x86_64 process attach.exe attaches a debugger to.
 *
 *   count.exe <ready> <detached>
 *
 * <ready> and <detached> are handles of two events it inherits from attach.exe. It sets <ready>,
 * counts in steps of 10 ms until attach.exe has detached and set <detached>, then counts STEPS
 * more and exits 0. It never ends on a clock: if <detached> never comes, run.zsh's time limit ends
 * the run. */
#include <stdlib.h>
#include <windows.h>

#define STEPS 200

int main( int argc, char **argv )
{
    HANDLE ready, detached;
    unsigned int n;
    DWORD ret;

    if (argc != 3) return 1;
    ready = (HANDLE)(ULONG_PTR)strtoul( argv[1], NULL, 0 );
    detached = (HANDLE)(ULONG_PTR)strtoul( argv[2], NULL, 0 );
    if (!SetEvent( ready )) return 1;
    while ((ret = WaitForSingleObject( detached, 10 )) == WAIT_TIMEOUT) ;
    if (ret != WAIT_OBJECT_0) return 1;
    for (n = 0; n < STEPS; n++) Sleep( 10 );
    return 0;
}
