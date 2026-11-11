/* gw2 (jdrly/gw2-on-mac#42): the event pool's tests. They pass with WINE_HEAT_EVPOOL off, 1 and
 * 1,mask, and under the engine's own ntdll.so.
 *
 *   storm       20,000 create, check, set, wait, close rounds on each of 4 threads, auto-reset
 *               and manual-reset, while another thread suspends and resumes those threads
 *               without pause (each suspend is a signal on that thread). No handle is handed to
 *               two threads at once, every new event has the state it was created with, a set is
 *               seen once by an auto-reset event and stays on a manual-reset one.
 *   dup         An event duplicated with the pseudo-handle, with a real handle to this process,
 *               and with DUPLICATE_CLOSE_SOURCE, then closed; 1,100 new events of its kind
 *               (more than a pool holds) are each set: the duplicate stays unsignaled.
 *   read        An event given to an overlapped read of a pipe, closed before the read
 *               completes; 1,100 new events, then the write: none of them is signaled.
 *   select      The same with WSAEventSelect on a socket and the data that signals it.
 *   twice       A second close of an event fails with ERROR_INVALID_HANDLE, and so does a
 *               duplicate of it.
 *   counters    With WINE_HEAT_COUNT=1 and WINE_HEAT_EXE naming this program: the log's mode
 *               names the pool exactly when WINE_HEAT_EVPOOL is on. With the pool on, a thread
 *               that makes 1,000 create, set, wait, close rounds on parked events has 1,000
 *               evpool_hit and 1,000 evpool_put, and its few server calls are its own start
 *               and end (at most 50), not three for each round; the process counted the double
 *               closes.
 *
 * Each case prints its name first; a stall shows as the run's time limit with the last name
 * printed. Prints "gw2 test evpool: passed" and exits 0, or names what failed and exits 1. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <winsock2.h>
#include <windows.h>

#define WORKERS 4
#define ROUNDS 20000
#define MORE_THAN_A_POOL 1100
#define HITS 1000

static const char *current = "startup";

#define FAIL(...) do { \
        printf( "gw2 test evpool: %s failed, line %d: ", current, __LINE__ ); \
        printf( __VA_ARGS__ ); \
        printf( "\n" ); \
        fflush( stdout ); \
        exit( 1 ); \
    } while (0)

static void start( const char *name )
{
    current = name;
    printf( "gw2 test evpool: %s\n", name );
    fflush( stdout );
}

static HANDLE thread( LPTHREAD_START_ROUTINE func, void *arg )
{
    HANDLE h = CreateThread( NULL, 0, func, arg, 0, NULL );
    if (!h) FAIL( "CreateThread: error %lu", GetLastError() );
    return h;
}

static void join( HANDLE h, DWORD ms )
{
    DWORD code;
    if (WaitForSingleObject( h, ms ) != WAIT_OBJECT_0) FAIL( "a thread didn't end within %lu ms", ms );
    if (!GetExitCodeThread( h, &code ) || code) FAIL( "a thread ended with %lu", code );
    CloseHandle( h );
}

static HANDLE event( BOOL manual, BOOL state )
{
    HANDLE h = CreateEventW( NULL, manual, state, NULL );
    if (!h) FAIL( "CreateEvent: error %lu", GetLastError() );
    return h;
}

static BOOL signaled( HANDLE h )
{
    DWORD ret = WaitForSingleObject( h, 0 );
    if (ret != WAIT_OBJECT_0 && ret != WAIT_TIMEOUT) FAIL( "WaitForSingleObject: %lu, error %lu", ret, GetLastError() );
    return ret == WAIT_OBJECT_0;
}

/* storm */

static volatile LONG owner[1 << 20];  /* by handle value / 4: the thread that holds it */
static volatile LONG storm_done;

