// The system's media sessions, read and driven on a thread of their own. MinGW ships no header for Windows.Media.Control,
// so its interfaces are called through their vtables by slot, in the order of Windows.Media.winmd (as windows-rs lists
// them), and combase's WinRT functions are looked up at run time. Asynchronous calls are waited on by polling IAsyncInfo,
// which needs none of the parameterized interface ids a completion handler would.
#include "media.h"
#include "str.h"
#include <objbase.h>
#include <stdlib.h>
#include <string.h>

typedef struct { void **vt; } Obj;
#define VT(o) (((Obj *)(o))->vt)
typedef HRESULT (WINAPI *QueryFn)(void *, REFIID, void **);
typedef ULONG (WINAPI *ReleaseFn)(void *);
typedef HRESULT (WINAPI *GetPtrFn)(void *, void **);
typedef HRESULT (WINAPI *GetIntFn)(void *, INT32 *);
typedef HRESULT (WINAPI *GetUintFn)(void *, UINT32 *);
typedef HRESULT (WINAPI *GetAtFn)(void *, UINT32, void **);
typedef HRESULT (WINAPI *GetBoolFn)(void *, unsigned char *);

// Slots after IUnknown (0-2) and IInspectable (3-5).
enum { STATICS_REQUEST = 6 };
enum { MANAGER_CURRENT = 6, MANAGER_SESSIONS = 7 };
enum { SESSION_APP_ID = 6, SESSION_PROPERTIES = 7, SESSION_PLAYBACK = 9, SESSION_PLAY = 10, SESSION_PAUSE = 11, SESSION_NEXT = 16, SESSION_PREVIOUS = 17 };
enum { PROPS_TITLE = 6, PROPS_ALBUM_ARTIST = 8, PROPS_ARTIST = 9 };
enum { PLAYBACK_CONTROLS = 6, PLAYBACK_STATUS = 7 };
enum { CONTROLS_PLAY = 6, CONTROLS_PAUSE = 7, CONTROLS_NEXT = 12, CONTROLS_PREVIOUS = 13, CONTROLS_TOGGLE = 16 };
enum { VECTOR_GET_AT = 6, VECTOR_SIZE = 7 };
enum { ASYNC_RESULTS = 8, ASYNC_INFO_STATUS = 7 };
enum { STATUS_PLAYING = 4 };
enum { ASYNC_STARTED = 0, ASYNC_COMPLETED = 1 };

static const GUID IID_ManagerStatics = { 0x2050c4ee, 0x11a0, 0x57de, { 0xae, 0xd7, 0xc9, 0x7c, 0x70, 0x33, 0x82, 0x45 } };
static const GUID IID_AsyncInfo = { 0x00000036, 0x0000, 0x0000, { 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46 } };
static const wchar_t MANAGER_CLASS[] = L"Windows.Media.Control.GlobalSystemMediaTransportControlsSessionManager";

typedef void *WinString;
static HRESULT (WINAPI *ro_get_factory)(WinString, REFIID, void **);
static HRESULT (WINAPI *hstr_create)(const wchar_t *, UINT32, WinString *);
static HRESULT (WINAPI *hstr_delete)(WinString);
static const wchar_t *(WINAPI *hstr_buffer)(WinString, UINT32 *);

static CRITICAL_SECTION g_lock;
static MediaState g_state;
static HWND g_notify;
static HANDLE g_thread, g_wake;
static volatile LONG g_quit, g_active = 1;
static MediaCommand g_pending[8];
static int g_pending_count;

static void release(void *o) { if (o) ((ReleaseFn)VT(o)[2])(o); }
static void *get_ptr(void *o, int slot) {
    void *out = NULL;
    if (!o || FAILED(((GetPtrFn)VT(o)[slot])(o, &out))) return NULL;
    return out;
}
static bool get_bool(void *o, int slot) { unsigned char b = 0; return o && SUCCEEDED(((GetBoolFn)VT(o)[slot])(o, &b)) && b; }

