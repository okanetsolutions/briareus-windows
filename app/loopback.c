#define COBJMACROS
#include "loopback.h"
#include "str.h"
#include <process.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>

// audioclientactivationparams.h, which MinGW lacks: the process loopback request.
typedef struct { DWORD TargetProcessId; int ProcessLoopbackMode; } LoopbackParams;
typedef struct { int ActivationType; LoopbackParams ProcessLoopbackParams; } ActivationParams;
enum { ACTIVATION_TYPE_PROCESS_LOOPBACK = 1, LOOPBACK_MODE_INCLUDE_TREE = 0, LOOPBACK_MODE_EXCLUDE_TREE = 1 };
static const wchar_t PROCESS_LOOPBACK_DEVICE[] = L"VAD\\Process_Loopback";

// The interfaces' ids, here so no import library has to carry them.
static const IID ID_IUnknown = { 0x00000000, 0x0000, 0x0000, { 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46 } };
static const IID ID_IAgileObject = { 0x94ea2b94, 0xe9cc, 0x49e0, { 0xc0, 0xff, 0xee, 0x64, 0xca, 0x8f, 0x5b, 0x90 } };
static const IID ID_IAudioClient = { 0x1CB9AD4C, 0xDBFA, 0x4c32, { 0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2 } };
static const IID ID_IAudioCaptureClient = { 0xC8ADBD64, 0xE71E, 0x48a0, { 0xA4, 0xDE, 0x18, 0x5C, 0x39, 0x5C, 0xD3, 0x17 } };
static const IID ID_ICompletionHandler = { 0x41D949AB, 0x9862, 0x444A, { 0x80, 0xF6, 0xC2, 0x61, 0x33, 0x4D, 0xA5, 0xEB } };

#define RATE 24000

struct Loopback {
    unsigned pid; bool exclude;
    LoopbackFrames frames; void *ctx;
    HANDLE thread, started, stop;
    char *error;
    volatile LONG running;
};

// MARK: - The activation's completion