static DWORD WINAPI rounds( void *arg )
{
    LONG self = GetCurrentThreadId();
    volatile LONG *slot;
    BOOL manual, state;
    HANDLE h;
    int i;

    for (i = 0; i < ROUNDS; i++)
    {
        manual = i & 1;
        state = (i >> 1) & 1;
        h = event( manual, state );
        slot = (ULONG_PTR)h / 4 < ARRAYSIZE(owner) ? &owner[(ULONG_PTR)h / 4] : NULL;
        if (slot && InterlockedCompareExchange( slot, self, 0 ))
            FAIL( "round %d: handle %p is held by thread %04lx too", i, h, *slot );
        if (signaled( h ) != state) FAIL( "round %d: a new event created %s isn't", i, state ? "signaled" : "unsignaled" );
        if (!SetEvent( h )) FAIL( "SetEvent: error %lu", GetLastError() );
        if (!signaled( h )) FAIL( "round %d: a set was lost", i );
        if (signaled( h ) != manual)
            FAIL( "round %d: %s", i, manual ? "a manual-reset event reset itself" : "an auto-reset event released two waits" );
        if (slot) InterlockedExchange( slot, 0 );
        if (!CloseHandle( h )) FAIL( "round %d: CloseHandle: error %lu", i, GetLastError() );
    }
    return 0;
}

static DWORD WINAPI storm( void *arg )
{
    HANDLE *workers = arg;
    LONG n = 0;
    int i;

    while (!storm_done)
    {
        for (i = 0; i < WORKERS; i++)
        {
            /* a worker that ended can't be suspended any more; one that stayed suspended
             * shows as a deadlock */
            if (SuspendThread( workers[i] ) == (DWORD)-1) continue;
            ResumeThread( workers[i] );
            n++;
        }
    }
    return n ? 0 : 2;
}

static void test_storm(void)
{
    HANDLE workers[WORKERS], storm_thread;
    DWORD t0 = GetTickCount();
    int i;

    start( "storm" );
    for (i = 0; i < WORKERS; i++) workers[i] = thread( rounds, NULL );
    storm_thread = thread( storm, workers );
    for (i = 0; i < WORKERS; i++)
        if (WaitForSingleObject( workers[i], 240000 ) != WAIT_OBJECT_0) FAIL( "worker %d didn't end: a deadlock", i );
    storm_done = 1;
    join( storm_thread, 10000 );
    for (i = 0; i < WORKERS; i++) join( workers[i], 0 );
    printf( "gw2 test evpool: %d rounds on %d threads in %lu ms\n", ROUNDS, WORKERS, GetTickCount() - t0 );
}

/* dup, read, select: <old> is closed but something else still holds its object; none of
 * MORE_THAN_A_POOL new events of its kind may be that object. <check> NULL: set each new event
 * and see that <old> stays unsignaled. Otherwise call it once they all exist, then see that none
 * of them was signaled. */

static void none_is( HANDLE old, BOOL manual, void (*check)(void) )
{
    static HANDLE made[MORE_THAN_A_POOL];
    int i;

    Sleep( 10 );  /* older than the pool's 2 ms */
    for (i = 0; i < MORE_THAN_A_POOL; i++)
    {
        made[i] = event( manual, FALSE );
        if (check) continue;
        if (!SetEvent( made[i] )) FAIL( "SetEvent: error %lu", GetLastError() );
        if (signaled( old )) FAIL( "new event %d is the old one", i );
    }
    if (check)
    {
        check();
        for (i = 0; i < MORE_THAN_A_POOL; i++) if (signaled( made[i] )) FAIL( "new event %d was signaled", i );
    }
    for (i = 0; i < MORE_THAN_A_POOL; i++) CloseHandle( made[i] );
}

