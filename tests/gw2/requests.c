/* gw2 (jdrly/gw2-on-mac#60): heatreq.c's server requests by code, and its splits of set_cursor,
 * get_message and set_queue_mask.
 *
 * The main thread first releases a semaphore of its own RELEASES times, so that it is the game
 * thread by the session report's rule (the most releases). Then, in one go:
 * 1. SetCursor 1,010 times with two cursors of its own, A 1,000 times and B 10: five rounds of B
 *    twice and A 200 times. Each run's first call changes the handle and the others repeat it, so
 *    1,000 of the 1,010 repeat. SetCursor returns the previous cursor, which each call checks.
 * 2. It posts itself WM_APP + 1 100 times and WM_APP + 2 50 times and takes each with PeekMessage,
 *    a get_message request whose reply names the message.
 * 3. MsgWaitForMultipleObjectsEx with no handle and no timeout, 100 times, the mask alternating
 *    between QS_KEY and QS_MOUSEBUTTON, so that each call sends set_queue_mask.
 * None of it needs a window: the server's set_cursor needs only the thread's queue. CreateCursor
 * loads the display driver (CreateCompatibleDC), so the program prints whether the Mac driver
 * started; with the null driver it passes all the same. It never calls GetCursor, which sends a
 * set_cursor request of its own once the thread has set a cursor.
 *
 * With WINE_HEAT_COUNT=1 and WINE_HEAT_EXE=requests.exe it starts right after the dumper's first
 * block, so the calls fall in one 5 s block, then waits for the next and adds up this thread's
 * blocks in heat-requests-*.log: 1,010 set_cursor requests, all handle requests, 1,000 repeats and
 * none of the other flags; the messages 0x8001 100 times and 0x8002 50 times; at least 150
 * get_message and 100 set_queue_mask requests. With GW2_TEST_SETCURSOR_SKIPS=1, for my win32u.so
 * with WINE_WIN32U_SETCURSOR=shm (jdrly/gw2-on-mac#61), the repeats make no request: 10 set_cursor
 * requests, each run's first call, and 0 repeats.
 *
 * With FEX_PROFILESTATS=1 as well (jdrly/gw2-on-mac#111), each of this thread's blocks has FEX's
 * line right after its requests line, with the twelve names in their order, and the blocks add up to
 * at least one block compiled. Without it, no block has a fex line.
 *
 * Prints "gw2 test requests: passed" and exits 0, or names what failed and exits 1. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#define RELEASES 20000
#define ROUNDS   5    /* of B twice and A A_RUN times */
#define A_RUN    200
#define POSTS    150  /* two of three WM_APP + 1, one WM_APP + 2 */
#define WAITS    100

#define FAIL(...) do { \
        printf( "gw2 test requests: failed, line %d: ", __LINE__ ); \
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

static HCURSOR cursor( BYTE fill )
{
    BYTE mask[32 * 32 / 8], bits[32 * 32 / 8];
    HCURSOR c;

    memset( mask, fill, sizeof(mask) );
    memset( bits, 0, sizeof(bits) );
    if (!(c = CreateCursor( GetModuleHandleW( NULL ), 0, 0, 32, 32, mask, bits )))
        FAIL( "CreateCursor: error %lu", GetLastError() );
    return c;
}

static HCURSOR set_cursor( HCURSOR c, HCURSOR want_prev )
{
    HCURSOR prev = SetCursor( c );
    if (want_prev && prev != want_prev) FAIL( "SetCursor(%p) returned %p, not %p", c, prev, want_prev );
    return c;
}

/* the requests file of this run in $WINE_HEAT_LOG, or FALSE if there is none yet */
static BOOL find_log( char *path, size_t size )
{
    char dir[MAX_PATH];
    WIN32_FIND_DATAA fd;
    HANDLE find;

    if (!GetEnvironmentVariableA( "WINE_HEAT_LOG", dir, sizeof(dir) )) FAIL( "WINE_HEAT_LOG isn't set" );
    snprintf( path, size, "Z:%s/heat-requests-*.log", dir );
    if ((find = FindFirstFileA( path, &fd )) == INVALID_HANDLE_VALUE) return FALSE;
    snprintf( path, size, "Z:%s/%s", dir, fd.cFileName );
    FindClose( find );
    return TRUE;
}

static DWORD log_size( const char *path )
{
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (!GetFileAttributesExA( path, GetFileExInfoStandard, &data )) FAIL( "can't read %s: error %lu", path, GetLastError() );
    return data.nFileSizeLow;
}

/* the number after " <name> " in <line>, or 0 */
static long long field( const char *line, const char *name )
{
    char key[64];
    const char *p;
    snprintf( key, sizeof(key), " %s ", name );
    return (p = strstr( line, key )) ? _atoi64( p + strlen( key ) ) : 0;
}

