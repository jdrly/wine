/* gw2 (jdrly/gw2-on-mac#44): msync's wait-all rollback must leave alone a mutex the caller
 * already owned. 0.1.x's round 7b test (gw2-on-mac's docs/research/heat-r7-2026-11-04.md).
 *
 * A owns the mutex M and loops a 1 ms wait-all on {M, E}. B sets the auto-reset event E and
 * steals it back. When A's grab pass finds E gone, it puts back what it took. msync's rollback
 * also set M's owner to 0 although A owned M before the call, and C, which polls M with a 0
 * timeout, then takes it while A still holds it. C must never get M, and A's releases of M
 * must all succeed.
 *
 * The engine's own ntdll.so fails this (C took M after 36 to 474 wait-alls on 0.1.x's engine);
 * mine has to pass it. Runs for SECONDS, or until the first violation. Prints
 * "gw2 test mutexall: passed" and exits 0, or names what failed and exits 1. */
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>

#define SECONDS 20

static HANDLE mutex, event;
static volatile LONG stop, ready, taken_by_c, lost_by_a;
static volatile DWORD lost_error;
static volatile LONG64 wait_alls, got_all, steals, polls;

static DWORD WINAPI owner( void *arg )
{
    HANDLE objs[2] = { mutex, event };
    DWORD r;

    if (WaitForSingleObject( mutex, 0 ) != WAIT_OBJECT_0) return 1;
    InterlockedIncrement( &ready );
    while (!stop)
    {
        r = WaitForMultipleObjects( 2, objs, TRUE, 1 );
        InterlockedIncrement64( &wait_alls );
        if (r == WAIT_OBJECT_0)
        {
            InterlockedIncrement64( &got_all );
            /* the wait-all took M once more */
            if (!ReleaseMutex( mutex ))
            {
                lost_error = GetLastError();
                lost_by_a = stop = 1;
            }
        }
        else if (r != WAIT_TIMEOUT) return 2;
    }
    if (!ReleaseMutex( mutex ) && !lost_by_a)
    {
        lost_error = GetLastError();
        lost_by_a = 1;
    }
    return 0;
}

static DWORD WINAPI stealer( void *arg )
{
    while (ready < 1 && !stop) SwitchToThread();
    while (!stop)
    {
        if (!SetEvent( event )) return 1;
        if (WaitForSingleObject( event, 0 ) == WAIT_OBJECT_0) InterlockedIncrement64( &steals );
    }
    return 0;
}

static DWORD WINAPI poller( void *arg )
{
    while (ready < 1 && !stop) SwitchToThread();
    while (!stop)
    {
        if (WaitForSingleObject( mutex, 0 ) == WAIT_OBJECT_0)
        {
            InterlockedIncrement( &taken_by_c );
            stop = 1;
            ReleaseMutex( mutex );
        }
        InterlockedIncrement64( &polls );
    }
    return 0;
}

int main(void)
{
    HANDLE threads[3];
    DWORD code, t0;
    int i;

    setvbuf( stdout, NULL, _IONBF, 0 );
    printf( "gw2 test mutexall: A owns M and wait-alls {M, E}, B sets and steals E, C polls M\n" );
    if (!(mutex = CreateMutexW( NULL, FALSE, NULL ))) return 3;
    if (!(event = CreateEventW( NULL, FALSE, FALSE, NULL ))) return 3;
    threads[0] = CreateThread( NULL, 0, owner, NULL, 0, NULL );
    threads[1] = CreateThread( NULL, 0, stealer, NULL, 0, NULL );
    threads[2] = CreateThread( NULL, 0, poller, NULL, 0, NULL );
    for (i = 0; i < 3; i++) if (!threads[i]) return 3;

    for (t0 = GetTickCount(); !stop && GetTickCount() - t0 < SECONDS * 1000;) Sleep( 10 );
    stop = 1;
    for (i = 0; i < 3; i++)
    {
        if (WaitForSingleObject( threads[i], 10000 ) != WAIT_OBJECT_0)
        {
            printf( "gw2 test mutexall: failed: thread %d didn't end\n", i );
            return 1;
        }
        GetExitCodeThread( threads[i], &code );
        if (code)
        {
            printf( "gw2 test mutexall: failed: thread %d ended with %lu\n", i, code );
            return 1;
        }
    }

    printf( "gw2 test mutexall: %lld wait-alls (%lld got both), %lld steals, %lld polls in %lu ms\n",
            wait_alls, got_all, steals, polls, GetTickCount() - t0 );
    if (taken_by_c)
    {
        printf( "gw2 test mutexall: failed: another thread took the mutex its owner held, after %lld wait-alls\n",
                wait_alls );
        return 1;
    }
    if (lost_by_a)
    {
        printf( "gw2 test mutexall: failed: the owner's ReleaseMutex failed (error %lu)\n", lost_error );
        return 1;
    }
    if (!steals || !got_all)
    {
        printf( "gw2 test mutexall: failed: the race never ran (%lld steals, %lld wait-alls got both)\n", steals, got_all );
        return 1;
    }
    printf( "gw2 test mutexall: passed\n" );
    return 0;
}
