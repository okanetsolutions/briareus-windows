// Briareus for Windows: the main window, its two columns, and the navigation between them.
#include "canvas.h"
#include "dialogs.h"
#include "media.h"
#include "meeting.h"
#include "resource.h"
#include "screens.h"
#include "sftp_session.h"
#include "str.h"
#include "terminal.h"
#include "theme.h"
#include "updater.h"
#include <stdio.h>
#include <commctrl.h>
#include <objbase.h>
#include <stdlib.h>
#include <string.h>

#define MAIN_CLASS L"BriareusMain"
// The dashboard's columns: a 268px sidebar, the main column, and a 272px pull request panel beside a conversation.
// Below its `lg` breakpoint (64rem) the dashboard turns the columns into drawers; here that is one column at a time.
#define SIDEBAR_WIDTH 268
#define PANEL_WIDTH 272
#define NARROW_WIDTH 1024
// The side panel a board's card opens in covers this share of the main column, at least OVERLAY_MIN_WIDTH.
#define OVERLAY_SHARE 62
#define OVERLAY_MIN_WIDTH 560
// A session's browser docks as a column of its own on the right, beside the conversation, with a divider to drag.
#define BROWSER_MIN_WIDTH 360
#define DETAIL_MIN_WIDTH 380
#define SPLITTER_WIDTH 6
// The detail's ⧉ moves its page into a window of its own once the click is over.
#define WM_APP_POP_OUT (WM_APP + 5)

static HWND g_main;
typedef HRESULT (WINAPI *TaskDialogIndirectFn)(const TASKDIALOGCONFIG *, int *, int *, BOOL *);
static TaskDialogIndirectFn g_task_dialog;
static Pane *g_pairing, *g_sidebar, *g_detail, *g_panel, *g_browser, *g_overlay;
static int g_browser_width;    // the docked browser's width once the divider was dragged; 0 for the default
static bool g_browser_expanded, g_dragging_splitter;
static bool g_connected_layout;
static bool g_narrow_detail;   // in one column, whether the detail is the visible pane
static int g_modal;            // dialogs open
static bool g_modal_disabled;  // whether the first of them disabled the main window
static HWND g_focus;           // what had the keyboard when the main window lost the foreground

HWND app_window(void) { return g_main; }
HWND app_dialog_owner(void) { HWND active = GetActiveWindow(); return active ? active : g_main; }
// The dialog disables its owner itself, and enables it again before it hands the foreground back.
void app_modal_begin(HWND owner) {
    if (g_modal++) return;
    if (owner) owner = GetAncestor(owner, GA_ROOT);   // a pane passed as the owner stands for its window
    // A window already disabled (by a file picker) is left for whoever disabled it to enable again.
    g_modal_disabled = g_main && g_main != owner && IsWindowEnabled(g_main);
    if (g_modal_disabled) EnableWindow(g_main, FALSE);
    detached_enable(owner, false);
}
void app_modal_end(void) {
    if (--g_modal) return;
    if (g_modal_disabled && g_main) EnableWindow(g_main, TRUE);
    g_modal_disabled = false;
    detached_enable(NULL, true);
}
Pane *app_sidebar_pane(void) { return g_sidebar; }
Pane *app_detail_pane(void) { return g_detail; }
Pane *app_panel_pane(void) { return g_panel; }
Pane *app_browser_pane(void) { return g_browser; }

