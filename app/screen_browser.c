// A session's shared browser, as the dashboard's browser panel: the headless Chromium on the server that the session's
// agent drives through Playwright, shown here as the frames its event stream sends and driven with this window's mouse
// and keyboard (`POST …/browser/input`), on the same tabs at the same time. The header goes back, forward, reloads, opens
// and closes tabs and switches the browser on or off; the tabs and an address field sit over the picture.
#include "screens.h"
#include "str.h"
#include <commctrl.h>
#include <math.h>
#include <objidl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wincodec.h>

enum { ACT_START = 1000, ACT_STOP, ACT_BACK, ACT_FORWARD, ACT_RELOAD, ACT_NEW_TAB, ACT_CLOSE_TAB, ACT_TAB };
enum { TIMER_POLL = 1, TIMER_RECONNECT = 2 };
enum { ID_ADDRESS = 401 };
#define WM_BROWSER_FEED (WM_APP + 61)
#define MAX_SIDE 8192

// Windows Imaging Component, by identifier: no import library beyond ole32 is needed.
static const GUID clsid_wic_factory = { 0xcacaf262, 0x9370, 0x4615, { 0xa1, 0x3b, 0x9f, 0x55, 0x39, 0xda, 0x4c, 0x0a } };
static const GUID iid_wic_factory = { 0xec5ec8a9, 0xc395, 0x4314, { 0x9c, 0x77, 0x54, 0xd7, 0xa9, 0x35, 0xff, 0x70 } };
static const GUID wic_bgra = { 0x6fddc324, 0x4e03, 0x4bfe, { 0xb1, 0x85, 0x3d, 0x77, 0x76, 0x8d, 0xc9, 0x0f } };

// MARK: - The feed

/// The event stream, read on a thread of its own and shared with the screen: the latest tabs and picture wait here until
/// the view window takes them, and older ones are dropped. The screen and the thread each hold a reference.
typedef struct {
    volatile LONG refs;
    CRITICAL_SECTION lock;
    ApiStreamCancel cancel;
    ApiClient *client; char *session_id;
    HWND notify;                                // told when something arrived; NULL once the screen let go
    bool posted;                                // a WM_BROWSER_FEED is on its way
    Json *tabs;                                 // the latest `tabs` event
    unsigned char *pixels; int pw, ph, fw, fh;  // the latest picture: BGRA rows, its pixel size and the page's CSS size
    bool heard, closed, ended;                  // an event came; `closed` came; the stream returned
    ApiError error;
} Feed;

static void feed_release(Feed *f) {
    if (InterlockedDecrement(&f->refs) > 0) return;
    api_stream_cancel_free(&f->cancel);
    DeleteCriticalSection(&f->lock);
    api_client_release(f->client);
    json_free(f->tabs); free(f->pixels); free(f->session_id);
    api_error_clear(&f->error);
    free(f);
}
/// With the lock held.
static void feed_post(Feed *f) {
    if (!f->posted && f->notify) f->posted = PostMessageW(f->notify, WM_BROWSER_FEED, 0, 0) != 0;
}

typedef struct { Feed *feed; IWICImagingFactory *wic; } FeedReader;

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
    Json *args = json_object(); json_set_str(args, "sessionId", f->session_id);
    ApiError error; api_error_init(&error);
    api_stream(f->client, "browser_stream", args, &f->cancel, feed_event, &r, &error);
    json_free(args);
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
static Feed *feed_start(const char *session_id, HWND notify) {
    if (!g_store.client) return NULL;
    Feed *f = xcalloc(1, sizeof *f);
    f->refs = 2;
    InitializeCriticalSection(&f->lock);
    api_stream_cancel_init(&f->cancel);
    api_error_init(&f->error);
    f->client = api_client_retain(g_store.client);
    f->session_id = xstrdup(session_id);
    f->notify = notify;
    HANDLE thread = CreateThread(NULL, 0, feed_thread, f, 0, NULL);
    if (!thread) { f->refs = 1; feed_release(f); return NULL; }
    CloseHandle(thread);
    return f;
}
/// Lets go of the feed: nothing more is posted, and the read under way stops.
static void feed_stop(Feed *f) {
    if (!f) return;
    EnterCriticalSection(&f->lock);
    f->notify = NULL;
    LeaveCriticalSection(&f->lock);
    api_stream_cancel(&f->cancel);
    feed_release(f);
}

// MARK: - The screen