/// The handler ActivateAudioInterfaceAsync calls back on a thread of its own, so it says it is agile.
typedef struct { IActivateAudioInterfaceCompletionHandler iface; LONG refs; HANDLE done; HRESULT result; IAudioClient *client; } Handler;
static HRESULT STDMETHODCALLTYPE h_query(IActivateAudioInterfaceCompletionHandler *self, REFIID riid, void **out) {
    if (IsEqualIID(riid, &ID_IUnknown) || IsEqualIID(riid, &ID_IAgileObject) || IsEqualIID(riid, &ID_ICompletionHandler)) {
        *out = self; InterlockedIncrement(&((Handler *)self)->refs); return S_OK;
    }
    *out = NULL; return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE h_addref(IActivateAudioInterfaceCompletionHandler *self) { return (ULONG)InterlockedIncrement(&((Handler *)self)->refs); }
// Freed by whoever lets go last: the waiting thread, or Windows calling back after the wait gave up.
static ULONG STDMETHODCALLTYPE h_release(IActivateAudioInterfaceCompletionHandler *self) {
    Handler *h = (Handler *)self;
    LONG left = InterlockedDecrement(&h->refs);
    if (!left) { if (h->client) IAudioClient_Release(h->client); CloseHandle(h->done); free(h); }
    return (ULONG)left;
}
static HRESULT STDMETHODCALLTYPE h_completed(IActivateAudioInterfaceCompletionHandler *self, IActivateAudioInterfaceAsyncOperation *op) {
    Handler *h = (Handler *)self;
    IUnknown *unknown = NULL;
    HRESULT result = E_FAIL;
    HRESULT hr = IActivateAudioInterfaceAsyncOperation_GetActivateResult(op, &result, &unknown);
    if (SUCCEEDED(hr) && SUCCEEDED(result) && unknown) result = IUnknown_QueryInterface(unknown, &ID_IAudioClient, (void **)&h->client);
    else if (SUCCEEDED(hr)) hr = result;
    if (unknown) IUnknown_Release(unknown);
    h->result = FAILED(hr) ? hr : result;
    SetEvent(h->done);
    return S_OK;
}
static IActivateAudioInterfaceCompletionHandlerVtbl HANDLER_VTBL = { h_query, h_addref, h_release, h_completed };

typedef HRESULT (WINAPI *ActivateFn)(LPCWSTR, REFIID, PROPVARIANT *, IActivateAudioInterfaceCompletionHandler *, IActivateAudioInterfaceAsyncOperation **);

static IAudioClient *activate(unsigned pid, bool exclude, HRESULT *why) {
    static ActivateFn activate_async;
    if (!activate_async) {
        HMODULE dll = LoadLibraryW(L"mmdevapi.dll");
        activate_async = dll ? (ActivateFn)(void *)GetProcAddress(dll, "ActivateAudioInterfaceAsync") : NULL;
        if (!activate_async) { *why = E_NOTIMPL; return NULL; }
    }
    ActivationParams params = { ACTIVATION_TYPE_PROCESS_LOOPBACK, { pid, exclude ? LOOPBACK_MODE_EXCLUDE_TREE : LOOPBACK_MODE_INCLUDE_TREE } };
    PROPVARIANT pv; memset(&pv, 0, sizeof pv);
    pv.vt = VT_BLOB;
    pv.blob.cbSize = sizeof params;
    pv.blob.pBlobData = (BYTE *)&params;
    Handler *h = xcalloc(1, sizeof *h);
    h->iface.lpVtbl = &HANDLER_VTBL; h->refs = 1; h->done = CreateEventW(NULL, TRUE, FALSE, NULL); h->result = E_FAIL;
    IActivateAudioInterfaceAsyncOperation *op = NULL;
    IAudioClient *client = NULL;
    HRESULT hr = activate_async(PROCESS_LOOPBACK_DEVICE, &ID_IAudioClient, &pv, &h->iface, &op);
    if (SUCCEEDED(hr)) {
        // Windows calls back once the activation is through; never more than a moment on a working system.
        hr = WaitForSingleObject(h->done, 5000) == WAIT_OBJECT_0 ? h->result : HRESULT_FROM_WIN32(ERROR_TIMEOUT);
        if (SUCCEEDED(hr)) { client = h->client; h->client = NULL; }
    }
    if (op) IActivateAudioInterfaceAsyncOperation_Release(op);
    h_release(&h->iface);
    *why = hr;
    return client;
}

// MARK: - Capture

/// Process loopback takes the format it is asked for; 24 kHz mono is tried first, then CD-quality stereo that is brought
/// down to it here.
static HRESULT open_stream(IAudioClient *client, HANDLE event, WAVEFORMATEX *wf) {
    const DWORD flags = AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
    static const struct { DWORD rate; WORD channels; } TRIES[] = { { RATE, 1 }, { 48000, 2 }, { 44100, 2 } };
    HRESULT hr = E_FAIL;
    for (size_t i = 0; i < sizeof TRIES / sizeof *TRIES; i++) {
        memset(wf, 0, sizeof *wf);
        wf->wFormatTag = WAVE_FORMAT_PCM; wf->nChannels = TRIES[i].channels; wf->nSamplesPerSec = TRIES[i].rate; wf->wBitsPerSample = 16;
        wf->nBlockAlign = (WORD)(wf->nChannels * 2); wf->nAvgBytesPerSec = wf->nSamplesPerSec * wf->nBlockAlign;
        hr = IAudioClient_Initialize(client, AUDCLNT_SHAREMODE_SHARED, flags, 2000000, 0, wf, NULL);
        if (SUCCEEDED(hr)) return IAudioClient_SetEventHandle(client, event);
    }
    return hr;
}

/// Mono at 24 kHz from whatever came: channels averaged, then stepped through at the rate's ratio.
typedef struct { double pos; } Resampler;
static size_t convert(Resampler *r, const int16_t *in, size_t frames, const WAVEFORMATEX *wf, int16_t *out, size_t cap) {
    size_t n = 0;
    double step = (double)wf->nSamplesPerSec / RATE;
    for (; r->pos < (double)frames && n < cap; r->pos += step) {
        size_t f = (size_t)r->pos;
        int sum = 0;
        for (WORD c = 0; c < wf->nChannels; c++) sum += in[f * wf->nChannels + c];
        out[n++] = (int16_t)(sum / wf->nChannels);
    }
    r->pos -= (double)frames;
    return n;
}

static unsigned __stdcall capture_main(void *arg) {
    Loopback *l = arg;
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    HRESULT hr;
    IAudioClient *client = activate(l->pid, l->exclude, &hr);
    IAudioCaptureClient *capture = NULL;
    HANDLE event = CreateEventW(NULL, FALSE, FALSE, NULL);
    WAVEFORMATEX wf;
    if (client) hr = open_stream(client, event, &wf);
    if (client && SUCCEEDED(hr)) hr = IAudioClient_GetService(client, &ID_IAudioCaptureClient, (void **)&capture);
    if (capture) hr = IAudioClient_Start(client);
    if (FAILED(hr)) {
        l->error = xstrfmt("The meeting's sound could not be captured (0x%08lX). Per-app capture needs Windows 10 version 2004 or later.", (unsigned long)hr);
        SetEvent(l->started);
    } else {
        InterlockedExchange(&l->running, 1);
        SetEvent(l->started);
        Resampler r = { 0 };
        int16_t out[RATE / 5], silence[RATE / 5];
        memset(silence, 0, sizeof silence);
        HANDLE waits[2] = { l->stop, event };
        while (WaitForMultipleObjects(2, waits, FALSE, 200) != WAIT_OBJECT_0) {
            UINT32 packet = 0;
            while (SUCCEEDED(IAudioCaptureClient_GetNextPacketSize(capture, &packet)) && packet) {
                BYTE *data; UINT32 frames; DWORD flags;
                if (FAILED(IAudioCaptureClient_GetBuffer(capture, &data, &frames, &flags, NULL, NULL))) break;
                const int16_t *pcm = (flags & AUDCLNT_BUFFERFLAGS_SILENT) ? NULL : (const int16_t *)data;
                if (wf.nSamplesPerSec == RATE && wf.nChannels == 1) {
                    for (UINT32 done = 0; done < frames; ) {
                        UINT32 n = frames - done < RATE / 5 ? frames - done : RATE / 5;
                        l->frames(l->ctx, pcm ? pcm + done : silence, n);
                        done += n;
                    }
                } else if (pcm) {
                    size_t n = convert(&r, pcm, frames, &wf, out, sizeof out / sizeof *out);
                    l->frames(l->ctx, out, n);
                } else {
                    size_t n = (size_t)((double)frames * RATE / wf.nSamplesPerSec);
                    l->frames(l->ctx, silence, n < RATE / 5 ? n : RATE / 5);
                }
                IAudioCaptureClient_ReleaseBuffer(capture, frames);
            }
        }
        IAudioClient_Stop(client);
    }
    if (capture) IAudioCaptureClient_Release(capture);
    if (client) IAudioClient_Release(client);
    CloseHandle(event);
    CoUninitialize();
    return 0;
}

Loopback *loopback_start(unsigned pid, bool exclude, LoopbackFrames frames, void *ctx, char **error) {
    *error = NULL;
    Loopback *l = xcalloc(1, sizeof *l);
    l->pid = pid; l->exclude = exclude; l->frames = frames; l->ctx = ctx;
    l->started = CreateEventW(NULL, TRUE, FALSE, NULL);
    l->stop = CreateEventW(NULL, TRUE, FALSE, NULL);
    l->thread = (HANDLE)_beginthreadex(NULL, 0, capture_main, l, 0, NULL);
    WaitForSingleObject(l->started, 8000);
    if (!l->running) {
        *error = l->error ? l->error : xstrdup("The meeting's sound could not be captured: Windows did not answer.");
        l->error = NULL;
        loopback_stop(l);
        return NULL;
    }
    return l;
}

void loopback_stop(Loopback *l) {
    if (!l) return;
    SetEvent(l->stop);
    if (l->thread) { WaitForSingleObject(l->thread, INFINITE); CloseHandle(l->thread); }
    CloseHandle(l->started); CloseHandle(l->stop);
    free(l->error);
    free(l);
}