static void test_dup(void)
{
    HANDLE h, dup, self;

    start( "dup with the pseudo-handle" );
    h = event( TRUE, FALSE );
    if (!DuplicateHandle( GetCurrentProcess(), h, GetCurrentProcess(), &dup, 0, FALSE, DUPLICATE_SAME_ACCESS ))
        FAIL( "DuplicateHandle: error %lu", GetLastError() );
    CloseHandle( h );
    none_is( dup, TRUE, NULL );
    CloseHandle( dup );

    start( "dup with a real handle to this process" );
    if (!(self = OpenProcess( PROCESS_DUP_HANDLE, FALSE, GetCurrentProcessId() )))
        FAIL( "OpenProcess: error %lu", GetLastError() );
    h = event( FALSE, FALSE );
    if (!DuplicateHandle( self, h, GetCurrentProcess(), &dup, 0, FALSE, DUPLICATE_SAME_ACCESS ))
        FAIL( "DuplicateHandle: error %lu", GetLastError() );
    CloseHandle( h );
    none_is( dup, FALSE, NULL );
    CloseHandle( dup );

    start( "dup closing the source" );
    h = event( TRUE, FALSE );
    if (!DuplicateHandle( self, h, GetCurrentProcess(), &dup, 0, FALSE, DUPLICATE_SAME_ACCESS | DUPLICATE_CLOSE_SOURCE ))
        FAIL( "DuplicateHandle: error %lu", GetLastError() );
    none_is( dup, TRUE, NULL );
    CloseHandle( dup );
    CloseHandle( self );
}

/* read */

static HANDLE pipe_client;

static void write_pipe(void)
{
    DWORD written;
    if (!WriteFile( pipe_client, "x", 1, &written, NULL ) || written != 1) FAIL( "WriteFile: error %lu", GetLastError() );
}

static void test_read(void)
{
    WCHAR name[64];
    OVERLAPPED ov = {0};
    HANDLE server;
    char buf[16];
    DWORD t0;

    start( "overlapped read" );
    swprintf( name, ARRAYSIZE(name), L"\\\\.\\pipe\\gw2-evpool-%lu", GetCurrentProcessId() );
    server = CreateNamedPipeW( name, PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED, PIPE_TYPE_BYTE | PIPE_WAIT, 1,
                               4096, 4096, 0, NULL );
    if (server == INVALID_HANDLE_VALUE) FAIL( "CreateNamedPipe: error %lu", GetLastError() );
    pipe_client = CreateFileW( name, GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL );
    if (pipe_client == INVALID_HANDLE_VALUE) FAIL( "CreateFile: error %lu", GetLastError() );

    ov.hEvent = event( TRUE, FALSE );
    if (ReadFile( server, buf, sizeof(buf), NULL, &ov ) || GetLastError() != ERROR_IO_PENDING)
        FAIL( "the read didn't stay pending: error %lu", GetLastError() );
    CloseHandle( ov.hEvent );
    none_is( NULL, TRUE, write_pipe );
    for (t0 = GetTickCount(); !HasOverlappedIoCompleted( &ov ); Sleep( 1 ))
        if (GetTickCount() - t0 > 5000) FAIL( "the read didn't complete" );
    CloseHandle( pipe_client );
    CloseHandle( server );
}

/* select, with ws2_32 loaded at run time: the tests are built without import libraries */

static int (WINAPI *p_WSAStartup)( WORD, WSADATA * );
static SOCKET (WINAPI *p_socket)( int, int, int );
static int (WINAPI *p_bind)( SOCKET, const struct sockaddr *, int );
static int (WINAPI *p_listen)( SOCKET, int );
static int (WINAPI *p_getsockname)( SOCKET, struct sockaddr *, int * );
static int (WINAPI *p_connect)( SOCKET, const struct sockaddr *, int );
static SOCKET (WINAPI *p_accept)( SOCKET, struct sockaddr *, int * );
static int (WINAPI *p_send)( SOCKET, const char *, int, int );
static int (WINAPI *p_WSAEventSelect)( SOCKET, HANDLE, LONG );
static int (WINAPI *p_closesocket)( SOCKET );
static int (WINAPI *p_WSAGetLastError)( void );

static SOCKET sock_client;

static void send_byte(void)
{
    if (p_send( sock_client, "x", 1, 0 ) != 1) FAIL( "send: error %d", p_WSAGetLastError() );
    Sleep( 200 );  /* the server signals the selected event */
}