typedef struct {
    Screen base;
    char *session_id, *title;
    BrowserState state; bool state_read, starting, stopping, closed_session;
    char *error;                         // the last failure of an action, shown over the picture
    char *state_error;                   // why the state could not be read; gone once it or the stream comes through
    Request *req_state, *req_toggle, *req_input;
    Poller poller;                       // reads the state while the browser is down: the agent's next turn starts it
    Feed *feed; int failures; bool shown;
    HWND view, address; RECT view_rc, address_rc;   // content coordinates from the last layout
    unsigned char *pixels; int pw, ph, fw, fh;      // the picture on show
    BrowserRect drawn;                              // where it was last drawn in the view
    BrowserInputs inputs;
    // The pointer: a press becomes a click when it is let go where it started, else a drag of down, moves and up.
    bool pressing, dragging; int press_x, press_y, clicks; const char *button;
    wchar_t high_surrogate;
} BrowserScreen;

static void read_state(BrowserScreen *s);
static void changed(BrowserScreen *s) { if (s->base.pane) { pane_relayout(s->base.pane); pane_header_changed(s->base.pane); } }
static bool can_drive(void) { return store_can_manage() && store_supports("browser_input"); }

static void show_frame(BrowserScreen *s, unsigned char *pixels, int pw, int ph, int fw, int fh) {
    free(s->pixels);
    s->pixels = pixels; s->pw = pw; s->ph = ph; s->fw = fw; s->fh = fh;
    if (s->view) InvalidateRect(s->view, NULL, FALSE);
}
static void stop_feed(BrowserScreen *s) {
    feed_stop(s->feed); s->feed = NULL;
    if (s->base.pane) KillTimer(pane_hwnd(s->base.pane), TIMER_RECONNECT);
}
static void start_feed(BrowserScreen *s) {
    if (s->feed || !s->shown || !s->state.running || !s->view || !store_supports("browser_stream")) return;
    s->feed = feed_start(s->session_id, s->view);
}
static void set_address(BrowserScreen *s) {
    if (!s->address || GetFocus() == s->address) return;
    const BrowserTab *tab = browser_active_tab(&s->state);
    wchar_t *w = utf8_to_wide(tab && !str_eq(tab->url, "about:blank") ? tab->url : "");
    SetWindowTextW(s->address, w);
    free(w);
}
/// The browser went down: the stream stops and the picture goes; the state is polled until it comes back.
static void went_down(BrowserScreen *s) {
    s->state.running = false;
    stop_feed(s);
    show_frame(s, NULL, 0, 0, 0, 0);
    browser_inputs_free(&s->inputs);
    if (s->shown && !s->poller.running) poller_start(&s->poller, s->base.pane, TIMER_POLL, 5000);
}
static void came_up(BrowserScreen *s) {
    poller_stop(&s->poller);
    s->failures = 0;
    start_feed(s);
    set_address(s);
}

// MARK: - Reading and switching

static void state_done(void *owner, Request *req) {
    BrowserScreen *s = owner;
    bool failed = !req->ok;
    s->state_read = true;
    if (req->ok) {
        s->closed_session = false;
        set_string(&s->state_error, NULL);
        browser_state_read(&s->state, json_get(req->result, "browser"));
        if (s->state.running) came_up(s); else went_down(s);
    } else {
        if (api_error_unauthorized(&req->error)) store_invalidate_credentials(&req->error);
        request_error_into(&s->state_error, req);
        // A read that failed is tried again, sooner or later as the server says.
        if (s->shown && !s->poller.running) poller_start(&s->poller, s->base.pane, TIMER_POLL, 5000);
    }
    poller_finished(&s->poller, failed, req->error.retry_after);
    changed(s);
}
static void read_state(BrowserScreen *s) {
    if (s->req_state || !store_supports("browser")) return;
    Json *a = json_object(); json_set_str(a, "sessionId", s->session_id);
    store_call("browser", a, 0, s, state_done, 0, &s->req_state);
}
static void toggle_done(void *owner, Request *req) {
    BrowserScreen *s = owner;
    bool on = str_eq(req->operation, "browser_on");
    s->starting = s->stopping = false;
    if (!req->ok) {
        request_error_into(&s->error, req);
        // 409: the conversation is closed, and its browser with it until it is reopened.
        if (on && req->error.status == 409) s->closed_session = true;
    } else if (on) {
        set_string(&s->error, NULL);
        browser_state_read(&s->state, json_get(req->result, "browser"));
        s->state.on = true;
        if (s->state.running) came_up(s); else went_down(s);
    } else {
        set_string(&s->error, NULL);
        browser_state_free(&s->state);
        s->state.on = false;
        went_down(s);
    }
    changed(s);
}
static void switch_browser(BrowserScreen *s, bool on) {
    if (s->req_toggle || !store_supports(on ? "browser_on" : "browser_off")) return;
    if (!on && !app_confirm("Switch the browser off?",
                            "The agent loses its browser tools until it is switched on again. Its tabs close; cookies and logins are kept until the conversation is deleted.",
                            "Switch off", false)) return;
    if (on) s->starting = true; else s->stopping = true;
    set_string(&s->error, NULL);
    Json *a = json_object(); json_set_str(a, "sessionId", s->session_id);
    store_call(on ? "browser_on" : "browser_off", a, 0, s, toggle_done, 0, &s->req_toggle);
    changed(s);
}

