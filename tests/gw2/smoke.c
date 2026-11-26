/* gw2 (jdrly/gw2-on-mac#40): the test stage's first program. Four workers wait on one semaphore
 * and take 10,000 single releases; an event wakes a waiter; a wait times out. It shows that the
 * engine with my ntdll.so starts a process, runs threads and waits, as the engine with its own does.
 *
 * Prints "gw2 test smoke: passed" and exits 0, or names what failed and exits 1. */
#include <stdio.h>
#include <windows.h>

#define WORKERS 4
#define JOBS 10000

static HANDLE sem;
static volatile LONG taken;

static DWORD WINAPI worker( void *arg )
{
    for (;;)
    {
        if (WaitForSingleObject( sem, INFINITE ) != WAIT_OBJECT_0) return 1;
        if (InterlockedIncrement( &taken ) > JOBS) return 0;
    }
}

static int fail( const char *what )
{
    printf( "gw2 test smoke: %s failed (error %lu)\n", what, GetLastError() );
    return 1;
}

int main( void )
{
    HANDLE threads[WORKERS], event;
    int i;

    if (!(sem = CreateSemaphoreW( NULL, 0, JOBS + WORKERS, NULL ))) return fail( "CreateSemaphore" );
    for (i = 0; i < WORKERS; i++)
        if (!(threads[i] = CreateThread( NULL, 0, worker, NULL, 0, NULL ))) return fail( "CreateThread" );
    for (i = 0; i < JOBS; i++)
        if (!ReleaseSemaphore( sem, 1, NULL )) return fail( "ReleaseSemaphore" );
    /* one more release per worker, so each sees a count above JOBS and returns */
    if (!ReleaseSemaphore( sem, WORKERS, NULL )) return fail( "the last ReleaseSemaphore" );
    if (WaitForMultipleObjects( WORKERS, threads, TRUE, 30000 ) != WAIT_OBJECT_0) return fail( "the workers' exit" );
    if (taken != JOBS + WORKERS) return fail( "taking every release" );

    if (!(event = CreateEventW( NULL, FALSE, FALSE, NULL ))) return fail( "CreateEvent" );
    if (WaitForSingleObject( event, 1 ) != WAIT_TIMEOUT) return fail( "a 1 ms wait on an unset event" );
    if (!SetEvent( event )) return fail( "SetEvent" );
    if (WaitForSingleObject( event, 0 ) != WAIT_OBJECT_0) return fail( "a wait on a set event" );

    printf( "gw2 test smoke: passed\n" );
    return 0;
}