static bool is_narrow(void) { RECT rc; GetClientRect(g_main, &rc); return rc.right - rc.left < px(NARROW_WIDTH); }
static bool browser_docked(void) { return g_browser && pane_root(g_browser) != NULL; }
// The browser column takes the pull request panel's place while it is open.
static bool panel_shown(void) { return g_panel && pane_root(g_panel) != NULL && !browser_docked(); }
static bool overlay_shown(void) { return g_overlay && pane_root(g_overlay) != NULL; }
bool app_browser_dockable(void) { return g_main && !is_narrow() && !IsIconic(g_main); }
bool app_browser_expanded(void) { return g_browser_expanded; }
/// The docked browser's left edge, and the divider before it; false when it is not beside the detail.
static bool browser_split(const RECT *rc, int *left, RECT *splitter) {
    if (!g_connected_layout || !browser_docked() || is_narrow()) return false;
    int sw = px(SIDEBAR_WIDTH), avail = rc->right - sw;
    if (g_browser_expanded) { *left = sw; SetRectEmpty(splitter); return true; }
    int gap = px(SPLITTER_WIDTH), lo = px(BROWSER_MIN_WIDTH), hi = avail - gap - px(DETAIL_MIN_WIDTH);
    int bw = g_browser_width ? g_browser_width : avail * 45 / 100;
    if (bw > hi) bw = hi;
    if (bw < lo) bw = hi < lo ? avail / 2 : lo;
    *left = rc->right - bw;
    SetRect(splitter, *left - gap, rc->top, *left, rc->bottom);
    return true;
}

static void back_to_sidebar(void *ctx) { (void)ctx; g_narrow_detail = false; PostMessageW(g_main, WM_SIZE, 0, 0); }