// MARK: - Input

static void pump(BrowserScreen *s);
static void input_done(void *owner, Request *req) {
    BrowserScreen *s = owner;
    if (!req->ok) {
        if (api_error_unauthorized(&req->error)) store_invalidate_credentials(&req->error);
        // 409: it stopped under us; what was waiting is for a page that is gone.
        if (req->error.status == 409) { browser_inputs_free(&s->inputs); went_down(s); read_state(s); }
        request_error_into(&s->error, req);
        changed(s);
    } else if (s->error) { set_string(&s->error, NULL); changed(s); }
    pump(s);
}
static void pump(BrowserScreen *s) {
    if (s->req_input || !s->state.running) return;
    Json *input = browser_inputs_pop(&s->inputs);
    if (!input) return;
    json_set_str(input, "sessionId", s->session_id);
    store_call("browser_input", input, 0, s, input_done, 0, &s->req_input);
}
static void send_input(BrowserScreen *s, Json *input) {
    if (!can_drive() || !s->state.running) { json_free(input); return; }
    browser_inputs_push(&s->inputs, input);
    pump(s);
}
static Json *simple_input(const char *type) { Json *j = json_object(); json_set_str(j, "type", type); return j; }
static void add_modifiers(Json *input, bool with_shift) {
    Json *mods = json_array();
    if (GetKeyState(VK_MENU) < 0) json_array_push(mods, json_string("alt"));
    if (GetKeyState(VK_CONTROL) < 0) json_array_push(mods, json_string("ctrl"));
    if (GetKeyState(VK_LWIN) < 0 || GetKeyState(VK_RWIN) < 0) json_array_push(mods, json_string("meta"));
    if (with_shift && GetKeyState(VK_SHIFT) < 0) json_array_push(mods, json_string("shift"));
    if (json_count(mods)) json_object_set(input, "modifiers", mods); else json_free(mods);
}
/// A pointer input at a view point, or NULL off the picture.
static Json *pointer_input(BrowserScreen *s, const char *type, int x, int y) {
    double page_x, page_y;
    if (!browser_page_point(&s->drawn, s->fw, s->fh, x, y, &page_x, &page_y)) return NULL;
    Json *j = simple_input(type);
    json_set_num(j, "x", floor(page_x)); json_set_num(j, "y", floor(page_y));
    add_modifiers(j, true);
    return j;
}
static void navigate(BrowserScreen *s) {
    int n = GetWindowTextLengthW(s->address);
    wchar_t *w = xmalloc(((size_t)n + 1) * sizeof *w);
    GetWindowTextW(s->address, w, n + 1);
    char *typed = wide_to_utf8(w); free(w);
    char *url = browser_address(typed);
    free(typed);
    if (!url) { set_string(&s->error, "Enter a web address: http, https or about:blank."); changed(s); return; }
    Json *j = simple_input("navigate"); json_set_str(j, "url", url);
    free(url);
    send_input(s, j);
    SetFocus(s->view);
}
static void paste(BrowserScreen *s) {
    if (!OpenClipboard(s->view)) return;
    HANDLE h = GetClipboardData(CF_UNICODETEXT);
    const wchar_t *w = h ? GlobalLock(h) : NULL;
    char *text = w ? wide_to_utf8(w) : NULL;
    if (w) GlobalUnlock(h);
    CloseClipboard();
    if (text && *text) {
        char *lf = str_replace(text, "\r\n", "\n");
        Json *j = simple_input("type"); json_set_str(j, "text", lf);
        send_input(s, j);
        free(lf);
    }
    free(text);
}

// MARK: - The view

static const wchar_t VIEW_CLASS[] = L"BriareusBrowserView";