/* jdrly/gw2-on-mac#111: the fex line's first names, in this order */
static const char * const fex_names[] = { "jit_ms", "jit", "miss", "rlock_ms", "wlock_ms", "smc", "sigbus", "float",
                                          "signal_ms", "dcache_hit", "dcache_miss", "dcache_ms" };

/* a block's fex line: its names checked, its jit count added to <jit> */
static void check_fex( const char *line, long long *jit )
{
    const char *p = line + 3;
    char name[32], value[32];
    unsigned int i;
    int n;

    if (strncmp( line, "fex ", 4 ) || !strncmp( line, "fex has no slot", 15 ) || !strncmp( line, "fex stats ", 10 ))
        FAIL( "the line after this thread's requests line isn't FEX's counters: %s", line );
    for (i = 0; i < sizeof(fex_names) / sizeof(fex_names[0]); i++)
    {
        if (sscanf( p, " %31s %31s%n", name, value, &n ) != 2 || strcmp( name, fex_names[i] ))
            FAIL( "the fex line's field %u isn't %s: %s", i + 1, fex_names[i], line );
        if (i == 1) *jit += _atoi64( value );
        p += n;
    }
}

static void check_log(void)
{
    char path[MAX_PATH * 2], want[64], line[512];
    long long blocks = 0, req_cursor = 0, req_message = 0, req_mask = 0, handle = 0, repeat = 0, others = 0;
    long long app1 = 0, app2 = 0, other = 0, internal = 0, poll = 0, nopoll = 0, fex_lines = 0, jit = 0;
    double cursor_ms = 0;
    BOOL mine = FALSE, skips = env_is( "GW2_TEST_SETCURSOR_SKIPS", "1" );
    BOOL fex = env_is( "FEX_PROFILESTATS", "1" ), first = FALSE;
    long long want_cursor = skips ? ROUNDS * 2 : ROUNDS * (2 + A_RUN), want_repeat = skips ? 0 : ROUNDS * A_RUN;
    FILE *f;

    if (!find_log( path, sizeof(path) )) FAIL( "no heat-requests-*.log" );
    if (!(f = fopen( path, "r" ))) FAIL( "can't open %s", path );
    snprintf( want, sizeof(want), "requests thread %04lx ", GetCurrentThreadId() );
    while (fgets( line, sizeof(line), f ))
    {
        BOOL after = first;  /* the line right after a requests line */

        first = FALSE;
        if (!strncmp( line, "requests thread ", 16 ))
        {
            if ((mine = !strncmp( line, want, strlen( want ) ))) blocks++;
            first = TRUE;
            continue;
        }
        if (!strncmp( line, "fex ", 4 ))
        {
            if (!fex) FAIL( "a fex line without FEX_PROFILESTATS: %s", line );
            if (!after) FAIL( "a fex line that doesn't follow a requests line: %s", line );
        }
        if (mine && after && fex)
        {
            check_fex( line, &jit );
            fex_lines++;
            continue;
        }
        if (!mine) continue;
        if (!strncmp( line, "req set_cursor ", 15 ))
        {
            req_cursor += _atoi64( line + 15 );
            cursor_ms += atof( strchr( line + 15, ' ' ) + 1 );
        }
        else if (!strncmp( line, "req get_message ", 16 )) req_message += _atoi64( line + 16 );
        else if (!strncmp( line, "req set_queue_mask ", 19 )) req_mask += _atoi64( line + 19 );
        else if (!strncmp( line, "set_cursor ", 11 ))
        {
            handle += field( line, "handle" );
            repeat += field( line, "repeat" );
            others += field( line, "count" ) + field( line, "pos" ) + field( line, "clip" ) + field( line, "noclip" );
        }
        else if (!strncmp( line, "get_message ", 12 ))
        {
            internal += field( line, "internal" );
            other += field( line, "other" );
        }
        else if (!strncmp( line, "message 0x8001 ", 15 )) app1 += _atoi64( line + 15 );
        else if (!strncmp( line, "message 0x8002 ", 15 )) app2 += _atoi64( line + 15 );
        else if (!strncmp( line, "set_queue_mask ", 15 ))
        {
            poll += field( line, "poll" );
            nopoll += field( line, "nopoll" );
        }
    }
    fclose( f );

    printf( "gw2 test requests: %lld blocks of this thread; req set_cursor %lld (%.3f ms) get_message %lld "
            "set_queue_mask %lld; set_cursor handle %lld repeat %lld other flags %lld; get_message internal %lld "
            "other %lld; message 0x8001 %lld 0x8002 %lld; set_queue_mask poll %lld nopoll %lld\n",
            blocks, req_cursor, cursor_ms, req_message, req_mask, handle, repeat, others, internal, other,
            app1, app2, poll, nopoll );
    if (fex) printf( "gw2 test requests: %lld fex lines of this thread, jit %lld\n", fex_lines, jit );
    if (!blocks) FAIL( "no block of this thread: it isn't the game thread" );
    if (fex && (fex_lines != blocks || !jit))
        FAIL( "%lld fex lines in %lld blocks of this thread, jit %lld: each block needs one, and FEX compiled code",
              fex_lines, blocks, jit );
    /* the splits count every request of a block, the req lines only a block's twelve most frequent,
       where the 10 requests with skips may not be */
    if (req_cursor != want_cursor && !(skips && !req_cursor))
        FAIL( "%lld set_cursor requests, not %lld", req_cursor, want_cursor );
    if (req_cursor && cursor_ms <= 0) FAIL( "no time for set_cursor" );
    if (handle != want_cursor || others) FAIL( "set_cursor: %lld handle requests, %lld with other flags", handle, others );
    if (repeat != want_repeat) FAIL( "%lld repeats, not %lld", repeat, want_repeat );
    if (app1 != POSTS / 3 * 2 || app2 != POSTS / 3) FAIL( "messages 0x8001 %lld and 0x8002 %lld, not %d and %d",
                                                         app1, app2, POSTS / 3 * 2, POSTS / 3 );
    if (req_message < POSTS || other < POSTS) FAIL( "%lld get_message requests, %lld not internal", req_message, other );
    if (req_mask < WAITS || poll + nopoll < req_mask) FAIL( "%lld set_queue_mask requests, poll %lld nopoll %lld",
                                                            req_mask, poll, nopoll );
}