/// A string property into `out`, cut at a character boundary.
static void get_string(void *o, int slot, char *out, size_t cap) {
    out[0] = 0;
    WinString h = get_ptr(o, slot);
    if (!h) return;
    char *s = wide_to_utf8(hstr_buffer(h, NULL));
    hstr_delete(h);
    if (!s) return;
    size_t n = strlen(s);
    if (n >= cap) { n = cap - 1; while (n && ((unsigned char)s[n] & 0xC0) == 0x80) n--; }
    memcpy(out, s, n); out[n] = 0;
    free(s);
}

/// Waits for an asynchronous operation; its result for an object-valued one (NULL for the others, or on failure). Takes
/// the operation's reference.
static void *await(void *op, bool wants_result, DWORD timeout_ms) {
    if (!op) return NULL;
    void *info = NULL, *result = NULL;
    if (SUCCEEDED(((QueryFn)VT(op)[0])(op, &IID_AsyncInfo, &info))) {
        INT32 status = ASYNC_STARTED;
        for (DWORD waited = 0; waited < timeout_ms && !g_quit; waited += 10) {
            if (FAILED(((GetIntFn)VT(info)[ASYNC_INFO_STATUS])(info, &status)) || status != ASYNC_STARTED) break;
            Sleep(10);
        }
        if (status == ASYNC_COMPLETED && wants_result) result = get_ptr(op, ASYNC_RESULTS);
        release(info);
    }
    release(op);
    return result;
}

/// Spotify's session when it has one open, otherwise the one Windows calls current.
static void *pick_session(void *manager, bool *spotify) {
    *spotify = false;
    void *list = get_ptr(manager, MANAGER_SESSIONS);
    UINT32 count = 0;
    if (list && SUCCEEDED(((GetUintFn)VT(list)[VECTOR_SIZE])(list, &count))) {
        for (UINT32 i = 0; i < count; i++) {
            void *session = NULL;
            if (FAILED(((GetAtFn)VT(list)[VECTOR_GET_AT])(list, i, &session)) || !session) continue;
            char app[256]; get_string(session, SESSION_APP_ID, app, sizeof app);
            if (str_icontains(app, "spotify")) { release(list); *spotify = true; return session; }
            release(session);
        }
    }
    release(list);
    return get_ptr(manager, MANAGER_CURRENT);
}

static void read_state(void *manager, MediaState *st) {
    memset(st, 0, sizeof *st);
    void *session = pick_session(manager, &st->spotify);
    if (!session) return;
    st->available = true;
    void *props = await(get_ptr(session, SESSION_PROPERTIES), true, 2000);
    if (props) {
        get_string(props, PROPS_TITLE, st->title, sizeof st->title);
        get_string(props, PROPS_ARTIST, st->artist, sizeof st->artist);
        if (!st->artist[0]) get_string(props, PROPS_ALBUM_ARTIST, st->artist, sizeof st->artist);
        release(props);
    }
    void *playback = get_ptr(session, SESSION_PLAYBACK);
    if (playback) {
        INT32 status = 0;
        if (SUCCEEDED(((GetIntFn)VT(playback)[PLAYBACK_STATUS])(playback, &status))) st->playing = status == STATUS_PLAYING;
        void *controls = get_ptr(playback, PLAYBACK_CONTROLS);
        st->can_previous = get_bool(controls, CONTROLS_PREVIOUS);
        st->can_next = get_bool(controls, CONTROLS_NEXT);
        st->can_toggle = get_bool(controls, CONTROLS_TOGGLE) || get_bool(controls, st->playing ? CONTROLS_PAUSE : CONTROLS_PLAY);
        release(controls);
        release(playback);
    }
    release(session);
}

static void run_command(void *manager, MediaCommand command) {
    bool spotify;
    void *session = pick_session(manager, &spotify);
    if (!session) return;
    bool playing = false;
    if (command == MEDIA_TOGGLE) {
        void *playback = get_ptr(session, SESSION_PLAYBACK);
        INT32 status = 0;
        if (playback && SUCCEEDED(((GetIntFn)VT(playback)[PLAYBACK_STATUS])(playback, &status))) playing = status == STATUS_PLAYING;
        release(playback);
    }
    int slot = command == MEDIA_PREVIOUS ? SESSION_PREVIOUS : command == MEDIA_NEXT ? SESSION_NEXT : playing ? SESSION_PAUSE : SESSION_PLAY;
    await(get_ptr(session, slot), false, 2000);
    release(session);
}

