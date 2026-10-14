#include "screen_browser_internal.h"

typedef struct { Feed *feed; IWICImagingFactory *wic; } FeedReader;

static void feed_release(Feed *f);
static void feed_post(Feed *f);
static unsigned char *decode_picture(IWICImagingFactory *wic, const unsigned char *bytes, size_t len, int *w, int *h);
static void feed_event(void *ctx, const char *event, const char *data, size_t len);
static DWORD WINAPI feed_thread(LPVOID p);

// Windows Imaging Component, by identifier: no import library beyond ole32 is needed.
static const GUID clsid_wic_factory = { 0xcacaf262, 0x9370, 0x4615, { 0xa1, 0x3b, 0x9f, 0x55, 0x39, 0xda, 0x4c, 0x0a } };

static const GUID iid_wic_factory = { 0xec5ec8a9, 0xc395, 0x4314, { 0x9c, 0x77, 0x54, 0xd7, 0xa9, 0x35, 0xff, 0x70 } };

static const GUID wic_bgra = { 0x6fddc324, 0x4e03, 0x4bfe, { 0xb1, 0x85, 0x3d, 0x77, 0x76, 0x8d, 0xc9, 0x0f } };

static void feed_release(Feed *f) {
    if (InterlockedDecrement(&f->refs) > 0) return;
    store_stream_free(f->stream);
    DeleteCriticalSection(&f->lock);
    json_free(f->tabs); free(f->pixels);
    api_error_clear(&f->error);
    free(f);
}

/// With the lock held.
static void feed_post(Feed *f) {
    if (!f->posted && f->notify) f->posted = PostMessageW(f->notify, WM_BROWSER_FEED, 0, 0) != 0;
}

static unsigned char *decode_picture(IWICImagingFactory *wic, const unsigned char *bytes, size_t len, int *w, int *h) {
    unsigned char *pixels = NULL;
    IWICStream *stream = NULL; IWICBitmapDecoder *decoder = NULL; IWICBitmapFrameDecode *frame = NULL; IWICFormatConverter *converter = NULL;
    UINT width = 0, height = 0;
    if (!wic || !len || len > 0xFFFFFFFFu) return NULL;
    if (FAILED(wic->lpVtbl->CreateStream(wic, &stream))) goto out;
    if (FAILED(stream->lpVtbl->InitializeFromMemory(stream, (BYTE *)bytes, (DWORD)len))) goto out;
    if (FAILED(wic->lpVtbl->CreateDecoderFromStream(wic, (IStream *)stream, NULL, WICDecodeMetadataCacheOnDemand, &decoder))) goto out;
    if (FAILED(decoder->lpVtbl->GetFrame(decoder, 0, &frame))) goto out;
    if (FAILED(wic->lpVtbl->CreateFormatConverter(wic, &converter))) goto out;
    if (FAILED(converter->lpVtbl->Initialize(converter, (IWICBitmapSource *)frame, &wic_bgra, WICBitmapDitherTypeNone, NULL, 0, WICBitmapPaletteTypeCustom))) goto out;
    if (FAILED(converter->lpVtbl->GetSize(converter, &width, &height)) || !width || !height || width > MAX_SIDE || height > MAX_SIDE) goto out;
    pixels = xmalloc((size_t)width * height * 4);
    if (FAILED(converter->lpVtbl->CopyPixels(converter, NULL, width * 4, width * height * 4, pixels))) { free(pixels); pixels = NULL; goto out; }
    *w = (int)width; *h = (int)height;
out:
    if (converter) converter->lpVtbl->Release(converter);
    if (frame) frame->lpVtbl->Release(frame);
    if (decoder) decoder->lpVtbl->Release(decoder);
    if (stream) stream->lpVtbl->Release(stream);
    return pixels;
}

static void feed_event(void *ctx, const char *event, const char *data, size_t len) {
    FeedReader *r = ctx;
    Feed *f = r->feed;
    if (str_eq(event, "frame")) {
        Json *j = json_parse(data, len);
        const char *b64 = json_str(json_get(j, "data"));
        int fw = json_int_or(json_get(j, "width"), 0), fh = json_int_or(json_get(j, "height"), 0);
        size_t n = 0; int pw = 0, ph = 0;
        unsigned char *bytes = b64 ? base64_decode(b64, strlen(b64), &n) : NULL;
        unsigned char *pixels = bytes ? decode_picture(r->wic, bytes, n, &pw, &ph) : NULL;
        free(bytes); json_free(j);
        if (!pixels) return;
        EnterCriticalSection(&f->lock);
        free(f->pixels);
        f->pixels = pixels; f->pw = pw; f->ph = ph; f->fw = fw > 0 ? fw : pw; f->fh = fh > 0 ? fh : ph;
        f->heard = true; feed_post(f);
        LeaveCriticalSection(&f->lock);
    } else if (str_eq(event, "tabs")) {
        Json *j = json_parse(data, len);
        if (!j) return;
        EnterCriticalSection(&f->lock);
        json_free(f->tabs); f->tabs = j;
        f->heard = true; feed_post(f);
        LeaveCriticalSection(&f->lock);
    } else if (str_eq(event, "closed")) {
        EnterCriticalSection(&f->lock);
        f->closed = true; feed_post(f);
        LeaveCriticalSection(&f->lock);
    }
}

static DWORD WINAPI feed_thread(LPVOID p) {
    Feed *f = p;
    HRESULT co = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    FeedReader r = { f, NULL };
    if (FAILED(CoCreateInstance(&clsid_wic_factory, NULL, CLSCTX_INPROC_SERVER, &iid_wic_factory, (void **)&r.wic))) r.wic = NULL;
    ApiError error; api_error_init(&error);
    store_stream_read(f->stream, feed_event, &r, &error);
    EnterCriticalSection(&f->lock);
    f->ended = true; api_error_copy(&f->error, &error);
    feed_post(f);
    LeaveCriticalSection(&f->lock);
    api_error_clear(&error);
    if (r.wic) r.wic->lpVtbl->Release(r.wic);
    if (SUCCEEDED(co)) CoUninitialize();
    feed_release(f);
    return 0;
}

Feed *screen_browser_feed_start(const char *session_id, HWND notify) {
    if (!g_store.client) return NULL;
    Feed *f = xcalloc(1, sizeof *f);
    f->refs = 2;
    InitializeCriticalSection(&f->lock);
    api_error_init(&f->error);
    Json *args = json_object(); json_set_str(args, "sessionId", session_id);
    f->stream = store_stream_new("browser_stream", args, &f->error);
    if (!f->stream) { DeleteCriticalSection(&f->lock); api_error_clear(&f->error); free(f); return NULL; }
    f->notify = notify;
    HANDLE thread = CreateThread(NULL, 0, feed_thread, f, 0, NULL);
    if (!thread) { f->refs = 1; feed_release(f); return NULL; }
    CloseHandle(thread);
    return f;
}

/// Lets go of the feed: nothing more is posted, and the read under way stops.
void screen_browser_feed_stop(Feed *f) {
    if (!f) return;
    EnterCriticalSection(&f->lock);
    f->notify = NULL;
    LeaveCriticalSection(&f->lock);
    store_stream_cancel(f->stream);
    feed_release(f);
}