static void test_select(void)
{
    struct sockaddr_in addr = { AF_INET };
    HMODULE ws2 = LoadLibraryA( "ws2_32.dll" );
    SOCKET listener, accepted;
    WSADATA data;
    HANDLE ev;
    int len = sizeof(addr);

    start( "WSAEventSelect" );
    if (!ws2) FAIL( "LoadLibrary: error %lu", GetLastError() );
#define LOAD(f) if (!(*(FARPROC *)&p_##f = GetProcAddress( ws2, #f ))) FAIL( "no %s", #f )
    LOAD(WSAStartup); LOAD(socket); LOAD(bind); LOAD(listen); LOAD(getsockname); LOAD(connect);
    LOAD(accept); LOAD(send); LOAD(WSAEventSelect); LOAD(closesocket); LOAD(WSAGetLastError);
#undef LOAD
    if (p_WSAStartup( MAKEWORD( 2, 2 ), &data )) FAIL( "WSAStartup failed" );
    addr.sin_addr.s_addr = 0x0100007f;  /* 127.0.0.1; htonl is ws2_32's too */
    if ((listener = p_socket( AF_INET, SOCK_STREAM, 0 )) == INVALID_SOCKET) FAIL( "socket: error %d", p_WSAGetLastError() );
    if (p_bind( listener, (struct sockaddr *)&addr, sizeof(addr) ) || p_listen( listener, 1 ) ||
        p_getsockname( listener, (struct sockaddr *)&addr, &len ))
        FAIL( "listen: error %d", p_WSAGetLastError() );
    if ((sock_client = p_socket( AF_INET, SOCK_STREAM, 0 )) == INVALID_SOCKET) FAIL( "socket: error %d", p_WSAGetLastError() );
    if (p_connect( sock_client, (struct sockaddr *)&addr, sizeof(addr) )) FAIL( "connect: error %d", p_WSAGetLastError() );
    if ((accepted = p_accept( listener, NULL, NULL )) == INVALID_SOCKET) FAIL( "accept: error %d", p_WSAGetLastError() );

    ev = event( TRUE, FALSE );
    if (p_WSAEventSelect( accepted, ev, FD_READ )) FAIL( "WSAEventSelect: error %d", p_WSAGetLastError() );
    CloseHandle( ev );
    none_is( NULL, TRUE, send_byte );
    p_closesocket( accepted );
    p_closesocket( sock_client );
    p_closesocket( listener );
}

/* twice */

static void test_twice(void)
{
    HANDLE h, dup;

    start( "double close" );
    h = event( FALSE, FALSE );
    if (!CloseHandle( h )) FAIL( "CloseHandle: error %lu", GetLastError() );
    SetLastError( 0 );
    if (CloseHandle( h ) || GetLastError() != ERROR_INVALID_HANDLE)
        FAIL( "the second close: %lu", GetLastError() );
    SetLastError( 0 );
    if (DuplicateHandle( GetCurrentProcess(), h, GetCurrentProcess(), &dup, 0, FALSE, DUPLICATE_SAME_ACCESS ) ||
        GetLastError() != ERROR_INVALID_HANDLE)
        FAIL( "a duplicate of the closed handle: %lu", GetLastError() );
}

/* counters */

static BOOL env_is( const char *name, const char *value )
{
    char buf[256];
    DWORD n = GetEnvironmentVariableA( name, buf, sizeof(buf) );
    return n && n < sizeof(buf) && !_stricmp( buf, value );
}

/* counter <name> on a line of heat.c's log, -1 if it isn't there */
static long long counter( const char *line, const char *name )
{
    char key[64];
    const char *p;
    snprintf( key, sizeof(key), " %s ", name );
    return (p = strstr( line, key )) ? _atoi64( p + strlen( key ) ) : -1;
}

