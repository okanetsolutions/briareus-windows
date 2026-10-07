// Windows of their own: a screen popped out of the main window, as a session's browser or a conversation, shown in a
// top-level window holding one pane, so several can sit side by side on different monitors.
#include "resource.h"
#include "screens.h"
#include "str.h"
#include "theme.h"
#include <stdlib.h>
#include <string.h>

typedef struct { HWND hwnd; Pane *pane; HWND focus; bool sidebar, modal_disabled; } Detached;
static Detached **g_windows; static size_t g_window_count;
static const wchar_t WINDOW_CLASS[] = L"BriareusWindow";
#define WM_DETACHED_DOCK (WM_APP + 63)

static void forget(Detached *w) {
    for (size_t i = 0; i < g_window_count; i++) if (g_windows[i] == w) { g_windows[i] = g_windows[--g_window_count]; break; }
}
static Detached *find_hwnd(HWND hwnd) {
    for (size_t i = 0; i < g_window_count; i++) if (g_windows[i]->hwnd == hwnd) return g_windows[i];
    return NULL;
}

/// The window's ↙ asks for the screen to go back to the main window once the click is over.
static void post_dock(void *ctx) { PostMessageW(((Detached *)ctx)->hwnd, WM_DETACHED_DOCK, 0, 0); }

/// Back to the main window's detail, unless the page there keeps its unsaved changes.
static void dock(Detached *w) {
    Screen *root = w->pane ? pane_root(w->pane) : NULL;
    if (!root || pane_depth(w->pane) != 1 || !app_detail_can_leave()) return;
    Screen *s = pane_take_root(w->pane);
    app_show_detail(s);
    HWND main = app_window();
    DestroyWindow(w->hwnd);
    if (IsIconic(main)) ShowWindow(main, SW_RESTORE);
    SetForegroundWindow(main);
}

static LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    Detached *w = (Detached *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (msg) {
    case WM_NCCREATE:
        w = ((CREATESTRUCTW *)lp)->lpCreateParams;
        w->hwnd = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)w);
        break;
    case WM_CREATE:
        theme_apply_window(hwnd);
        w->pane = pane_create(hwnd, w->sidebar);
        pane_set_titles_window(w->pane);
        pane_set_move_button(w->pane, 0xE90D, "Back to the main window", post_dock, w);
        return 0;
    case WM_SIZE: if (w && w->pane && wp != SIZE_MINIMIZED) { RECT rc; GetClientRect(hwnd, &rc); pane_set_bounds(w->pane, &rc); } return 0;
    case WM_ERASEBKGND: return 1;
    case WM_ACTIVATE:
        // Keys go back to where they went before the window lost the foreground: a composer, a page, or the pane.
        if (!w || !w->pane) break;
        if (LOWORD(wp) == WA_INACTIVE) w->focus = GetFocus();
        else SetFocus(w->focus && IsChild(hwnd, w->focus) ? w->focus : pane_hwnd(w->pane));
        return 0;
    case WM_GETMINMAXINFO: { MINMAXINFO *mmi = (MINMAXINFO *)lp; mmi->ptMinTrackSize.x = px(420); mmi->ptMinTrackSize.y = px(320); return 0; }
    case WM_DPICHANGED: { RECT *rc = (RECT *)lp; SetWindowPos(hwnd, NULL, rc->left, rc->top, rc->right - rc->left, rc->bottom - rc->top, SWP_NOZORDER | SWP_NOACTIVATE); return 0; }
    case WM_DETACHED_DOCK: if (w) dock(w); return 0;
    case WM_DESTROY:
        if (!w) return 0;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        forget(w);
        pane_destroy(w->pane);
        free(w);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