static void paint_view(BrowserScreen *s, HWND hwnd, HDC dc) {
    RECT rc; GetClientRect(hwnd, &rc);
    int w = rc.right, h = rc.bottom;
    if (w <= 0 || h <= 0) return;
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, w, h), old = SelectObject(mem, bmp);
    HBRUSH bg = CreateSolidBrush(theme.sunken);
    FillRect(mem, &rc, bg);
    DeleteObject(bg);
    if (s->pixels) {
        s->drawn = browser_fit(w - 2, h - 2, s->pw, s->ph);
        s->drawn.left += 1; s->drawn.right += 1; s->drawn.top += 1; s->drawn.bottom += 1;
        BITMAPINFO bi; memset(&bi, 0, sizeof bi);
        bi.bmiHeader.biSize = sizeof bi.bmiHeader; bi.bmiHeader.biWidth = s->pw; bi.bmiHeader.biHeight = -s->ph;
        bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
        SetStretchBltMode(mem, HALFTONE); SetBrushOrgEx(mem, 0, 0, NULL);
        StretchDIBits(mem, s->drawn.left, s->drawn.top, s->drawn.right - s->drawn.left, s->drawn.bottom - s->drawn.top,
                      0, 0, s->pw, s->ph, s->pixels, &bi, DIB_RGB_COLORS, SRCCOPY);
    } else {
        memset(&s->drawn, 0, sizeof s->drawn);
        const wchar_t *note = s->feed ? L"Waiting for the first picture\x2026" : L"Connecting to the browser\x2026";
        HFONT old_font = SelectObject(mem, font(FONT_BODY));
        SetBkMode(mem, TRANSPARENT); SetTextColor(mem, theme.muted);
        DrawTextW(mem, note, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(mem, old_font);
    }
    // A ring in the accent while keys go to the page.
    HBRUSH ring = CreateSolidBrush(GetFocus() == hwnd ? theme.accent : theme.line);
    FrameRect(mem, &rc, ring);
    DeleteObject(ring);
    BitBlt(dc, 0, 0, w, h, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old); DeleteObject(bmp); DeleteDC(mem);
}

static void drain_feed(BrowserScreen *s) {
    Feed *f = s->feed;
    if (!f) return;
    EnterCriticalSection(&f->lock);
    Json *tabs = f->tabs; f->tabs = NULL;
    unsigned char *pixels = f->pixels; f->pixels = NULL;
    int pw = f->pw, ph = f->ph, fw = f->fw, fh = f->fh;
    bool heard = f->heard, closed = f->closed, ended = f->ended;
    ApiError error; api_error_init(&error); api_error_copy(&error, &f->error);
    f->posted = false;
    LeaveCriticalSection(&f->lock);
    if (heard) {
        s->failures = 0;
        if (s->state_error) { set_string(&s->state_error, NULL); changed(s); }
    }
    if (pixels) show_frame(s, pixels, pw, ph, fw, fh);
    if (tabs) {
        browser_state_read(&s->state, tabs);
        json_free(tabs);
        set_address(s);
        changed(s);
    }
    if (closed) {
        went_down(s);
        read_state(s);
        changed(s);
    } else if (ended) {
        stop_feed(s);
        if (api_error_unauthorized(&error)) store_invalidate_credentials(&error);
        else if (error.kind == API_HTTP && error.status == 409) { went_down(s); read_state(s); changed(s); }
        else if (s->shown && s->base.pane) {
            // A dropped connection: back after a pause that grows while it keeps failing.
            SetTimer(pane_hwnd(s->base.pane), TIMER_RECONNECT, (UINT)poll_delay_ms(1000, s->failures++, error.retry_after), NULL);
        }
    }
    api_error_clear(&error);
}

