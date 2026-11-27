/* gw2 (jdrly/gw2-on-mac#387): a session notification registered and unregistered the way NAudio
 * 2.0.0 does it for Blish HUD. NAudio takes the default render endpoint, activates
 * IAudioSessionManager2 on it and registers an IAudioSessionNotification in its constructor, then
 * unregisters it from its finalizer, on another thread, and throws on any result but S_OK both
 * times. The engine's mmdevapi.dll answers E_NOTIMPL to both; mine keeps the notification.
 *
 * The checks, in order:
 * 1. RegisterSessionNotification returns S_OK and holds a reference on the notification.
 * 2. UnregisterSessionNotification, from a second thread, returns S_OK and drops it.
 * 3. A second unregister of the same pointer returns E_INVALIDARG.
 * 4. A notification still registered when the manager's last reference goes is released with it.
 *
 * A runner may have no audio device, so before COM starts the program writes a render endpoint
 * into the prefix's registry, as mmdevapi keeps the endpoints it has seen. mmdevapi loads it as
 * not present, and its session manager doesn't care. The default endpoint comes first; the
 * written one only when there is none. If neither gives a manager, the program says
 * "inconclusive" and passes, and the workflow warns.
 *
 * Prints "gw2 test sessions: passed" and exits 0, or names the call that failed with its result
 * and exits 1. Under the engine's mmdevapi.dll the first check fails with
 * "RegisterSessionNotification returned 0x80004001". */
#define COBJMACROS
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>
#include <initguid.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>

/* the endpoint this program writes, and the name mmdevapi reads for it
 * (PKEY_Device_FriendlyName, {a45c254e-df1c-4efd-8020-67d146a850e0},14) */
#define SEEDED_GUID L"{6B1F3E2A-9C47-4D8E-A250-387000000387}"
#define SEEDED_KEY  L"Software\\Microsoft\\Windows\\CurrentVersion\\MMDevices\\Audio\\Render\\" SEEDED_GUID L"\\Properties"
#define SEEDED_ID   L"{0.0.0.00000000}." SEEDED_GUID
#define FRIENDLY_NAME_VALUE L"{A45C254E-DF1C-4EFD-8020-67D146A850E0},14"

#define FAIL(...) do { \
        printf( "gw2 test sessions: failed: " ); \
        printf( __VA_ARGS__ ); \
        printf( "\n" ); \
        exit( 1 ); \
    } while (0)

/* the notification, which counts its references */
struct notification
{
    IAudioSessionNotification iface;
    LONG ref;
};