Pane *detached_open(Screen *screen, const RECT *at, bool sidebar) {
    static bool registered;
    HINSTANCE instance = GetModuleHandleW(NULL);
    if (!registered) {
        WNDCLASSEXW wc; memset(&wc, 0, sizeof wc);
        wc.cbSize = sizeof wc; wc.lpfnWndProc = window_proc; wc.hInstance = instance; wc.lpszClassName = WINDOW_CLASS;
        wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
        wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_APP)); wc.hIconSm = wc.hIcon;
        registered = RegisterClassExW(&wc) != 0;
    }
    int x = CW_USEDEFAULT, y = CW_USEDEFAULT, cx = px(1100), cy = px(800);
    if (at && !IsRectEmpty(at)) {
        x = at->left + px(24); y = at->top + px(24); cx = at->right - at->left; cy = at->bottom - at->top;
        // Kept on the monitor it popped out on.
        MONITORINFO mi = { sizeof mi };
        if (GetMonitorInfoW(MonitorFromRect(at, MONITOR_DEFAULTTONEAREST), &mi)) {
            RECT wa = mi.rcWork;
            if (cx > wa.right - wa.left) cx = wa.right - wa.left;
            if (cy > wa.bottom - wa.top) cy = wa.bottom - wa.top;
            if (x + cx > wa.right) x = wa.right - cx;
            if (y + cy > wa.bottom) y = wa.bottom - cy;
            if (x < wa.left) x = wa.left;
            if (y < wa.top) y = wa.top;
        }
    }
    Detached *w = xcalloc(1, sizeof *w);
    w->sidebar = sidebar;
    g_windows = xrealloc(g_windows, (g_window_count + 1) * sizeof *g_windows);
    g_windows[g_window_count++] = w;
    // Not owned by the main window: it stays put when that one is minimized, and has its own taskbar button.
    HWND hwnd = CreateWindowExW(WS_EX_APPWINDOW, WINDOW_CLASS, L"Briareus", WS_OVERLAPPEDWINDOW, x, y, cx, cy, NULL, NULL, instance, w);
    if (!hwnd) {
        forget(w);
        free(w);
        screen->vt->destroy(screen);
        return NULL;
    }
    RECT rc; GetClientRect(hwnd, &rc); pane_set_bounds(w->pane, &rc);
    pane_set_root(w->pane, screen);
    ShowWindow(hwnd, SW_SHOWNORMAL);
    SetForegroundWindow(hwnd);
    return w->pane;
}

Pane *detached_find(const char *id) {
    if (!id) return NULL;
    for (size_t i = 0; i < g_window_count; i++) {
        Screen *root = g_windows[i]->pane ? pane_root(g_windows[i]->pane) : NULL;
        if (root && str_eq(root->id, id)) return g_windows[i]->pane;
    }
    return NULL;
}
bool detached_pane(const Pane *pane) {
    for (size_t i = 0; i < g_window_count; i++) if (pane && g_windows[i]->pane == pane) return true;
    return false;
}
Pane *detached_pane_of(HWND hwnd) {
    Detached *w = hwnd ? find_hwnd(GetAncestor(hwnd, GA_ROOT)) : NULL;
    return w ? w->pane : NULL;
}
void detached_raise(Pane *pane) {
    HWND hwnd = GetAncestor(pane_hwnd(pane), GA_ROOT);
    if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);
    SetForegroundWindow(hwnd);
}
void detached_close(Pane *pane) { PostMessageW(GetAncestor(pane_hwnd(pane), GA_ROOT), WM_CLOSE, 0, 0); }
bool detached_any_shown(void) {
    for (size_t i = 0; i < g_window_count; i++) if (IsWindowVisible(g_windows[i]->hwnd) && !IsIconic(g_windows[i]->hwnd)) return true;
    return false;
}
void detached_close_all(void) {
    while (g_window_count) {
        size_t before = g_window_count;
        DestroyWindow(g_windows[g_window_count - 1]->hwnd);
        if (g_window_count == before) break;
    }
}
// Only the windows this disabled come back, so one a file picker disabled stays disabled under it.
void detached_enable(HWND except, bool enabled) {
    for (size_t i = 0; i < g_window_count; i++) {
        Detached *w = g_windows[i];
        if (enabled) { if (w->modal_disabled) EnableWindow(w->hwnd, TRUE); w->modal_disabled = false; }
        else if (w->hwnd != except && IsWindowEnabled(w->hwnd)) { EnableWindow(w->hwnd, FALSE); w->modal_disabled = true; }
    }
}
void detached_themed(void) {
    for (size_t i = 0; i < g_window_count; i++) {
        theme_apply_window(g_windows[i]->hwnd);
        if (!g_windows[i]->pane) continue;
        SendMessageW(pane_hwnd(g_windows[i]->pane), WM_THEMECHANGED, 0, 0);
        pane_relayout(g_windows[i]->pane);
    }
}