static LRESULT CALLBACK view_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    BrowserScreen *s = (BrowserScreen *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (!s) return DefWindowProcW(hwnd, msg, wp, lp);
    int x = (short)LOWORD(lp), y = (short)HIWORD(lp);
    switch (msg) {
    case WM_BROWSER_FEED: drain_feed(s); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: { PAINTSTRUCT ps; HDC dc = BeginPaint(hwnd, &ps); paint_view(s, hwnd, dc); EndPaint(hwnd, &ps); return 0; }
    case WM_SETFOCUS: case WM_KILLFOCUS: InvalidateRect(hwnd, NULL, FALSE); return 0;
    case WM_GETDLGCODE: return DLGC_WANTALLKEYS | DLGC_WANTCHARS | DLGC_WANTARROWS | DLGC_WANTTAB;
    case WM_SETCURSOR: SetCursor(LoadCursorW(NULL, (LPCWSTR)IDC_ARROW)); return TRUE;
    case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN:
        SetFocus(hwnd);
        if (s->pressing) return 0;
        s->pressing = true; s->dragging = false; s->press_x = x; s->press_y = y;
        s->clicks = msg == WM_LBUTTONDBLCLK ? 2 : 1;
        s->button = msg == WM_RBUTTONDOWN ? "right" : msg == WM_MBUTTONDOWN ? "middle" : "left";
        SetCapture(hwnd);
        return 0;
    case WM_MOUSEMOVE:
        if (!s->pressing || !str_eq(s->button, "left")) return 0;
        if (!s->dragging && abs(x - s->press_x) + abs(y - s->press_y) > GetSystemMetrics(SM_CXDRAG)) {
            Json *down = pointer_input(s, "down", s->press_x, s->press_y);
            if (down) { s->dragging = true; send_input(s, down); }
        }
        if (s->dragging) send_input(s, pointer_input(s, "move", x, y));
        return 0;
    case WM_LBUTTONUP: case WM_RBUTTONUP: case WM_MBUTTONUP: {
        if (!s->pressing) return 0;
        s->pressing = false;
        ReleaseCapture();
        if (s->dragging) { Json *up = pointer_input(s, "up", x, y); if (!up) { up = simple_input("up"); } send_input(s, up); return 0; }
        Json *click = pointer_input(s, "click", s->press_x, s->press_y);
        if (click) {
            if (!str_eq(s->button, "left")) json_set_str(click, "button", s->button);
            if (s->clicks > 1) json_set_num(click, "clickCount", s->clicks);
            send_input(s, click);
        }
        return 0;
    }
    case WM_CAPTURECHANGED: s->pressing = false; return 0;
    case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL: {
        POINT pt = { x, y }; ScreenToClient(hwnd, &pt);
        Json *wheel = pointer_input(s, "wheel", pt.x, pt.y);
        if (!wheel) return 0;
        // A notch scrolls 100 pixels, as Chromium does; shift turns it sideways.
        double delta = -(double)GET_WHEEL_DELTA_WPARAM(wp) * 100.0 / WHEEL_DELTA;
        bool sideways = msg == WM_MOUSEHWHEEL || GetKeyState(VK_SHIFT) < 0;
        if (msg == WM_MOUSEHWHEEL) delta = -delta;
        json_set_num(wheel, sideways ? "deltaX" : "deltaY", delta);
        send_input(s, wheel);
        return 0;
    }
    case WM_KEYDOWN: case WM_SYSKEYDOWN: {
        bool ctrl = GetKeyState(VK_CONTROL) < 0, alt = GetKeyState(VK_MENU) < 0;
        if (ctrl && wp == 'V') { paste(s); return 0; }
        if (ctrl && wp == 'L') { SetFocus(s->address); SendMessageW(s->address, EM_SETSEL, 0, -1); return 0; }
        if (wp == VK_F5) { send_input(s, simple_input("reload")); return 0; }
        const char *name = browser_key_name((unsigned)wp);
        bool character = (wp >= 'A' && wp <= 'Z') || (wp >= '0' && wp <= '9');
        // Letters and digits are typed through WM_CHAR, unless a shortcut holds ctrl or alt.
        if (name && (!character || ctrl || alt)) {
            Json *key = simple_input("key"); json_set_str(key, "key", name);
            add_modifiers(key, true);
            send_input(s, key);
            return 0;
        }
        if (msg == WM_SYSKEYDOWN) break;
        return 0;
    }
    case WM_CHAR: {
        wchar_t c = (wchar_t)wp;
        if (c >= 0xD800 && c <= 0xDBFF) { s->high_surrogate = c; return 0; }
        // Control characters came as keys already (Enter, Tab, Backspace, ctrl shortcuts).
        if (c < 0x20 || c == 0x7F) return 0;
        wchar_t text[3] = { 0 };
        if (c >= 0xDC00 && c <= 0xDFFF) { if (!s->high_surrogate) return 0; text[0] = s->high_surrogate; text[1] = c; }
        else text[0] = c;
        s->high_surrogate = 0;
        char *utf8 = wide_to_utf8(text);
        Json *type = simple_input("type"); json_set_str(type, "text", utf8);
        free(utf8);
        send_input(s, type);
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
static void register_view_class(void) {
    static bool registered;
    if (registered) return;
    WNDCLASSW wc; memset(&wc, 0, sizeof wc);
    wc.style = CS_DBLCLKS; wc.lpfnWndProc = view_proc; wc.hInstance = GetModuleHandleW(NULL);
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW); wc.lpszClassName = VIEW_CLASS;
    registered = RegisterClassW(&wc) != 0;
}

static LRESULT CALLBACK address_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref) {
    BrowserScreen *s = (BrowserScreen *)ref;
    if (msg == WM_KEYDOWN && wp == VK_RETURN) { navigate(s); return 0; }
    if (msg == WM_KEYDOWN && wp == VK_ESCAPE) { SetFocus(s->view); set_address(s); return 0; }
    if (msg == WM_CHAR && (wp == VK_RETURN || wp == VK_ESCAPE)) return 0;
    // A click into the field takes the whole address, as a browser's does, so typing replaces it.
    if (msg == WM_LBUTTONDOWN && GetFocus() != hwnd) { SetFocus(hwnd); SendMessageW(hwnd, EM_SETSEL, 0, -1); return 0; }
    if (msg == WM_KILLFOCUS) { LRESULT r = DefSubclassProc(hwnd, msg, wp, lp); set_address(s); return r; }
    if (msg == WM_NCDESTROY) RemoveWindowSubclass(hwnd, address_proc, id);
    return DefSubclassProc(hwnd, msg, wp, lp);
}
/// The controls are children of the pane, which is known once the screen is shown in it.
static void ensure_controls(BrowserScreen *s) {
    if (s->view || !s->base.pane) return;
    register_view_class();
    HWND parent = pane_hwnd(s->base.pane);
    s->view = CreateWindowExW(0, VIEW_CLASS, L"", WS_CHILD | WS_TABSTOP, 0, 0, 10, 10, parent, NULL, GetModuleHandleW(NULL), NULL);
    SetWindowLongPtrW(s->view, GWLP_USERDATA, (LONG_PTR)s);
    s->address = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | ES_AUTOHSCROLL, 0, 0, 10, 10, parent, (HMENU)(INT_PTR)ID_ADDRESS, GetModuleHandleW(NULL), NULL);
    SendMessageW(s->address, WM_SETFONT, (WPARAM)font(FONT_BODY), TRUE);
    SendMessageW(s->address, EM_SETCUEBANNER, TRUE, (LPARAM)L"Enter a web address");
    SetWindowSubclass(s->address, address_proc, ID_ADDRESS, (DWORD_PTR)s);
    theme_apply_control(s->address);
    set_address(s);
}

