/* gw2 (jdrly/gw2-on-mac#160): the Dock's half of tests/gw2/restore.c. restore.exe runs it through
 * ntdll's __wine_unix_spawnvp and waits for it, so its parent is the Wine process with the window.
 *
 *   reopen shown     returns once a window of the parent is on the screen
 *   reopen restore   waits until no window of the parent is on the screen, then sends the parent
 *                    the reopen Apple event (kAEReopenApplication, which a click on a running
 *                    app's Dock icon sends) until a window of it is back
 *
 * The Mac driver answers the reopen event by deminiaturizing a minimized window when none is
 * visible (applicationShouldHandleReopen: and unminimizeWindowIfNoneVisible in cocoa_app.m).
 * A window counts when CGWindowListCopyWindowInfo lists it on the screen at the normal level
 * (layer 0), which needs no permission for a window's owner and layer. While AppKit miniaturizes
 * the window, it can be off the screen before AppKit counts it as minimized, and a reopen event
 * then finds nothing to bring back. So it sends the event again each second until a window is
 * back. That second only paces the retries; whether restore.exe passes doesn't depend on it.
 *
 * Prints what it does on lines restore.exe's log shows ("gw2 test restore: reopen: ..."). Exits
 * 0 when done, 2 when the system refuses the Apple event (the machine's privacy settings), and
 * 1 on any other error. */
#include <CoreGraphics/CoreGraphics.h>
#include <CoreServices/CoreServices.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* the parent's windows on the screen at layer 0, or -1 without a list */
static int on_screen( pid_t pid )
{
    CFArrayRef list = CGWindowListCopyWindowInfo( kCGWindowListOptionOnScreenOnly | kCGWindowListExcludeDesktopElements,
                                                  kCGNullWindowID );
    CFIndex i;
    int n = 0;

    if (!list) return -1;
    for (i = 0; i < CFArrayGetCount( list ); i++)
    {
        CFDictionaryRef w = CFArrayGetValueAtIndex( list, i );
        CFNumberRef owner = CFDictionaryGetValue( w, kCGWindowOwnerPID );
        CFNumberRef layer = CFDictionaryGetValue( w, kCGWindowLayer );
        int o = 0, l = -1;

        if (owner) CFNumberGetValue( owner, kCFNumberIntType, &o );
        if (layer) CFNumberGetValue( layer, kCFNumberIntType, &l );
        if (o == pid && l == 0) n++;
    }
    CFRelease( list );
    return n;
}

/* waits until the parent has a window on the screen (want 1) or none (want 0); 0, or -1 without a list */
static int wait_for( pid_t pid, int want )
{
    int n;

    while ((n = on_screen( pid )) >= 0 && (n > 0) != want) usleep( 20000 );
    return n < 0 ? -1 : 0;
}

static OSStatus send_reopen( pid_t pid )
{
    AEAddressDesc target;
    AppleEvent event, reply = { typeNull, NULL };
    OSStatus status;

    if ((status = AECreateDesc( typeKernelProcessID, &pid, sizeof(pid), &target ))) return status;
    status = AECreateAppleEvent( kCoreEventClass, kAEReopenApplication, &target, kAutoGenerateReturnID,
                                 kAnyTransactionID, &event );
    AEDisposeDesc( &target );
    if (status) return status;
    /* waits until the parent's main thread has handled it, 10 seconds at most (600 ticks) */
    status = AESendMessage( &event, &reply, kAEWaitReply | kAENeverInteract | kAEDoNotPromptForUserConsent, 600 );
    AEDisposeDesc( &event );
    AEDisposeDesc( &reply );
    return status;
}

int main( int argc, char *argv[] )
{
    pid_t pid = getppid();
    OSStatus status;
    int i, sent = 0;

    setvbuf( stdout, NULL, _IONBF, 0 );
    if (argc == 2 && !strcmp( argv[1], "shown" ))
    {
        if (wait_for( pid, 1 )) goto no_list;
        printf( "gw2 test restore: reopen: the window is on the screen\n" );
        return 0;
    }
    if (argc != 2 || strcmp( argv[1], "restore" ))
    {
        printf( "gw2 test restore: reopen: usage: reopen shown | restore\n" );
        return 1;
    }

    if (wait_for( pid, 0 )) goto no_list;
    printf( "gw2 test restore: reopen: the window is off the screen\n" );
    for (;;)
    {
        status = send_reopen( pid );
        sent++;
        if (status == errAEEventNotPermitted || status == errAEEventWouldRequireUserConsent)
        {
            printf( "gw2 test restore: reopen: the system refused the reopen event (%d)\n", (int)status );
            return 2;
        }
        /* a timeout leaves the event queued for the main thread, which may still handle it */
        if (status && status != errAETimeout)
        {
            printf( "gw2 test restore: reopen: AESendMessage failed (%d)\n", (int)status );
            return 1;
        }
        for (i = 0; i < 50; i++)
        {
            int n = on_screen( pid );

            if (n < 0) goto no_list;
            if (n > 0)
            {
                printf( "gw2 test restore: reopen: the window is back after %d reopen event%s\n", sent, sent == 1 ? "" : "s" );
                return 0;
            }
            usleep( 20000 );
        }
    }

no_list:
    printf( "gw2 test restore: reopen: CGWindowListCopyWindowInfo returned no list\n" );
    return 1;
}