static DWORD WINAPI hits( void *arg )
{
    HANDLE h;
    int i;

    for (i = 0; i < HITS; i++)
    {
        h = event( FALSE, FALSE );
        if (!SetEvent( h ) || !signaled( h )) FAIL( "round %d: a set was lost", i );
        CloseHandle( h );
    }
    return 0;
}

static void test_counters(void)
{
    static HANDLE parked[HITS];
    char dir[MAX_PATH], path[MAX_PATH * 2], want[32], line[4096], head[512] = "", *mode, *end;
    long long hit = -1, put = -1, server = -1, twice = -1;
    BOOL pool, found = FALSE, mine;
    WIN32_FIND_DATAA fd;
    HANDLE t, find;
    DWORD tid;
    FILE *f;
    int i;

    start( "counters" );
    pool = env_is( "WINE_HEAT_EVPOOL", "1" ) || env_is( "WINE_HEAT_EVPOOL", "1,mask" );
    /* HITS events parked, each looked up once, so their next set needs no request */
    for (i = 0; i < HITS; i++) SetEvent( parked[i] = event( FALSE, FALSE ) );
    for (i = 0; i < HITS; i++) CloseHandle( parked[i] );
    Sleep( 10 );
    t = thread( hits, NULL );
    tid = GetThreadId( t );
    join( t, 60000 );

    if (!env_is( "WINE_HEAT_COUNT", "1" ) || !env_is( "WINE_HEAT_EXE", "evpool.exe" )) return;
    if (!GetEnvironmentVariableA( "WINE_HEAT_LOG", dir, sizeof(dir) )) FAIL( "WINE_HEAT_LOG isn't set" );
    Sleep( 6000 );  /* the counters are written every 5 s */

    snprintf( want, sizeof(want), "thread %04lx ", tid );
    snprintf( path, sizeof(path), "Z:%s/heat-*.log", dir );
    if ((find = FindFirstFileA( path, &fd )) == INVALID_HANDLE_VALUE) FAIL( "no heat-*.log in %s", dir );
    do
    {
        snprintf( path, sizeof(path), "Z:%s/%s", dir, fd.cFileName );
        if (!(f = fopen( path, "r" ))) continue;
        mine = FALSE;
        while (fgets( line, sizeof(line), f ))
        {
            if (!strncmp( line, "heat pid ", 9 )) snprintf( head, sizeof(head), "%s", line );
            else if (!strncmp( line, "total ", 6 )) twice = counter( line, "evpool_double_close" );
            else if (!strncmp( line, want, strlen( want ) ))
            {
                mine = TRUE;
                hit = counter( line, "evpool_hit" );
                put = counter( line, "evpool_put" );
                server = counter( line, "server" );
            }
        }
        fclose( f );
        found = mine;
    } while (!found && FindNextFileA( find, &fd ));
    FindClose( find );
    if (!found) FAIL( "no row for the thread in %s", dir );

    printf( "gw2 test evpool: %s", head );
    printf( "gw2 test evpool: the thread: evpool_hit %lld evpool_put %lld server %lld; the process: evpool_double_close %lld\n",
            hit, put, server, twice );
    /* "... mode count evpool seconds ...", and the exe's name may hold the word too */
    if (!(mode = strstr( head, " mode " )) || !(end = strstr( mode, " seconds " ))) FAIL( "no mode in the log" );
    *end = 0;
    if (!strstr( mode, " evpool" ) != !pool) FAIL( "the log's mode doesn't match WINE_HEAT_EVPOOL" );
    if (!pool) return;
    if (hit != HITS || put != HITS) FAIL( "%d of each expected", HITS );
    /* its start and its end; without the pool each round makes three */
    if (server < 0 || server > 50) FAIL( "%lld server calls: the pool's creates and closes made some", server );
    if (twice < 2) FAIL( "the double close and its duplicate weren't counted" );
}

int main( int argc, char **argv )
{
    setvbuf( stdout, NULL, _IONBF, 0 );

    test_storm();
    test_dup();
    test_read();
    test_select();
    test_twice();
    test_counters();

    printf( "gw2 test evpool: passed\n" );
    return 0;
}