// MARK: - Layout

static void paint_field(Doc *doc, Item *it, Canvas *cv, const RECT *rc) { (void)doc; (void)it; fill_round_rect(cv, rc, px(8), theme.field, theme.line); }
static void paint_lock(Doc *doc, Item *it, Canvas *cv, const RECT *rc) { (void)doc; draw_glyph(cv, (wchar_t)it->arg, rc, FONT_ICON_SMALL, theme.muted); }

/// A tab's title for its strip: the page title, else its address, cut to a length a tab can show.
static char *tab_label(const BrowserTab *tab) {
    const char *t = !str_empty(tab->title) ? tab->title : !str_empty(tab->url) && !str_eq(tab->url, "about:blank") ? tab->url : "New tab";
    size_t len = strlen(t), cut = 0, chars = 0;
    while (cut < len && chars < 28) { cut++; while (cut < len && ((unsigned char)t[cut] & 0xC0) == 0x80) cut++; chars++; }
    return cut < len ? xstrfmt("%.*s\xE2\x80\xA6", (int)cut, t) : xstrdup(t);
}

static void browser_layout(Screen *base, Doc *doc) {
    BrowserScreen *s = (BrowserScreen *)base;
    int w = doc->width;
    SetRectEmpty(&s->view_rc); SetRectEmpty(&s->address_rc);
    doc_space(doc, px(8));
    const char *notice = s->error ? s->error : s->state_error;
    if (notice) { doc_notice(doc, 0, w, notice); doc_space(doc, px(10)); }
    if (!s->state_read) { doc_loading(doc, 0, w, "Reading the browser\xE2\x80\xA6"); return; }
    if (!s->state.running) {
        const char *title = s->starting ? "Starting the browser\xE2\x80\xA6" : s->state.on ? "The browser is switched on but not running" : "The shared browser is off";
        const char *detail = s->closed_session ? "This conversation is closed. Reopen it, then start the browser."
            : s->state.on ? "It starts with the agent\xE2\x80\x99s next turn, or start it now. Its cookies and logins are still there."
            : "Switch on a Chromium of this session\xE2\x80\x99s own: the agent drives it with its browser tools, and you watch and use it here at the same time, on the same tabs. You can log in for the agent or take over a step and hand back with a message.";
        doc_empty_state(doc, 0, w, 0xE774, title, detail);
        if (store_can_manage() && store_supports("browser_on")) {
            doc_space(doc, px(8));
            doc_button(doc, 0, 0, s->starting ? "Starting\xE2\x80\xA6" : "\xE2\x96\xB6 Start the browser", BUTTON_PROMINENT, ACT_START, 0, !s->starting && !s->stopping);
        }
        return;
    }
    // The tabs, as a tab strip, and ＋ for a new one.
    int h = px(38), tx = 0, ty = doc->y;
    for (size_t i = 0; i < s->state.count; i++) {
        char *label = tab_label(&s->state.tabs[i]);
        doc_tab(doc, &tx, &ty, 0, w, h, 0xE774, label, NULL, str_eq(s->state.tabs[i].id, s->state.active), ACT_TAB, (intptr_t)i);
        free(label);
    }
    if (can_drive()) doc_tab(doc, &tx, &ty, 0, w, h, 0xE710, "New tab", NULL, false, ACT_NEW_TAB, 0);
    doc->y = ty + h;
    doc_rule(doc, 0, w);
    doc_space(doc, px(10));
    // The address field: the tab in view's address, and where to go on Enter.
    int field_h = px(34), top = doc->y;
    RECT field = { 0, top, w, top + field_h };
    doc_add(doc, &field, paint_field);
    RECT lock = { px(10), top, px(10) + px(16), top + field_h };
    const BrowserTab *tab = browser_active_tab(&s->state);
    int li = doc_add(doc, &lock, paint_lock);
    doc_item(doc, li)->arg = tab && str_has_prefix(tab->url, "https://") ? 0xE72E : 0xE774;
    int ex = px(34), eh = edit_line_height(FONT_BODY);
    SetRect(&s->address_rc, ex, top + (field_h - eh) / 2, w - px(10), top + (field_h - eh) / 2 + eh);
    doc->y = top + field_h + px(10);
    // The picture fills what is left of the pane.
    RECT view = pane_content_rect(base->pane);
    int area = doc->y, vh = (view.bottom - view.top) - area - px(12);
    if (vh < px(320)) vh = px(320);
    SetRect(&s->view_rc, 0, area, w, area + vh);
    doc->y = area + vh;
}