static HRESULT WINAPI notification_QueryInterface( IAudioSessionNotification *iface, REFIID riid, void **out )
{
    if (IsEqualIID( riid, &IID_IUnknown ) || IsEqualIID( riid, &IID_IAudioSessionNotification ))
    {
        *out = iface;
        IAudioSessionNotification_AddRef( iface );
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI notification_AddRef( IAudioSessionNotification *iface )
{
    return InterlockedIncrement( &CONTAINING_RECORD( iface, struct notification, iface )->ref );
}

static ULONG WINAPI notification_Release( IAudioSessionNotification *iface )
{
    return InterlockedDecrement( &CONTAINING_RECORD( iface, struct notification, iface )->ref );
}

static HRESULT WINAPI notification_OnSessionCreated( IAudioSessionNotification *iface, IAudioSessionControl *session )
{
    return S_OK;
}

static IAudioSessionNotificationVtbl notification_vtbl =
{
    notification_QueryInterface,
    notification_AddRef,
    notification_Release,
    notification_OnSessionCreated,
};

static void seed_endpoint( void )
{
    static const WCHAR name[] = L"gw2 test sessions";
    HKEY key;
    LONG ret;

    ret = RegCreateKeyExW( HKEY_LOCAL_MACHINE, SEEDED_KEY, 0, NULL, 0, KEY_WRITE | KEY_WOW64_64KEY, NULL, &key, NULL );
    if (ret) FAIL( "RegCreateKeyEx for the written endpoint returned %ld", ret );
    ret = RegSetValueExW( key, FRIENDLY_NAME_VALUE, 0, REG_SZ, (const BYTE *)name, sizeof(name) );
    RegCloseKey( key );
    if (ret) FAIL( "RegSetValueEx for the written endpoint returned %ld", ret );
}

/* the second thread, as NAudio's finalizer */
static IAudioSessionManager2 *manager;
static struct notification notification = { { &notification_vtbl }, 1 };
static HRESULT unregister_result;

static DWORD WINAPI finalizer( void *arg )
{
    CoInitializeEx( NULL, COINIT_MULTITHREADED );
    unregister_result = IAudioSessionManager2_UnregisterSessionNotification( manager, &notification.iface );
    CoUninitialize();
    return 0;
}

int main( void )
{
    IMMDeviceEnumerator *enumerator;
    IMMDevice *device = NULL;
    HRESULT hr, hr_default, hr_seeded = E_FAIL;
    HANDLE thread;
    ULONG left;

    setvbuf( stdout, NULL, _IONBF, 0 );
    seed_endpoint();

    hr = CoInitializeEx( NULL, COINIT_MULTITHREADED );
    if (FAILED(hr)) FAIL( "CoInitializeEx returned 0x%08lx", hr );
    hr = CoCreateInstance( &CLSID_MMDeviceEnumerator, NULL, CLSCTX_INPROC_SERVER, &IID_IMMDeviceEnumerator, (void **)&enumerator );
    if (FAILED(hr)) FAIL( "CoCreateInstance(MMDeviceEnumerator) returned 0x%08lx", hr );

    hr_default = IMMDeviceEnumerator_GetDefaultAudioEndpoint( enumerator, eRender, eMultimedia, &device );
    if (SUCCEEDED(hr_default))
        printf( "gw2 test sessions: the default render endpoint\n" );
    else
    {
        hr_seeded = IMMDeviceEnumerator_GetDevice( enumerator, SEEDED_ID, &device );
        if (SUCCEEDED(hr_seeded))
            printf( "gw2 test sessions: no default render endpoint (0x%08lx), the written one\n", hr_default );
    }
    if (!device)
    {
        printf( "gw2 test sessions: inconclusive, no endpoint: GetDefaultAudioEndpoint returned 0x%08lx, "
                "GetDevice of the written one 0x%08lx\n", hr_default, hr_seeded );
        printf( "gw2 test sessions: passed\n" );
        return 0;
    }

    hr = IMMDevice_Activate( device, &IID_IAudioSessionManager2, CLSCTX_ALL, NULL, (void **)&manager );
    if (FAILED(hr)) FAIL( "Activate(IAudioSessionManager2) returned 0x%08lx", hr );

    /* 1 */
    hr = IAudioSessionManager2_RegisterSessionNotification( manager, &notification.iface );
    printf( "gw2 test sessions: RegisterSessionNotification 0x%08lx, the notification's references %ld\n", hr, notification.ref );
    if (hr != S_OK) FAIL( "RegisterSessionNotification returned 0x%08lx, not S_OK", hr );
    if (notification.ref != 2) FAIL( "the registered notification has %ld references, not 2", notification.ref );

    /* 2 */
    if (!(thread = CreateThread( NULL, 0, finalizer, NULL, 0, NULL ))) FAIL( "CreateThread, error %lu", GetLastError() );
    if (WaitForSingleObject( thread, 30000 ) != WAIT_OBJECT_0) FAIL( "the second thread didn't end within 30 s" );
    CloseHandle( thread );
    printf( "gw2 test sessions: UnregisterSessionNotification from a second thread 0x%08lx, the notification's references %ld\n",
            unregister_result, notification.ref );
    if (unregister_result != S_OK) FAIL( "UnregisterSessionNotification returned 0x%08lx, not S_OK", unregister_result );
    if (notification.ref != 1) FAIL( "the unregistered notification has %ld references, not 1", notification.ref );

    /* 3 */
    hr = IAudioSessionManager2_UnregisterSessionNotification( manager, &notification.iface );
    printf( "gw2 test sessions: the second UnregisterSessionNotification 0x%08lx\n", hr );
    if (hr != E_INVALIDARG) FAIL( "the second UnregisterSessionNotification returned 0x%08lx, not E_INVALIDARG", hr );

    /* 4 */
    hr = IAudioSessionManager2_RegisterSessionNotification( manager, &notification.iface );
    if (hr != S_OK) FAIL( "RegisterSessionNotification, the second time, returned 0x%08lx, not S_OK", hr );
    left = IAudioSessionManager2_Release( manager );
    printf( "gw2 test sessions: the manager released with the notification registered: %lu references left on it, "
            "%ld on the notification\n", left, notification.ref );
    if (left) FAIL( "the manager has %lu references left, so its release can't show the notification's", left );
    if (notification.ref != 1) FAIL( "the released manager left the notification %ld references, not 1", notification.ref );

    IMMDevice_Release( device );
    IMMDeviceEnumerator_Release( enumerator );
    CoUninitialize();
    printf( "gw2 test sessions: passed\n" );
    return 0;
}