static void layout(void) {
    RECT rc; GetClientRect(g_main, &rc);
    if (!g_connected_layout) {
        if (g_pairing) { pane_set_bounds(g_pairing, &rc); pane_show(g_pairing, true); }
        if (g_sidebar) pane_show(g_sidebar, false);
        if (g_detail) pane_show(g_detail, false);
        if (g_panel) pane_show(g_panel, false);
        if (g_browser) pane_show(g_browser, false);
        if (g_overlay) pane_show(g_overlay, false);
        return;
    }
    if (g_pairing) pane_show(g_pairing, false);
    bool narrow = is_narrow();
    bool detail_has_content = g_detail && pane_root(g_detail) && !str_eq(pane_root(g_detail)->id, "placeholder");
    if (narrow) {
        bool show_detail = g_narrow_detail && detail_has_content;
        pane_set_root_back(g_detail, true, back_to_sidebar, NULL);
        if (show_detail) { pane_set_bounds(g_detail, &rc); pane_show(g_detail, true); pane_show(g_sidebar, false); }
        else { pane_set_bounds(g_sidebar, &rc); pane_show(g_sidebar, true); pane_show(g_detail, false); }
        if (g_panel) pane_show(g_panel, false);
        if (g_browser) pane_show(g_browser, false);
        if (g_overlay && overlay_shown()) pane_set_bounds(g_overlay, &rc);
    } else {
        pane_set_root_back(g_detail, false, NULL, NULL);
        int sw = px(SIDEBAR_WIDTH), pw = panel_shown() ? px(PANEL_WIDTH) : 0;
        RECT side = { rc.left, rc.top, rc.left + sw, rc.bottom }, main = { rc.left + sw, rc.top, rc.right - pw, rc.bottom };
        RECT panel = { rc.right - pw, rc.top, rc.right, rc.bottom };
        pane_set_bounds(g_sidebar, &side); pane_set_bounds(g_detail, &main);
        pane_show(g_sidebar, true); pane_show(g_detail, true);
        if (g_panel) { pane_set_bounds(g_panel, &panel); pane_show(g_panel, pw > 0); }
        int left; RECT splitter;
        if (browser_split(&rc, &left, &splitter)) {
            RECT browser = { left, rc.top, rc.right, rc.bottom };
            pane_set_bounds(g_browser, &browser); pane_show(g_browser, true);
            if (g_browser_expanded) pane_show(g_detail, false);
            else { main.right = splitter.left; pane_set_bounds(g_detail, &main); }
        } else if (g_browser) pane_show(g_browser, false);
        if (g_overlay && overlay_shown()) {
            int mw = main.right - main.left, ow = mw * OVERLAY_SHARE / 100;
            if (ow < px(OVERLAY_MIN_WIDTH)) ow = px(OVERLAY_MIN_WIDTH);
            if (ow > mw) ow = mw;
            RECT over = { main.right - ow, main.top, main.right, main.bottom };
            pane_set_bounds(g_overlay, &over);
        }
    }
    if (g_overlay) {
        pane_show(g_overlay, overlay_shown());
        if (overlay_shown()) SetWindowPos(pane_hwnd(g_overlay), HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
}

static void paint_divider(Canvas *cv) {
    if (!g_connected_layout || is_narrow()) return;
    RECT rc; GetClientRect(g_main, &rc);
    int x = px(SIDEBAR_WIDTH);
    draw_line(cv, x - 1, rc.top, x - 1, rc.bottom, theme.line);
    if (panel_shown()) { int px_ = rc.right - px(PANEL_WIDTH); draw_line(cv, px_, rc.top, px_, rc.bottom, theme.line); }
    int left; RECT splitter;
    if (browser_split(&rc, &left, &splitter) && !IsRectEmpty(&splitter)) {
        int mx = (splitter.left + splitter.right) / 2;
        draw_line(cv, mx, rc.top, mx, rc.bottom, g_dragging_splitter ? theme.accent : theme.line);
    }
}
static bool over_splitter(int x, int y) {
    RECT rc; GetClientRect(g_main, &rc);
    int left; RECT splitter;
    POINT pt = { x, y };
    return browser_split(&rc, &left, &splitter) && PtInRect(&splitter, pt);
}

void app_set_panel(Screen *screen) {
    if (!g_panel) { if (screen) screen->vt->destroy(screen); return; }
    Screen *root = pane_root(g_panel);
    if (root && screen && screen->id && str_eq(root->id, screen->id)) { screen->vt->destroy(screen); return; }
    pane_set_root(g_panel, screen);
    layout();
    InvalidateRect(g_main, NULL, TRUE);
}

static void close_overlay(void *ctx) { (void)ctx; app_set_overlay(NULL); }
void app_set_overlay(Screen *screen) {
    if (!g_overlay) { if (screen) screen->vt->destroy(screen); return; }
    Screen *root = pane_root(g_overlay);
    if (root && screen && screen->id && str_eq(root->id, screen->id) && pane_depth(g_overlay) == 1) { screen->vt->destroy(screen); return; }
    if (!screen && !root) return;
    // Win32 leaves focus on a hidden window, so closing the panel hands it back to the board.
    HWND focus = GetFocus(), overlay = pane_hwnd(g_overlay);
    bool had_focus = focus && (focus == overlay || IsChild(overlay, focus));
    pane_set_root(g_overlay, screen);
    layout();
    if (screen) SetFocus(overlay);
    else if (had_focus && g_detail) SetFocus(pane_hwnd(g_detail));
    InvalidateRect(g_main, NULL, TRUE);
}

void app_set_browser(Screen *screen) {
    if (!g_browser) { if (screen) screen->vt->destroy(screen); return; }
    Screen *root = pane_root(g_browser);
    if (root && screen && screen->id && str_eq(root->id, screen->id)) { screen->vt->destroy(screen); return; }
    if (!screen) g_browser_expanded = false;
    pane_set_root(g_browser, screen);
    layout();
    InvalidateRect(g_main, NULL, TRUE);
}
void app_set_browser_expanded(bool expanded) {
    if (g_browser_expanded == expanded) return;
    g_browser_expanded = expanded;
    layout();
    if (g_browser) pane_relayout(g_browser);
    InvalidateRect(g_main, NULL, TRUE);
}
/// The docked browser belongs to the conversation beside it: another page in the detail takes it away.
static void drop_browser_unless(const char *detail_id) {
    Screen *root = g_browser ? pane_root(g_browser) : NULL;
    if (!root) return;
    const char *session = root->id && str_has_prefix(root->id, "browser:") ? root->id + 8 : NULL;
    if (session && detail_id && str_has_prefix(detail_id, "conversation:") && str_eq(detail_id + 13, session)) return;
    app_set_browser(NULL);
}

static void rebuild_for_connection(void) {
    bool connected = store_connected();
    if (connected == g_connected_layout && (connected || g_pairing)) {
        if (!connected && g_pairing) pane_relayout(g_pairing);
        if (connected) { if (g_sidebar) pane_relayout(g_sidebar); if (g_detail) pane_relayout(g_detail); }
        return;
    }
    g_connected_layout = connected;
    if (connected) {
        if (g_pairing) pane_set_root(g_pairing, NULL);
        pane_set_root(g_sidebar, projects_screen_new());
        pane_set_root(g_detail, placeholder_screen_new());
        pane_set_root(g_panel, NULL);
        pane_set_root(g_browser, NULL);
        pane_set_root(g_overlay, NULL);
        pane_set_selected_id(g_sidebar, NULL);
        g_narrow_detail = false;
    } else {
        detached_close_all();
        pane_set_root(g_sidebar, NULL); pane_set_root(g_detail, NULL); pane_set_root(g_panel, NULL); pane_set_root(g_browser, NULL); pane_set_root(g_overlay, NULL);
        pane_set_root(g_pairing, pairing_screen_new());
    }
    layout();
}

/// A page already in a window of its own is brought forward there instead of opening a second time.
static bool raise_detached(Screen *screen) {
    Pane *held = screen->id ? detached_find(screen->id) : NULL;
    if (!held) return false;
    screen->vt->destroy(screen);
    detached_raise(held);
    return true;
}
bool app_detail_can_leave(void) {
    Screen *root = g_detail ? pane_root(g_detail) : NULL;
    return !root || !root->vt->can_leave || root->vt->can_leave(root);
}
void app_show_detail(Screen *screen) {
    if (raise_detached(screen)) return;
    Screen *root = pane_root(g_detail);
    // A form with unsaved changes may keep its place.
    bool same = root && screen->id && str_eq(root->id, screen->id) && pane_depth(g_detail) == 1;
    if (root && !same && root->vt->can_leave && !root->vt->can_leave(root)) { screen->vt->destroy(screen); return; }
    if (same) {
        if (root->vt->adopt) root->vt->adopt(root, screen);
        screen->vt->destroy(screen);
    } else {
        pane_set_root(g_panel, NULL);
        pane_set_root(g_overlay, NULL);
        drop_browser_unless(screen->id);
        pane_set_root(g_detail, screen);
    }
    pane_set_selected_id(g_sidebar, pane_root(g_detail) ? pane_root(g_detail)->id : NULL);
    g_narrow_detail = true;
    layout();
    // Opened from the sidebar, keys follow to what opened, so Ctrl+F reaches the detail pane.
    HWND focus = GetFocus(), detail = pane_hwnd(g_detail);
    if (focus && g_sidebar && (focus == pane_hwnd(g_sidebar) || IsChild(pane_hwnd(g_sidebar), focus)) && IsWindowVisible(detail)) SetFocus(detail);
    InvalidateRect(g_main, NULL, TRUE);
}
// From the side panel, what an item links to opens in the panel, as GitHub's does.
void app_push_detail(Screen *screen) {
    if (raise_detached(screen)) return;
    pane_push(overlay_shown() ? g_overlay : g_detail, screen); g_narrow_detail = true; layout();
}
void app_push_from(Screen *from, Screen *screen) {
    if (from && from->pane && detached_pane(from->pane)) {
        // The window's own page, asked for from deeper in that window, comes back to the top there.
        Pane *held = screen->id ? detached_find(screen->id) : NULL;
        if (held == from->pane) { screen->vt->destroy(screen); pane_pop_to_root(held); return; }
        if (!raise_detached(screen)) pane_push(from->pane, screen);
        return;
    }
    app_push_detail(screen);
}
/// The detail's page moves, as it is, into a window of its own over where it was, and the detail empties.
static void pop_out_detail(void) {
    Screen *root = pane_root(g_detail);
    if (!root || pane_depth(g_detail) != 1 || !root->vt->detachable) return;
    RECT at; GetWindowRect(pane_hwnd(g_detail), &at);
    pane_set_root(g_panel, NULL);
    pane_set_root(g_overlay, NULL);
    app_set_browser(NULL);
    Screen *screen = pane_take_root(g_detail);
    pane_set_root(g_detail, placeholder_screen_new());
    pane_set_selected_id(g_sidebar, NULL);
    g_narrow_detail = false;
    layout();
    InvalidateRect(g_main, NULL, TRUE);
    detached_open(screen, &at, false);
}
static void post_pop_out(void *ctx) { (void)ctx; PostMessageW(g_main, WM_APP_POP_OUT, 0, 0); }
void app_clear_detail(void) {
    Screen *root = pane_root(g_detail);
    if (root && root->vt->can_leave && !root->vt->can_leave(root)) return;
    pane_set_root(g_panel, NULL);
    pane_set_root(g_overlay, NULL);
    app_set_browser(NULL);
    pane_set_root(g_detail, placeholder_screen_new());
    pane_set_selected_id(g_sidebar, NULL);
    g_narrow_detail = false;
    layout();
    InvalidateRect(g_main, NULL, TRUE);
}

// MARK: - Dialog helpers

static HRESULT task_dialog(const char *title, const char *message, const TASKDIALOG_BUTTON *buttons, UINT count, int *chosen) {
    wchar_t *wt = utf8_to_wide(title), *wm = message ? utf8_to_wide(message) : NULL;
    TASKDIALOGCONFIG cfg; memset(&cfg, 0, sizeof cfg);
    cfg.cbSize = sizeof cfg; cfg.hwndParent = app_dialog_owner(); cfg.hInstance = GetModuleHandleW(NULL);
    cfg.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW | TDF_SIZE_TO_CONTENT;
    cfg.pszWindowTitle = L"Briareus"; cfg.pszMainInstruction = wt; cfg.pszContent = wm;
    cfg.pButtons = buttons; cfg.cButtons = count; cfg.nDefaultButton = buttons[0].nButtonID;
    cfg.dwCommonButtons = TDCBF_CANCEL_BUTTON;
    int button = 0;
    app_modal_begin(cfg.hwndParent);
    HRESULT hr = g_task_dialog ? g_task_dialog(&cfg, &button, NULL, NULL) : E_NOTIMPL;
    app_modal_end();
    *chosen = button;
    free(wt); free(wm);
    return hr;
}
bool app_confirm(const char *title, const char *message, const char *continue_label, bool destructive) {
    (void)destructive;
    wchar_t *label = utf8_to_wide(continue_label ? continue_label : "Continue");
    TASKDIALOG_BUTTON buttons[1] = { { 100, label } };
    int chosen = 0;
    HRESULT hr = task_dialog(title, message, buttons, 1, &chosen);
    free(label);
    if (FAILED(hr)) {
        wchar_t *wt = utf8_to_wide(title), *wm = utf8_to_wide(message ? message : "");
        HWND owner = app_dialog_owner();
        app_modal_begin(owner);
        int r = MessageBoxW(owner, *wm ? wm : wt, wt, MB_OKCANCEL | (destructive ? MB_ICONWARNING : MB_ICONQUESTION));
        app_modal_end();
        free(wt); free(wm);
        return r == IDOK;
    }
    return chosen == 100;
}
int app_choose(const char *title, const char *message, const char *const *choices, size_t count) {
    if (!count) return -1;
    TASKDIALOG_BUTTON *buttons = xcalloc(count, sizeof *buttons);
    for (size_t i = 0; i < count; i++) { buttons[i].nButtonID = 100 + (int)i; buttons[i].pszButtonText = utf8_to_wide(choices[i]); }
    int chosen = 0;
    HRESULT hr = task_dialog(title, message, buttons, (UINT)count, &chosen);
    for (size_t i = 0; i < count; i++) free((void *)buttons[i].pszButtonText);
    free(buttons);
    if (FAILED(hr) || chosen < 100) return -1;
    return chosen - 100;
}
void app_alert(const char *title, const char *message) {
    wchar_t *wt = utf8_to_wide(title), *wm = utf8_to_wide(message ? message : "");
    HWND owner = app_dialog_owner();
    app_modal_begin(owner);
    MessageBoxW(owner, wm, wt, MB_OK | MB_ICONINFORMATION);
    app_modal_end();
    free(wt); free(wm);
}

// MARK: - Window

static void set_active(bool active) {
    if (g_store.active == active) return;
    g_store.active = active;
    media_set_active(active);
    pane_activate_all(active);
}

static LRESULT CALLBACK main_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        g_main = hwnd;
        theme_apply_window(hwnd);
        g_pairing = pane_create(hwnd, false);
        g_sidebar = pane_create(hwnd, true);
        g_detail = pane_create(hwnd, false);
        g_panel = pane_create(hwnd, true);
        g_browser = pane_create(hwnd, true);
        g_overlay = pane_create(hwnd, false);   // last, so it lies over the detail
        pane_set_overlay(g_overlay, close_overlay, NULL);
        pane_set_move_button(g_detail, 0xE8A7, "Open in a new window", post_pop_out, NULL);
        store_init(hwnd);
        g_connected_layout = true;   // forces the first rebuild
        rebuild_for_connection();
        store_restore();
        media_start(hwnd);
        updater_start();
        return 0;
    case WM_SIZE:
        // A conversation in a window of its own keeps going while the main window is minimized.
        if (wp == SIZE_MINIMIZED) set_active(detached_any_shown());
        else { set_active(GetForegroundWindow() == hwnd || g_store.active); layout(); }
        return 0;
    case WM_ACTIVATE:
        // Keys go back to where they went before the window lost the foreground, so Ctrl+F still reaches a pane.
        if (LOWORD(wp) == WA_INACTIVE) { g_focus = GetFocus(); return 0; }
        if (g_focus && IsChild(hwnd, g_focus) && IsWindowVisible(g_focus)) SetFocus(g_focus);
        else if (g_detail && IsWindowVisible(pane_hwnd(g_detail))) SetFocus(pane_hwnd(g_detail));
        else break;
        return 0;
    case WM_ACTIVATEAPP: set_active(wp != 0 && (!IsIconic(hwnd) || detached_any_shown())); return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc; GetClientRect(hwnd, &rc);
        Canvas *cv = canvas_begin_dc(hdc, &rc);
        if (cv) { fill_rect(cv, &ps.rcPaint, theme.canvas); paint_divider(cv); canvas_end_dc(cv); }
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_GETMINMAXINFO: { MINMAXINFO *mmi = (MINMAXINFO *)lp; mmi->ptMinTrackSize.x = px(420); mmi->ptMinTrackSize.y = px(360); return 0; }
    case WM_APP_STORE_CHANGED: slack_inbox_store_changed(); rebuild_for_connection(); return 0;
    case WM_APP_POP_OUT: pop_out_detail(); return 0;
    case WM_APP_MEDIA_CHANGED: if (g_sidebar) pane_footer_changed(g_sidebar); return 0;
    case WM_APP_REQUEST_DONE: case WM_APP_ASYNC_DONE: store_handle_message(msg, wp, lp); return 0;
    case WM_SETTINGCHANGE: case WM_THEMECHANGED:
        // Switching Windows between dark and light app mode arrives as WM_SETTINGCHANGE("ImmersiveColorSet").
        if (!theme_refresh() && msg == WM_SETTINGCHANGE) return 0;
        theme_apply_window(hwnd);
        if (g_pairing) { SendMessageW(pane_hwnd(g_pairing), WM_THEMECHANGED, 0, 0); pane_relayout(g_pairing); }
        if (g_sidebar) { SendMessageW(pane_hwnd(g_sidebar), WM_THEMECHANGED, 0, 0); pane_relayout(g_sidebar); }
        if (g_detail) { SendMessageW(pane_hwnd(g_detail), WM_THEMECHANGED, 0, 0); pane_relayout(g_detail); }
        if (g_panel) { SendMessageW(pane_hwnd(g_panel), WM_THEMECHANGED, 0, 0); pane_relayout(g_panel); }
        if (g_browser) { SendMessageW(pane_hwnd(g_browser), WM_THEMECHANGED, 0, 0); pane_relayout(g_browser); }
        if (g_overlay) { SendMessageW(pane_hwnd(g_overlay), WM_THEMECHANGED, 0, 0); pane_relayout(g_overlay); }
        detached_themed();
        InvalidateRect(hwnd, NULL, TRUE);
        return 0;
    case WM_DPICHANGED: {
        theme_set_dpi(HIWORD(wp));
        RECT *rc = (RECT *)lp;
        SetWindowPos(hwnd, NULL, rc->left, rc->top, rc->right - rc->left, rc->bottom - rc->top, SWP_NOZORDER | SWP_NOACTIVATE);
        if (g_pairing) pane_relayout(g_pairing);
        if (g_sidebar) pane_relayout(g_sidebar);
        if (g_detail) pane_relayout(g_detail);
        if (g_panel) pane_relayout(g_panel);
        if (g_browser) pane_relayout(g_browser);
        if (g_overlay) pane_relayout(g_overlay);
        layout();
        return 0;
    }
    case WM_SETCURSOR:
        if ((HWND)wp == hwnd && LOWORD(lp) == HTCLIENT) {
            POINT pt; GetCursorPos(&pt); ScreenToClient(hwnd, &pt);
            if (g_dragging_splitter || over_splitter(pt.x, pt.y)) { SetCursor(LoadCursorW(NULL, IDC_SIZEWE)); return TRUE; }
        }
        break;
    case WM_LBUTTONDOWN:
        if (over_splitter((short)LOWORD(lp), (short)HIWORD(lp))) { g_dragging_splitter = true; SetCapture(hwnd); InvalidateRect(hwnd, NULL, FALSE); }
        return 0;
    case WM_MOUSEMOVE:
        if (g_dragging_splitter) {
            RECT rc; GetClientRect(hwnd, &rc);
            int x = (short)LOWORD(lp), w = rc.right - x - px(SPLITTER_WIDTH) / 2;
            if (w < px(BROWSER_MIN_WIDTH)) w = px(BROWSER_MIN_WIDTH);
            if (w != g_browser_width) { g_browser_width = w; layout(); InvalidateRect(hwnd, NULL, FALSE); }
        }
        return 0;
    case WM_LBUTTONUP: if (g_dragging_splitter) ReleaseCapture(); return 0;
    case WM_CAPTURECHANGED: if (g_dragging_splitter) { g_dragging_splitter = false; InvalidateRect(hwnd, NULL, FALSE); } return 0;
    case WM_CLOSE: {
        // Open SSH and SFTP sessions end with the app, so it asks first.
        size_t ssh = 0, sftp = sftp_live_count();
        for (size_t i = 0; i < term_count(); i++) if (term_running(term_at(i))) ssh++;
        size_t live = ssh + sftp;
        if (live) {
            char message[160];
            const char *kind = !sftp ? "SSH" : !ssh ? "SFTP" : "SSH and SFTP";
            snprintf(message, sizeof message, "%zu %s session%s still open; closing Briareus disconnects %s.", live, kind, live == 1 ? " is" : "s are", live == 1 ? "it" : "them");
            if (!app_confirm("Close Briareus?", message, "Close", true)) { updater_restart_cancelled(); return 0; }
        }
        DestroyWindow(hwnd);
        return 0;
    }
    case WM_ENDSESSION:
        // Windows ends the process once this returns, without WM_DESTROY: the teardown runs here, so what it saves
        // (a repository index read halfway) is kept.
        if (wp) DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        meeting_shutdown();
        detached_close_all();
        term_shutdown();
        sftp_shutdown();
        media_stop();
        // The overlay goes first, and its pointer with it: the board's tab, freed with the detail, closes it.
        pane_destroy(g_overlay); g_overlay = NULL;
        pane_destroy(g_pairing); pane_destroy(g_sidebar); pane_destroy(g_detail); pane_destroy(g_panel); pane_destroy(g_browser);
        g_pairing = g_sidebar = g_detail = g_panel = g_browser = g_overlay = NULL;
        store_shutdown();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous, PWSTR command_line, int show) {
    (void)previous; (void)command_line;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    // The manifest (common controls v6) is activated here rather than at load, so it can live beside MinGW's default one.
    wchar_t exe[MAX_PATH]; GetModuleFileNameW(instance, exe, MAX_PATH);
    ACTCTXW ctx; memset(&ctx, 0, sizeof ctx); ctx.cbSize = sizeof ctx;
    ctx.dwFlags = ACTCTX_FLAG_RESOURCE_NAME_VALID | ACTCTX_FLAG_HMODULE_VALID; ctx.hModule = instance; ctx.lpSource = exe; ctx.lpResourceName = MAKEINTRESOURCEW(IDR_MANIFEST);
    HANDLE act = CreateActCtxW(&ctx); ULONG_PTR cookie = 0;
    if (act != INVALID_HANDLE_VALUE) ActivateActCtx(act, &cookie);
    HMODULE comctl = LoadLibraryW(L"comctl32.dll");
    if (comctl) g_task_dialog = (TaskDialogIndirectFn)(void *)GetProcAddress(comctl, "TaskDialogIndirect");
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX icc = { sizeof icc, ICC_STANDARD_CLASSES | ICC_WIN95_CLASSES };
    InitCommonControlsEx(&icc);
    theme_init();
    // Started by ssh as its SSH_ASKPASS, for an SFTP session's password or host key: the prompt alone, no window.
    int askpass_exit;
    if (sftp_askpass_main(&askpass_exit)) { CoUninitialize(); return askpass_exit; }
    WNDCLASSEXW wc; memset(&wc, 0, sizeof wc);
    wc.cbSize = sizeof wc; wc.lpfnWndProc = main_proc; wc.hInstance = instance; wc.lpszClassName = MAIN_CLASS;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_APP)); wc.hIconSm = wc.hIcon;
    RegisterClassExW(&wc);
    // Sized for the dashboard's three columns at the monitor's DPI.
    HWND hwnd = CreateWindowExW(0, MAIN_CLASS, L"Briareus", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1400, 900, NULL, NULL, instance, NULL);
    if (!hwnd) return 1;
    int dpi = GetDpiForWindow(hwnd);
    if (dpi != 96) {
        theme_set_dpi(dpi);
        SetWindowPos(hwnd, NULL, 0, 0, MulDiv(1400, dpi, 96), MulDiv(900, dpi, 96), SWP_NOMOVE | SWP_NOZORDER);
        if (g_pairing) pane_relayout(g_pairing);
        if (g_sidebar) pane_relayout(g_sidebar);
        if (g_detail) pane_relayout(g_detail);
    }
    // The app always opens filling the screen, whatever the shortcut asks.
    (void)show;
    ShowWindow(hwnd, SW_SHOWMAXIMIZED);
    UpdateWindow(hwnd);
    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) {
        if (pane_find_message(&m)) continue;
        if (m.message == WM_KEYDOWN && (GetKeyState(VK_MENU) & 0x8000) && m.wParam == VK_LEFT) {
            // Alt+Left goes back in whichever pane has the focus: the detail's, or a window of its own's.
            HWND focus = GetFocus();
            Pane *pane = focus ? detached_pane_of(focus) : NULL;
            if (!pane && g_detail && focus && (focus == pane_hwnd(g_detail) || IsChild(pane_hwnd(g_detail), focus))) pane = g_detail;
            if (pane && !term_is_window(focus)) { SendMessageW(pane_hwnd(pane), WM_KEYDOWN, VK_LEFT, 0); continue; }
        }
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    updater_relaunch_if_asked();
    CoUninitialize();
    return 0;
}