static void browser_place(Screen *base, const RECT *content, int scroll_y) {
    BrowserScreen *s = (BrowserScreen *)base;
    if (!s->view) return;
    bool on = s->shown && s->state.running && content && !IsRectEmpty(&s->view_rc);
    if (on) {
        RECT rc; GetClientRect(pane_hwnd(base->pane), &rc);
        int m = (rc.right - rc.left - pane_content_width(base->pane)) / 2;
        RECT v = { content->left + m + s->view_rc.left, content->top + s->view_rc.top - scroll_y, content->left + m + s->view_rc.right, content->top + s->view_rc.bottom - scroll_y };
        RECT a = { content->left + m + s->address_rc.left, content->top + s->address_rc.top - scroll_y, content->left + m + s->address_rc.right, content->top + s->address_rc.bottom - scroll_y };
        RECT visible;
        if (IntersectRect(&visible, &v, content)) MoveWindow(s->view, visible.left, visible.top, visible.right - visible.left, visible.bottom - visible.top, TRUE);
        MoveWindow(s->address, a.left, a.top, a.right - a.left, a.bottom - a.top, TRUE);
    }
    ShowWindow(s->view, on ? SW_SHOWNA : SW_HIDE);
    ShowWindow(s->address, on ? SW_SHOWNA : SW_HIDE);
    EnableWindow(s->address, can_drive());
}

static void header_button(HeaderInfo *info, wchar_t glyph, const char *tip, int action, bool enabled, bool destructive) {
    if (info->button_count >= HEADER_BUTTONS) return;
    HeaderButton *b = &info->buttons[info->button_count++];
    b->glyph = glyph; b->tip = tip; b->action = action; b->enabled = enabled; b->destructive = destructive;
}
static void browser_header(Screen *base, HeaderInfo *info) {
    BrowserScreen *s = (BrowserScreen *)base;
    snprintf(info->title, sizeof info->title, "\xF0\x9F\x8C\x90 Browser \xC2\xB7 %s", s->title ? s->title : "Conversation");
    const BrowserTab *tab = browser_active_tab(&s->state);
    const char *line = !s->state_read ? "" : s->state.running ? (tab ? tab->url : "Running") : s->state.on ? "Switched on, not running" : "Off";
    snprintf(info->subtitle, sizeof info->subtitle, "%s%s", line, s->state.running && !can_drive() ? " \xC2\xB7 watching only: this token cannot act in it" : "");
    if (s->state.running) snprintf(info->status, sizeof info->status, "running");
    bool drive = can_drive() && s->state.running;
    header_button(info, 0xE72B, "Back", ACT_BACK, drive, false);
    header_button(info, 0xE72A, "Forward", ACT_FORWARD, drive, false);
    header_button(info, 0xE72C, "Reload the page", ACT_RELOAD, drive, false);
    header_button(info, 0xE710, "Open a new tab", ACT_NEW_TAB, drive, false);
    header_button(info, 0xE711, "Close this tab", ACT_CLOSE_TAB, drive && tab, false);
    if (store_can_manage() && s->state.on && store_supports("browser_off")) {
        HeaderButton *b = &info->buttons[info->button_count++];
        b->glyph = 0xE7E8; b->tip = "Switch the browser off"; b->action = ACT_STOP; b->enabled = !s->stopping && !s->starting; b->destructive = true;
        snprintf(b->label, sizeof b->label, "Switch off");
    } else if (store_can_manage() && store_supports("browser_on")) {
        HeaderButton *b = &info->buttons[info->button_count++];
        b->glyph = 0xE7E8; b->tip = "Switch the browser on"; b->action = ACT_START; b->enabled = !s->starting && !s->stopping; b->prominent = true;
        snprintf(b->label, sizeof b->label, "Start");
    }
}

