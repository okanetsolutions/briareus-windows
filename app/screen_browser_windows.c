#include "screen_browser_internal.h"

typedef struct { int op; char *session_id, *title; bool on, running; } Move;

static Pane *find_window(const char *session_id_);
static HWND window_of(Pane *p);
static BrowserScreen *docked(void);
static void open_window(Screen *screen, const RECT *at);
static void do_move(const Move *m);
static LRESULT CALLBACK host_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

// MARK: - Docked, or in a window of its own

static const wchar_t HOST_CLASS[] = L"BriareusBrowserHost";

/// A popped-out browser's window: one of detached.c's, its root screen the session's browser.
static Pane *find_window(const char *session_id_) {
    char *id = xstrfmt("browser:%s", session_id_);
    Pane *p = detached_find(id);
    free(id);
    return p;
}

static HWND window_of(Pane *p) { return GetAncestor(pane_hwnd(p), GA_ROOT); }

static BrowserScreen *docked(void) { return app_browser_pane() ? (BrowserScreen *)pane_root(app_browser_pane()) : NULL; }

/// Opens the screen in a window of its own, over `at` (screen coordinates) when given: where it was docked.
static void open_window(Screen *screen, const RECT *at) { detached_open(screen, at, true); }

/// Where the browser is shown changes once the click that asked for it is over: the screen that asked goes with it.
static void do_move(const Move *m) {
    Pane *w = find_window(m->session_id);
    BrowserScreen *d = docked();
    bool is_docked = d && str_eq(d->session_id, m->session_id);
    switch (m->op) {
    case ACT_CLOSE_PANE:
        if (w) DestroyWindow(window_of(w)); else if (is_docked) app_set_browser(NULL);
        break;
    case ACT_DETACH: {
        if (!is_docked) break;
        RECT at; GetWindowRect(pane_hwnd(app_browser_pane()), &at);
        app_set_browser(NULL);
        open_window(screen_browser_browser_new(m->session_id, m->title, m->on, m->running, true), &at);
        break;
    }
    case ACT_DOCK:
        if (!w || !app_browser_dockable() || !screen_browser_conversation_shown(m->session_id)) break;
        DestroyWindow(window_of(w));
        app_set_browser(screen_browser_browser_new(m->session_id, m->title, m->on, m->running, false));
        SetForegroundWindow(app_window());
        break;
    }
}

static LRESULT CALLBACK host_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_BROWSER_MOVE) {
        Move *m = (Move *)lp;
        do_move(m);
        free(m->session_id); free(m->title); free(m);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void screen_browser_post_move(BrowserScreen *s, int op) {
    static HWND host;
    if (!host) {
        WNDCLASSW wc; memset(&wc, 0, sizeof wc);
        wc.lpfnWndProc = host_proc; wc.hInstance = GetModuleHandleW(NULL); wc.lpszClassName = HOST_CLASS;
        RegisterClassW(&wc);
        host = CreateWindowExW(0, HOST_CLASS, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, GetModuleHandleW(NULL), NULL);
        if (!host) return;
    }
    Move *m = xcalloc(1, sizeof *m);
    m->op = op; m->session_id = xstrdup(s->session_id); m->title = xstrdup(s->title);
    m->on = s->state.on; m->running = s->state.running;
    if (!PostMessageW(host, WM_BROWSER_MOVE, 0, (LPARAM)m)) { free(m->session_id); free(m->title); free(m); }
}

void browser_open(const Session *session) {
    const char *id = session_id(session);
    Pane *w = find_window(id);
    if (w) { detached_raise(w); return; }
    BrowserScreen *d = docked();
    if (d && str_eq(d->session_id, id)) { app_set_browser(NULL); return; }
    Screen *s = browser_screen_new(session);
    if (app_browser_dockable() && screen_browser_conversation_shown(id)) { app_set_browser(s); return; }
    // Too narrow for a column beside the conversation, or the conversation is in a window of its own: a window of its own.
    ((BrowserScreen *)s)->detached = true;
    open_window(s, NULL);
}