static void *open_manager(void) {
    HMODULE combase = LoadLibraryW(L"combase.dll");
    if (!combase) return NULL;
    ro_get_factory = (void *)GetProcAddress(combase, "RoGetActivationFactory");
    hstr_create = (void *)GetProcAddress(combase, "WindowsCreateString");
    hstr_delete = (void *)GetProcAddress(combase, "WindowsDeleteString");
    hstr_buffer = (void *)GetProcAddress(combase, "WindowsGetStringRawBuffer");
    if (!ro_get_factory || !hstr_create || !hstr_delete || !hstr_buffer) return NULL;
    WinString name = NULL;
    if (FAILED(hstr_create(MANAGER_CLASS, (UINT32)wcslen(MANAGER_CLASS), &name))) return NULL;
    void *statics = NULL;
    HRESULT hr = ro_get_factory(name, &IID_ManagerStatics, &statics);
    hstr_delete(name);
    if (FAILED(hr) || !statics) return NULL;
    void *manager = await(get_ptr(statics, STATICS_REQUEST), true, 5000);
    release(statics);
    return manager;
}

static DWORD WINAPI watch(void *arg) {
    (void)arg;
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    void *manager = open_manager();
    while (manager && !g_quit) {
        EnterCriticalSection(&g_lock);
        MediaCommand pending[8]; int count = g_pending_count;
        memcpy(pending, g_pending, sizeof pending); g_pending_count = 0;
        LeaveCriticalSection(&g_lock);
        for (int i = 0; i < count && !g_quit; i++) run_command(manager, pending[i]);
        // A player takes a moment to report what a command did.
        if (count) Sleep(250);
        MediaState st; read_state(manager, &st);
        EnterCriticalSection(&g_lock);
        bool changed = memcmp(&st, &g_state, sizeof st) != 0;
        if (changed) g_state = st;
        LeaveCriticalSection(&g_lock);
        if (changed && g_notify) PostMessageW(g_notify, WM_APP_MEDIA_CHANGED, 0, 0);
        WaitForSingleObject(g_wake, g_active ? 1000 : 5000);
    }
    release(manager);
    CoUninitialize();
    return 0;
}

void media_start(HWND notify) {
    if (g_thread) return;
    InitializeCriticalSection(&g_lock);
    g_notify = notify;
    g_quit = 0;
    g_wake = CreateEventW(NULL, FALSE, FALSE, NULL);
    g_thread = CreateThread(NULL, 0, watch, NULL, 0, NULL);
}

void media_stop(void) {
    if (!g_thread) return;
    InterlockedExchange(&g_quit, 1);
    SetEvent(g_wake);
    // Every wait on the player is bounded, so the watcher is out within a few seconds; past that it is left to the exit.
    WaitForSingleObject(g_thread, 3000);
    CloseHandle(g_thread); CloseHandle(g_wake);
    g_thread = g_wake = NULL;
    g_notify = NULL;
}

void media_set_active(bool active) {
    LONG was = InterlockedExchange(&g_active, active ? 1 : 0);
    if (active && !was && g_wake) SetEvent(g_wake);
}

void media_state(MediaState *out) {
    if (!g_thread) { memset(out, 0, sizeof *out); return; }
    EnterCriticalSection(&g_lock);
    *out = g_state;
    LeaveCriticalSection(&g_lock);
}

void media_command(MediaCommand command) {
    if (!g_thread) return;
    EnterCriticalSection(&g_lock);
    if (g_pending_count < (int)(sizeof g_pending / sizeof g_pending[0])) g_pending[g_pending_count++] = command;
    // Shown at once; the watcher corrects it if the player did otherwise.
    if (command == MEDIA_TOGGLE) g_state.playing = !g_state.playing;
    LeaveCriticalSection(&g_lock);
    SetEvent(g_wake);
}
