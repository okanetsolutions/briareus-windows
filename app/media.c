// The system's media sessions, read and driven on a thread of their own. MinGW ships no header for Windows.Media.Control,
// so its interfaces are called through their vtables by slot, in the order of Windows.Media.winmd (as windows-rs lists
// them), and combase's WinRT functions are looked up at run time. Asynchronous calls are waited on by polling IAsyncInfo,
// which needs none of the parameterized interface ids a completion handler would. Volume is Core Audio's: the player
// app's own sessions in the volume mixer, or the speakers' when none of its sessions can be told apart.
#define COBJMACROS
#include "media.h"
#include "str.h"
#include <objbase.h>
#include <initguid.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <endpointvolume.h>
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
typedef HRESULT (WINAPI *RoGetFactoryFn)(WinString, REFIID, void **);
typedef HRESULT (WINAPI *HstrCreateFn)(const wchar_t *, UINT32, WinString *);
typedef HRESULT (WINAPI *HstrDeleteFn)(WinString);
typedef const wchar_t *(WINAPI *HstrBufferFn)(WinString, UINT32 *);
static RoGetFactoryFn ro_get_factory;
static HstrCreateFn hstr_create;
static HstrDeleteFn hstr_delete;
static HstrBufferFn hstr_buffer;

static CRITICAL_SECTION g_lock;
static MediaState g_state;
static HWND g_notify;
static HANDLE g_thread, g_wake;
static volatile LONG g_quit, g_active = 1;
static MediaCommand g_pending[8];
static int g_pending_count;
static float g_pending_volume = -1;     // the latest volume asked for and not yet set, or -1
static IMMDeviceEnumerator *g_devices;

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

// MARK: - Volume

/// True when the process's executable is the app a media session names: Spotify.exe for "Spotify.exe" or the Store's
/// "SpotifyAB.SpotifyMusic_...!Spotify", chrome.exe for "Chrome", msedge.exe for "MSEdge".
static bool process_is_app(DWORD pid, const char *app) {
    if (!pid || !app[0]) return false;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return false;
    wchar_t path[MAX_PATH]; DWORD n = MAX_PATH;
    bool ok = QueryFullProcessImageNameW(process, 0, path, &n) != 0;
    CloseHandle(process);
    if (!ok) return false;
    const wchar_t *base = wcsrchr(path, L'\\'); base = base ? base + 1 : path;
    char *name = wide_to_utf8(base);
    if (!name) return false;
    char *dot = strrchr(name, '.'); if (dot) *dot = 0;
    bool match = strlen(name) >= 3 && str_icontains(app, name);
    free(name);
    return match;
}

/// The default speakers, or NULL.
static IMMDevice *speakers(void) {
    if (!g_devices && FAILED(CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL, &IID_IMMDeviceEnumerator, (void **)&g_devices))) g_devices = NULL;
    IMMDevice *device = NULL;
    if (g_devices && FAILED(IMMDeviceEnumerator_GetDefaultAudioEndpoint(g_devices, eRender, eMultimedia, &device))) device = NULL;
    return device;
}

/// Calls `fn` on the volume of each of the app's sessions on the speakers; how many there were.
static int each_app_volume(const char *app, void (*fn)(ISimpleAudioVolume *, void *), void *ctx) {
    IMMDevice *device = speakers();
    if (!device) return 0;
    int found = 0;
    IAudioSessionManager2 *manager = NULL;
    IAudioSessionEnumerator *list = NULL;
    if (SUCCEEDED(IMMDevice_Activate(device, &IID_IAudioSessionManager2, CLSCTX_ALL, NULL, (void **)&manager))
        && SUCCEEDED(IAudioSessionManager2_GetSessionEnumerator(manager, &list))) {
        int count = 0; IAudioSessionEnumerator_GetCount(list, &count);
        for (int i = 0; i < count; i++) {
            IAudioSessionControl *control = NULL; IAudioSessionControl2 *control2 = NULL; ISimpleAudioVolume *volume = NULL;
            if (FAILED(IAudioSessionEnumerator_GetSession(list, i, &control))) continue;
            DWORD pid = 0;
            if (SUCCEEDED(IAudioSessionControl_QueryInterface(control, &IID_IAudioSessionControl2, (void **)&control2))) {
                IAudioSessionControl2_GetProcessId(control2, &pid);
                IAudioSessionControl2_Release(control2);
            }
            if (process_is_app(pid, app) && SUCCEEDED(IAudioSessionControl_QueryInterface(control, &IID_ISimpleAudioVolume, (void **)&volume))) {
                fn(volume, ctx);
                ISimpleAudioVolume_Release(volume);
                found++;
            }
            IAudioSessionControl_Release(control);
        }
    }
    if (list) IAudioSessionEnumerator_Release(list);
    if (manager) IAudioSessionManager2_Release(manager);
    IMMDevice_Release(device);
    return found;
}

/// The speakers' own volume, when the app has no session of its own to set.
static IAudioEndpointVolume *speaker_volume(void) {
    IMMDevice *device = speakers();
    if (!device) return NULL;
    IAudioEndpointVolume *volume = NULL;
    if (FAILED(IMMDevice_Activate(device, &IID_IAudioEndpointVolume, CLSCTX_ALL, NULL, (void **)&volume))) volume = NULL;
    IMMDevice_Release(device);
    return volume;
}

