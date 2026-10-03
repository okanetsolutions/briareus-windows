#include "meet_audio.h"
#include "loopback.h"
#include "meet.h"
#include "str.h"
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <tlhelp32.h>
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wundef"
#pragma GCC diagnostic ignored "-Wstrict-prototypes"
#pragma GCC diagnostic ignored "-Wshadow"
#elif defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#define MA_NO_DECODING
#define MA_NO_ENCODING
#define MA_NO_GENERATION
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_NODE_GRAPH
#define MA_NO_ENGINE
#define MA_ENABLE_ONLY_SPECIFIC_BACKENDS
#define MA_ENABLE_WASAPI
#include "../third_party/miniaudio/miniaudio.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#elif defined(_MSC_VER)
#pragma warning(pop)
#endif

// MARK: - Rings

/// One producer, one consumer, under a lock; past `max` samples the oldest go, so a slow reader never adds delay.
typedef struct { int16_t *data; size_t cap, read, count, max; SRWLOCK lock; } Ring;
static void ring_init(Ring *r, size_t cap, size_t max) { r->data = xcalloc(cap, sizeof *r->data); r->cap = cap; r->max = max < cap ? max : cap; r->read = r->count = 0; InitializeSRWLock(&r->lock); }
static void ring_free(Ring *r) { free(r->data); r->data = NULL; }
static void ring_write(Ring *r, const int16_t *pcm, size_t n) {
    AcquireSRWLockExclusive(&r->lock);
    if (n > r->max) { pcm += n - r->max; n = r->max; }
    if (r->count + n > r->max) { size_t drop = r->count + n - r->max; r->read = (r->read + drop) % r->cap; r->count -= drop; }
    size_t at = (r->read + r->count) % r->cap;
    for (size_t i = 0; i < n; i++) r->data[(at + i) % r->cap] = pcm ? pcm[i] : 0;
    r->count += n;
    ReleaseSRWLockExclusive(&r->lock);
}
/// Mixes up to `n` samples into `out` (added, saturating); returns how many there were.
static size_t ring_mix(Ring *r, int16_t *out, size_t n) {
    AcquireSRWLockExclusive(&r->lock);
    size_t take = n < r->count ? n : r->count;
    for (size_t i = 0; i < take; i++) {
        int v = out[i] + r->data[(r->read + i) % r->cap];
        out[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
    }
    r->read = (r->read + take) % r->cap; r->count -= take;
    ReleaseSRWLockExclusive(&r->lock);
    return take;
}
static void ring_clear(Ring *r) { AcquireSRWLockExclusive(&r->lock); r->read = r->count = 0; ReleaseSRWLockExclusive(&r->lock); }
static size_t ring_count(Ring *r) { AcquireSRWLockShared(&r->lock); size_t n = r->count; ReleaseSRWLockShared(&r->lock); return n; }

#define MS(n) ((size_t)MEET_SAMPLE_RATE * (n) / 1000)

struct MeetAudio {
    ma_context context;
    bool has_context;
    Loopback *loopback;
    ma_device mic, cable, monitor;
    bool has_mic, has_cable, has_monitor;
    Ring heard_meeting, heard_mic;   // what the assistant hears
    Ring mic_to_cable;               // the user's voice on its way into the meeting
    Ring said_cable, said_monitor;   // the assistant's voice
    volatile LONG muted;
};

static void on_loopback(void *ctx, const int16_t *pcm, size_t samples) { ring_write(&((MeetAudio *)ctx)->heard_meeting, pcm, samples); }
static void on_mic(ma_device *d, void *out, const void *in, ma_uint32 frames) {
    (void)out;
    MeetAudio *a = d->pUserData;
    ring_write(&a->heard_mic, in, frames);
    ring_write(&a->mic_to_cable, in, frames);
}
static void on_cable(ma_device *d, void *out, const void *in, ma_uint32 frames) {
    (void)in;
    MeetAudio *a = d->pUserData;
    memset(out, 0, frames * sizeof(int16_t));
    ring_mix(&a->mic_to_cable, out, frames);
    ring_mix(&a->said_cable, out, frames);
}
static void on_monitor(ma_device *d, void *out, const void *in, ma_uint32 frames) {
    (void)in;
    memset(out, 0, frames * sizeof(int16_t));
    ring_mix(&((MeetAudio *)d->pUserData)->said_monitor, out, frames);
}

// MARK: - Devices

static bool names_cable(const char *name) { return name && strstr(name, "CABLE") && strstr(name, "VB-Audio"); }

/// Finds the cable's input among the playback devices, and the first real microphone (the default one unless that is
/// the cable's output, which would feed the assistant its own voice).
static bool find_devices(ma_context *ctx, ma_device_id *cable, char **cable_name, ma_device_id *mic, bool *has_mic) {
    ma_device_info *playback = NULL, *capture = NULL;
    ma_uint32 np = 0, nc = 0;
    if (ma_context_get_devices(ctx, &playback, &np, &capture, &nc) != MA_SUCCESS) return false;
    bool found = false;
    for (ma_uint32 i = 0; i < np && !found; i++) {
        if (!names_cable(playback[i].name) || !strstr(playback[i].name, "Input")) continue;
        *cable = playback[i].id; found = true;
        if (cable_name) *cable_name = xstrdup(playback[i].name);
    }
    *has_mic = false;
    if (mic) {
        for (ma_uint32 i = 0; i < nc && !*has_mic; i++) if (capture[i].isDefault && !names_cable(capture[i].name)) { *mic = capture[i].id; *has_mic = true; }
        for (ma_uint32 i = 0; i < nc && !*has_mic; i++) if (!names_cable(capture[i].name)) { *mic = capture[i].id; *has_mic = true; }
    }
    return found;
}

static bool open_context(ma_context *ctx) {
    ma_backend backends[] = { ma_backend_wasapi };
    return ma_context_init(backends, 1, NULL, ctx) == MA_SUCCESS;
}

char *meet_cable_name(void) {
    ma_context ctx;
    if (!open_context(&ctx)) return NULL;
    ma_device_id cable; char *name = NULL; bool has_mic;
    memset(&cable, 0, sizeof cable);
    find_devices(&ctx, &cable, &name, NULL, &has_mic);
    ma_context_uninit(&ctx);
    return name;
}

static ma_device_config device_config(ma_device_type type, ma_device_data_proc proc, MeetAudio *a) {
    ma_device_config c = ma_device_config_init(type);
    c.sampleRate = MEET_SAMPLE_RATE;
    c.capture.format = c.playback.format = ma_format_s16;
    c.capture.channels = c.playback.channels = 1;
    c.dataCallback = proc;
    c.pUserData = a;
    c.periodSizeInMilliseconds = 20;
    return c;
}

MeetAudio *meet_audio_start(unsigned pid, bool monitor, char **error, char **note) {
    *error = NULL; *note = NULL;
    MeetAudio *a = xcalloc(1, sizeof *a);
    ring_init(&a->heard_meeting, MS(5000), MS(2000));
    ring_init(&a->heard_mic, MS(5000), MS(2000));
    ring_init(&a->mic_to_cable, MS(1000), MS(150));
    ring_init(&a->said_cable, MS(120000), MS(120000));
    ring_init(&a->said_monitor, MS(120000), MS(120000));
    if (!open_context(&a->context)) { *error = xstrdup("Windows audio (WASAPI) could not be opened."); goto fail; }
    a->has_context = true;
    ma_device_id cable, mic; bool has_mic = false;
    memset(&cable, 0, sizeof cable); memset(&mic, 0, sizeof mic);
    if (!find_devices(&a->context, &cable, NULL, &mic, &has_mic)) {
        *error = xstrdup("The virtual microphone was not found. Install VB-Cable (free, from vb-audio.com), restart Windows, then pick \xE2\x80\x9C" "CABLE Output\xE2\x80\x9D as the microphone in your meeting app.");
        goto fail;
    }
    ma_device_config c = device_config(ma_device_type_playback, on_cable, a);
    c.playback.pDeviceID = &cable;
    if (ma_device_init(&a->context, &c, &a->cable) != MA_SUCCESS) { *error = xstrdup("The virtual cable could not be opened for playback."); goto fail; }
    a->has_cable = true;
    if (has_mic) {
        c = device_config(ma_device_type_capture, on_mic, a);
        c.capture.pDeviceID = &mic;
        a->has_mic = ma_device_init(&a->context, &c, &a->mic) == MA_SUCCESS;
    }
    if (!a->has_mic) *note = xstrdup("No microphone could be opened: the meeting hears only the assistant.");
    if (monitor) {
        c = device_config(ma_device_type_playback, on_monitor, a);
        a->has_monitor = ma_device_init(&a->context, &c, &a->monitor) == MA_SUCCESS;
        // The default speakers being the cable itself would say everything twice.
        if (a->has_monitor && names_cable(a->monitor.playback.name)) { ma_device_uninit(&a->monitor); a->has_monitor = false; }
    }
    if (ma_device_start(&a->cable) != MA_SUCCESS) { *error = xstrdup("The virtual cable could not start."); goto fail; }
    // Per-app loopback: the meeting app's tree, or every app but this one.
    a->loopback = loopback_start(pid ? pid : GetCurrentProcessId(), pid == 0, on_loopback, a, error);
    if (!a->loopback) goto fail;
    if (a->has_mic) ma_device_start(&a->mic);
    if (a->has_monitor) ma_device_start(&a->monitor);
    return a;
fail:
    meet_audio_stop(a);
    return NULL;
}

void meet_audio_stop(MeetAudio *a) {
    if (!a) return;
    loopback_stop(a->loopback);
    if (a->has_mic) ma_device_uninit(&a->mic);
    if (a->has_cable) ma_device_uninit(&a->cable);
    if (a->has_monitor) ma_device_uninit(&a->monitor);
    if (a->has_context) ma_context_uninit(&a->context);
    ring_free(&a->heard_meeting); ring_free(&a->heard_mic); ring_free(&a->mic_to_cable); ring_free(&a->said_cable); ring_free(&a->said_monitor);
    free(a);
}

void meet_audio_take_input(MeetAudio *a, int16_t *out, size_t samples) {
    memset(out, 0, samples * sizeof *out);
    ring_mix(&a->heard_meeting, out, samples);
    ring_mix(&a->heard_mic, out, samples);
}
void meet_audio_play(MeetAudio *a, const int16_t *pcm, size_t samples) {
    if (a->muted) return;
    ring_write(&a->said_cable, pcm, samples);
    if (a->has_monitor) ring_write(&a->said_monitor, pcm, samples);
}
void meet_audio_flush(MeetAudio *a) { ring_clear(&a->said_cable); ring_clear(&a->said_monitor); }
void meet_audio_set_muted(MeetAudio *a, bool muted) { InterlockedExchange(&a->muted, muted); if (muted) meet_audio_flush(a); }
bool meet_audio_speaking(MeetAudio *a) { return ring_count(&a->said_cable) > MS(60); }

// MARK: - Meeting apps

static const struct { const char *exe, *label; } KNOWN_APPS[] = {
    { "ms-teams.exe", "Microsoft Teams" }, { "teams.exe", "Microsoft Teams (classic)" }, { "zoom.exe", "Zoom" },
    { "webex.exe", "Webex" }, { "ciscocollabhost.exe", "Webex" }, { "slack.exe", "Slack" }, { "discord.exe", "Discord" },
    { "chrome.exe", "Google Chrome (Meet)" }, { "msedge.exe", "Microsoft Edge (Meet)" }, { "firefox.exe", "Firefox (Meet)" },
    { "brave.exe", "Brave (Meet)" },
};

MeetApp *meet_apps_running(size_t *count) {
    *count = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return NULL;
    size_t n = 0, cap = 64;
    PROCESSENTRY32W *all = xmalloc(cap * sizeof *all);
    PROCESSENTRY32W pe = { .dwSize = sizeof pe };
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
        if (n == cap) { cap *= 2; all = xrealloc(all, cap * sizeof *all); }
        all[n++] = pe;
    }
    CloseHandle(snap);
    MeetApp *apps = NULL;
    size_t found = 0;
    for (size_t k = 0; k < sizeof KNOWN_APPS / sizeof *KNOWN_APPS; k++) {
        for (size_t i = 0; i < n; i++) {
            char *exe = wide_to_utf8(all[i].szExeFile);
            bool match = str_ieq(exe, KNOWN_APPS[k].exe);
            free(exe);
            if (!match) continue;
            // The root of the app's tree: its parent is not the same program (a browser's tabs and audio service are its children).
            bool root = true;
            for (size_t j = 0; j < n && root; j++) if (all[j].th32ProcessID == all[i].th32ParentProcessID && !_wcsicmp(all[j].szExeFile, all[i].szExeFile)) root = false;
            if (!root) continue;
            bool same_label = false;
            for (size_t f = 0; f < found; f++) if (str_eq(apps[f].label, KNOWN_APPS[k].label)) same_label = true;
            if (same_label) continue;
            apps = xrealloc(apps, (found + 1) * sizeof *apps);
            apps[found].label = xstrdup(KNOWN_APPS[k].label);
            apps[found].pid = all[i].th32ProcessID;
            found++;
        }
    }
    free(all);
    *count = found;
    return apps;
}
void meet_apps_free(MeetApp *apps, size_t count) { for (size_t i = 0; i < count; i++) free(apps[i].label); free(apps); }
