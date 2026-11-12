/* gw2 (jdrly/gw2-on-mac#61): the yield after an empty PeekMessage, which WINE_WIN32U_PEEK_YIELD=off
 * in my win32u.so leaves out.
 *
 * The main thread takes PEEKS empty PeekMessage calls (PM_REMOVE on an empty queue) and then posts
 * itself one message and takes it, so that the queue still works. With WINE_HEAT_COUNT=1 and
 * WINE_HEAT_EXE=peekyield.exe my ntdll.so counts each NtYieldExecution in the thread's row of
 * heat-<pid>.log, which the dumper rewrites every 5 s: the program waits for two rewrites after the
 * calls and reads its row. Wine yields once per empty peek, so the row has at least PEEKS yields;
 * with GW2_TEST_PEEK_NO_YIELD=1, for my win32u.so with WINE_WIN32U_PEEK_YIELD=off, it has fewer
 * than 10 (none of them the peeks'). Without counting it only makes the calls.
 *
 * Prints "gw2 test peekyield: passed" and exits 0, or names what failed and exits 1. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#define PEEKS 1000

#define FAIL(...) do { \
        printf( "gw2 test peekyield: failed, line %d: ", __LINE__ ); \
        printf( __VA_ARGS__ ); \
        printf( "\n" ); \
        exit( 1 ); \
    } while (0)

static BOOL env_is( const char *name, const char *value )
{
    char buf[256];
    DWORD n = GetEnvironmentVariableA( name, buf, sizeof(buf) );
    return n && n < sizeof(buf) && !_stricmp( buf, value );
}

static char text[65536];

/* the counters file of this process in $WINE_HEAT_LOG, read into text; FALSE if there is none yet */
static BOOL read_log(void)
{
    char dir[MAX_PATH], path[MAX_PATH * 2];
    WIN32_FIND_DATAA fd;
    HANDLE find;
    FILE *f;
    size_t n;

    if (!GetEnvironmentVariableA( "WINE_HEAT_LOG", dir, sizeof(dir) )) FAIL( "WINE_HEAT_LOG isn't set" );
    /* heat-<pid>.log, not heat-requests-<pid>.log */
    snprintf( path, sizeof(path), "Z:%s/heat-*.log", dir );
    if ((find = FindFirstFileA( path, &fd )) == INVALID_HANDLE_VALUE) return FALSE;
    do
    {
        if (strncmp( fd.cFileName, "heat-requests-", 14 )) break;
    } while (FindNextFileA( find, &fd ));
    FindClose( find );
    if (!strncmp( fd.cFileName, "heat-requests-", 14 )) return FALSE;
    snprintf( path, sizeof(path), "Z:%s/%s", dir, fd.cFileName );
    if (!(f = fopen( path, "rb" ))) return FALSE;
    n = fread( text, 1, sizeof(text) - 1, f );
    fclose( f );
    text[n] = 0;
    return n > 0;
}

/* the yields in this thread's row of text, or -1 if it has none */
static long long yields(void)
{
    char want[32];
    const char *line, *p;

    snprintf( want, sizeof(want), "thread %04lx ", GetCurrentThreadId() );
    for (line = text; line && *line; line = (p = strchr( line, '\n' )) ? p + 1 : NULL)
    {
        if (strncmp( line, want, strlen( want ) )) continue;
        if (!(p = strstr( line, " yield " )) || (strchr( line, '\n' ) && p > strchr( line, '\n' ))) return -1;
        return _atoi64( p + 7 );
    }
    return -1;
}

int main(void)
{
    BOOL counting = env_is( "WINE_HEAT_COUNT", "1" ) && env_is( "WINE_HEAT_EXE", "peekyield.exe" );
    BOOL no_yield = env_is( "GW2_TEST_PEEK_NO_YIELD", "1" );
    static char before[sizeof(text)];
    long long n;
    DWORD start;
    MSG msg;
    int i, rewrites;

    setvbuf( stdout, NULL, _IONBF, 0 );
    PeekMessageW( &msg, NULL, 0, 0, PM_NOREMOVE );  /* this thread's queue */

    for (i = 0; i < PEEKS; i++)
        if (PeekMessageW( &msg, NULL, 0, 0, PM_REMOVE )) FAIL( "peek %d found message %#x", i, msg.message );
    if (!PostThreadMessageW( GetCurrentThreadId(), WM_APP, 1, 0 )) FAIL( "PostThreadMessage: error %lu", GetLastError() );
    if (!PeekMessageW( &msg, NULL, 0, 0, PM_REMOVE ) || msg.message != WM_APP || msg.wParam != 1)
        FAIL( "the posted message wasn't there" );
    printf( "gw2 test peekyield: %d empty peeks and one message\n", PEEKS );

    if (counting)
    {
        /* two rewrites after the calls: the first may have begun during them */
        for (start = GetTickCount(); !read_log(); Sleep( 50 ))
            if (GetTickCount() - start > 15000) FAIL( "the dumper wrote no heat-<pid>.log in 15 s" );
        for (rewrites = 0; rewrites < 2; rewrites++)
        {
            memcpy( before, text, sizeof(text) );
            for (start = GetTickCount(); !read_log() || !strcmp( before, text ); Sleep( 50 ))
                if (GetTickCount() - start > 15000) FAIL( "heat-<pid>.log wasn't rewritten in 15 s" );
        }
        if ((n = yields()) < 0) FAIL( "no row of this thread with a yield count in heat-<pid>.log" );
        printf( "gw2 test peekyield: %lld yields on this thread\n", n );
        if (no_yield ? n >= 10 : n < PEEKS)
            FAIL( "%lld yields, %s", n, no_yield ? "not under 10" : "fewer than the empty peeks" );
    }
    printf( "gw2 test peekyield: passed\n" );
    return 0;
}