static void browser_action(Screen *base, int action, intptr_t arg, POINT pt) {
    BrowserScreen *s = (BrowserScreen *)base;
    (void)pt;
    switch (action) {
    case ACT_START: switch_browser(s, true); break;
    case ACT_STOP: switch_browser(s, false); break;
    case ACT_BACK: send_input(s, simple_input("back")); break;
    case ACT_FORWARD: send_input(s, simple_input("forward")); break;
    case ACT_RELOAD: send_input(s, simple_input("reload")); break;
    case ACT_NEW_TAB: send_input(s, simple_input("newTab")); SetFocus(s->address); break;
    case ACT_CLOSE_TAB: {
        const BrowserTab *tab = browser_active_tab(&s->state);
        if (!tab) break;
        Json *j = simple_input("closeTab"); json_set_str(j, "tab", tab->id);
        send_input(s, j);
        break;
    }
    case ACT_TAB:
        if (arg >= 0 && (size_t)arg < s->state.count) {
            Json *j = simple_input("tab"); json_set_str(j, "tab", s->state.tabs[arg].id);
            send_input(s, j);
        }
        break;
    }
}
/// A right click on a tab closes it, as a browser's tab menu would.
static void browser_context(Screen *base, int action, intptr_t arg, POINT pt) {
    BrowserScreen *s = (BrowserScreen *)base;
    if (action != ACT_TAB || arg < 0 || (size_t)arg >= s->state.count || !can_drive()) return;
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, 1, L"Show this tab");
    AppendMenuW(menu, MF_STRING, 2, L"Close this tab");
    int chosen = TrackPopupMenu(menu, TPM_RETURNCMD, pt.x, pt.y, 0, pane_hwnd(base->pane), NULL);
    DestroyMenu(menu);
    if ((size_t)arg >= s->state.count || chosen < 1) return;
    Json *j = simple_input(chosen == 1 ? "tab" : "closeTab"); json_set_str(j, "tab", s->state.tabs[arg].id);
    send_input(s, j);
}

static void browser_timer(Screen *base, UINT id) {
    BrowserScreen *s = (BrowserScreen *)base;
    if (id == TIMER_RECONNECT) { KillTimer(pane_hwnd(base->pane), TIMER_RECONNECT); start_feed(s); return; }
    if (poller_fired(&s->poller, id)) { if (!s->req_state) read_state(s); else poller_finished(&s->poller, false, -1); }
}
static void browser_visible(Screen *base, bool shown) {
    BrowserScreen *s = (BrowserScreen *)base;
    s->shown = shown;
    if (shown) {
        ensure_controls(s);
        read_state(s);
        start_feed(s);
    } else {
        // Frames are only sent while somebody watches: hidden, the stream stops.
        stop_feed(s);
        poller_stop(&s->poller);
        request_cancel(&s->req_state);
        if (s->view) { ShowWindow(s->view, SW_HIDE); ShowWindow(s->address, SW_HIDE); }
    }
}
static void browser_refresh(Screen *base) {
    BrowserScreen *s = (BrowserScreen *)base;
    if (s->state.running) send_input(s, simple_input("reload"));
    read_state(s);
}
static bool browser_key(Screen *base, WPARAM vk, bool ctrl, bool shift) {
    BrowserScreen *s = (BrowserScreen *)base;
    (void)shift;
    if (ctrl && vk == 'L' && s->address && IsWindowVisible(s->address)) { SetFocus(s->address); SendMessageW(s->address, EM_SETSEL, 0, -1); return true; }
    return false;
}
static void browser_destroy(Screen *base) {
    BrowserScreen *s = (BrowserScreen *)base;
    stop_feed(s);
    poller_stop(&s->poller);
    request_cancel(&s->req_state); request_cancel(&s->req_toggle); request_cancel(&s->req_input);
    if (s->view) { SetWindowLongPtrW(s->view, GWLP_USERDATA, 0); DestroyWindow(s->view); }
    if (s->address) DestroyWindow(s->address);
    browser_state_free(&s->state);
    browser_inputs_free(&s->inputs);
    free(s->pixels); free(s->session_id); free(s->title); free(s->error); free(s->state_error);
    screen_release(base);
}

static const ScreenVTable browser_vt = {
    .destroy = browser_destroy, .layout = browser_layout, .header = browser_header, .action = browser_action,
    .context = browser_context, .timer = browser_timer, .place = browser_place, .visible = browser_visible,
    .refresh = browser_refresh, .key = browser_key,
};

bool browser_offered(void) { return store_supports("browser"); }

Screen *browser_screen_new(const Session *session) {
    BrowserScreen *s = xcalloc(1, sizeof *s);
    s->base.vt = &browser_vt;
    s->base.id = xstrfmt("browser:%s", session_id(session));
    s->session_id = xstrdup(session_id(session));
    s->title = xstrdup(session_display_title(session));
    // The session record says whether the browser is on before the first read does.
    bool running = false;
    s->state.on = browser_session_on(session->raw, &running);
    s->state.running = running;
    return &s->base;
}