static void read_app_volume(ISimpleAudioVolume *v, void *ctx) {
    MediaState *st = ctx;
    float level = 0; BOOL muted = FALSE;
    ISimpleAudioVolume_GetMasterVolume(v, &level);
    ISimpleAudioVolume_GetMute(v, &muted);
    // A browser has a session per tab; the loudest one stands for the app, which is muted only when all of them are.
    if (!st->has_volume || level > st->volume) st->volume = level;
    st->muted = st->has_volume ? st->muted && muted : muted;
    st->has_volume = true;
}
static void read_volume(const char *app, MediaState *st) {
    if (each_app_volume(app, read_app_volume, st)) { st->app_volume = true; return; }
    IAudioEndpointVolume *v = speaker_volume();
    if (!v) return;
    BOOL muted = FALSE;
    if (SUCCEEDED(IAudioEndpointVolume_GetMasterVolumeLevelScalar(v, &st->volume)) && SUCCEEDED(IAudioEndpointVolume_GetMute(v, &muted))) {
        st->has_volume = true; st->muted = muted;
    }
    IAudioEndpointVolume_Release(v);
}

static void set_app_level(ISimpleAudioVolume *v, void *ctx) { ISimpleAudioVolume_SetMasterVolume(v, *(float *)ctx, NULL); ISimpleAudioVolume_SetMute(v, FALSE, NULL); }
static void set_app_mute(ISimpleAudioVolume *v, void *ctx) { ISimpleAudioVolume_SetMute(v, *(BOOL *)ctx, NULL); }

/// Sets the volume (0...1), which also unmutes, or with a negative level flips the mute.
static void apply_volume(const char *app, float level) {
    if (level >= 0) {
        if (each_app_volume(app, set_app_level, &level)) return;
        IAudioEndpointVolume *v = speaker_volume();
        if (v) { IAudioEndpointVolume_SetMasterVolumeLevelScalar(v, level, NULL); IAudioEndpointVolume_SetMute(v, FALSE, NULL); IAudioEndpointVolume_Release(v); }
        return;
    }
    MediaState now; memset(&now, 0, sizeof now);
    read_volume(app, &now);
    if (!now.has_volume) return;
    BOOL mute = !now.muted;
    if (now.app_volume) each_app_volume(app, set_app_mute, &mute);
    else { IAudioEndpointVolume *v = speaker_volume(); if (v) { IAudioEndpointVolume_SetMute(v, mute, NULL); IAudioEndpointVolume_Release(v); } }
}

static void read_state(void *manager, MediaState *st) {
    memset(st, 0, sizeof *st);
    void *session = pick_session(manager, &st->spotify);
    if (!session) return;
    st->available = true;
    char app[256]; get_string(session, SESSION_APP_ID, app, sizeof app);
    read_volume(app, st);
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

static void run_command(void *manager, MediaCommand command, float level) {
    bool spotify;
    void *session = pick_session(manager, &spotify);
    if (!session) return;
    if (command == MEDIA_MUTE || command == MEDIA_VOLUME) {
        char app[256]; get_string(session, SESSION_APP_ID, app, sizeof app);
        apply_volume(app, command == MEDIA_MUTE ? -1 : level);
        release(session);
        return;
    }
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
    ro_get_factory = (RoGetFactoryFn)(void *)GetProcAddress(combase, "RoGetActivationFactory");
    hstr_create = (HstrCreateFn)(void *)GetProcAddress(combase, "WindowsCreateString");
    hstr_delete = (HstrDeleteFn)(void *)GetProcAddress(combase, "WindowsDeleteString");
    hstr_buffer = (HstrBufferFn)(void *)GetProcAddress(combase, "WindowsGetStringRawBuffer");
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
        float level = g_pending_volume; g_pending_volume = -1;
        LeaveCriticalSection(&g_lock);
        if (level >= 0 && !g_quit) run_command(manager, MEDIA_VOLUME, level);
        bool transport = false;
        for (int i = 0; i < count && !g_quit; i++) { run_command(manager, pending[i], 0); transport |= pending[i] != MEDIA_MUTE; }
        // A player takes a moment to report what ⏮ ⏯ ⏭ did; a volume reads back at once.
        if (transport) Sleep(250);
        MediaState st; read_state(manager, &st);
        EnterCriticalSection(&g_lock);
        // A drag that moved on since keeps showing where it is, not a level it passed through.
        if (g_pending_volume >= 0) { st.volume = g_pending_volume; st.muted = false; }
        bool changed = memcmp(&st, &g_state, sizeof st) != 0;
        if (changed) g_state = st;
        LeaveCriticalSection(&g_lock);
        if (changed && g_notify) PostMessageW(g_notify, WM_APP_MEDIA_CHANGED, 0, 0);
        WaitForSingleObject(g_wake, g_active ? 1000 : 5000);
    }
    release(manager);
    if (g_devices) { IMMDeviceEnumerator_Release(g_devices); g_devices = NULL; }
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
    if (command == MEDIA_MUTE) g_state.muted = !g_state.muted;
    LeaveCriticalSection(&g_lock);
    SetEvent(g_wake);
}

void media_set_volume(float level) {
    if (!g_thread) return;
    if (level < 0) level = 0;
    if (level > 1) level = 1;
    EnterCriticalSection(&g_lock);
    // Only the latest level of a drag is set.
    g_pending_volume = level;
    g_state.volume = level; g_state.muted = false;
    LeaveCriticalSection(&g_lock);
    SetEvent(g_wake);
}