int main(void)
{
    HCURSOR a, b, prev = NULL;
    HANDLE sem;
    BOOL counting = env_is( "WINE_HEAT_COUNT", "1" ) && env_is( "WINE_HEAT_EXE", "requests.exe" );
    char path[MAX_PATH * 2];
    DWORD start, size;
    MSG msg;
    int i, k;

    setvbuf( stdout, NULL, _IONBF, 0 );
    a = cursor( 0xff );
    b = cursor( 0x0f );
    if (!(sem = CreateSemaphoreW( NULL, 0, 0x7fffffff, NULL ))) FAIL( "CreateSemaphore: error %lu", GetLastError() );
    for (i = 0; i < RELEASES; i++)
        if (!ReleaseSemaphore( sem, 1, NULL )) FAIL( "ReleaseSemaphore: error %lu", GetLastError() );
    CloseHandle( sem );
    PeekMessageW( &msg, NULL, 0, 0, PM_NOREMOVE );  /* this thread's queue */

    if (counting)
    {
        for (start = GetTickCount(); !find_log( path, sizeof(path) ); Sleep( 20 ))
            if (GetTickCount() - start > 15000) FAIL( "the dumper wrote no heat-requests-*.log in 15 s" );
    }

    /* 1 */
    for (k = 0; k < ROUNDS; k++)
    {
        for (i = 0; i < 2; i++) prev = set_cursor( b, prev );
        for (i = 0; i < A_RUN; i++) prev = set_cursor( a, prev );
    }
    printf( "gw2 test requests: the Mac driver %s\n",
            GetModuleHandleW( L"winemac.drv" ) ? "started" : "didn't start here, the null driver's calls" );

    /* 2 */
    for (i = 0; i < POSTS; i++)
    {
        UINT m = WM_APP + (i % 3 == 2 ? 2 : 1);
        if (!PostThreadMessageW( GetCurrentThreadId(), m, i, 0 )) FAIL( "PostThreadMessage: error %lu", GetLastError() );
        if (!PeekMessageW( &msg, NULL, 0, 0, PM_REMOVE )) FAIL( "message %d wasn't there", i );
        if (msg.message != m || msg.wParam != (WPARAM)i) FAIL( "message %d was %#x, not %#x", i, msg.message, m );
    }

    /* 3 */
    for (i = 0; i < WAITS; i++)
        if (MsgWaitForMultipleObjectsEx( 0, NULL, 0, i & 1 ? QS_MOUSEBUTTON : QS_KEY, 0 ) != WAIT_TIMEOUT)
            FAIL( "wait %d didn't time out", i );
    printf( "gw2 test requests: %d SetCursor calls, %d messages, %d waits\n", ROUNDS * (2 + A_RUN), POSTS, WAITS );

    if (counting)
    {
        /* a block written after the calls, at most 5 s on */
        for (size = log_size( path ), start = GetTickCount(); log_size( path ) == size; Sleep( 20 ))
            if (GetTickCount() - start > 15000) FAIL( "no block in 15 s after the calls" );
        check_log();
    }
    printf( "gw2 test requests: passed\n" );
    return 0;
}
