// Briareus for Windows: the main window, its two columns, and the navigation between them.
#include "dialogs.h"
#include "resource.h"
#include "screens.h"
#include "str.h"
#include "theme.h"
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

static HWND g_main;
typedef HRESULT (WINAPI *TaskDialogIndirectFn)(const TASKDIALOGCONFIG *, int *, int *, BOOL *);
static TaskDialogIndirectFn g_task_dialog;
static Pane *g_pairing, *g_sidebar, *g_detail, *g_panel;
static bool g_connected_layout;
static bool g_narrow_detail;   // in one column, whether the detail is the visible pane

HWND app_window(void) { return g_main; }
Pane *app_sidebar_pane(void) { return g_sidebar; }
Pane *app_detail_pane(void) { return g_detail; }
Pane *app_panel_pane(void) { return g_panel; }

static bool is_narrow(void) { RECT rc; GetClientRect(g_main, &rc); return rc.right - rc.left < px(NARROW_WIDTH); }
static bool panel_shown(void) { return g_panel && pane_root(g_panel) != NULL; }

static void back_to_sidebar(void *ctx) { (void)ctx; g_narrow_detail = false; PostMessageW(g_main, WM_SIZE, 0, 0); }

static void layout(void) {
    RECT rc; GetClientRect(g_main, &rc);
    if (!g_connected_layout) {
        if (g_pairing) { pane_set_bounds(g_pairing, &rc); pane_show(g_pairing, true); }
        if (g_sidebar) pane_show(g_sidebar, false);
        if (g_detail) pane_show(g_detail, false);
        if (g_panel) pane_show(g_panel, false);
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
    } else {
        pane_set_root_back(g_detail, false, NULL, NULL);
        int sw = px(SIDEBAR_WIDTH), pw = panel_shown() ? px(PANEL_WIDTH) : 0;
        RECT side = { rc.left, rc.top, rc.left + sw, rc.bottom }, main = { rc.left + sw, rc.top, rc.right - pw, rc.bottom };
        RECT panel = { rc.right - pw, rc.top, rc.right, rc.bottom };
        pane_set_bounds(g_sidebar, &side); pane_set_bounds(g_detail, &main);
        pane_show(g_sidebar, true); pane_show(g_detail, true);
        if (g_panel) { pane_set_bounds(g_panel, &panel); pane_show(g_panel, pw > 0); }
    }
}

static void paint_divider(HDC hdc) {
    if (!g_connected_layout || is_narrow()) return;
    RECT rc; GetClientRect(g_main, &rc);
    int x = px(SIDEBAR_WIDTH);
    draw_line(hdc, x - 1, rc.top, x - 1, rc.bottom, theme.line);
    if (panel_shown()) { int px_ = rc.right - px(PANEL_WIDTH); draw_line(hdc, px_, rc.top, px_, rc.bottom, theme.line); }
}

void app_set_panel(Screen *screen) {
    if (!g_panel) { if (screen) screen->vt->destroy(screen); return; }
    Screen *root = pane_root(g_panel);
    if (root && screen && screen->id && str_eq(root->id, screen->id)) { screen->vt->destroy(screen); return; }
    pane_set_root(g_panel, screen);
    layout();
    InvalidateRect(g_main, NULL, TRUE);
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
        pane_set_selected_id(g_sidebar, NULL);
        g_narrow_detail = false;
    } else {
        pane_set_root(g_sidebar, NULL); pane_set_root(g_detail, NULL); pane_set_root(g_panel, NULL);
        pane_set_root(g_pairing, pairing_screen_new());
    }
    layout();
}

void app_show_detail(Screen *screen) {
    Screen *root = pane_root(g_detail);
    if (root && screen->id && str_eq(root->id, screen->id) && pane_depth(g_detail) == 1) {
        screen->vt->destroy(screen);
    } else {
        pane_set_root(g_panel, NULL);
        pane_set_root(g_detail, screen);
    }
    pane_set_selected_id(g_sidebar, pane_root(g_detail) ? pane_root(g_detail)->id : NULL);
    g_narrow_detail = true;
    layout();
    InvalidateRect(g_main, NULL, TRUE);
}
void app_push_detail(Screen *screen) { pane_push(g_detail, screen); g_narrow_detail = true; layout(); }
void app_clear_detail(void) {
    pane_set_root(g_panel, NULL);
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
    cfg.cbSize = sizeof cfg; cfg.hwndParent = g_main; cfg.hInstance = GetModuleHandleW(NULL);
    cfg.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW | TDF_SIZE_TO_CONTENT;
    cfg.pszWindowTitle = L"Briareus"; cfg.pszMainInstruction = wt; cfg.pszContent = wm;
    cfg.pButtons = buttons; cfg.cButtons = count; cfg.nDefaultButton = buttons[0].nButtonID;
    cfg.dwCommonButtons = TDCBF_CANCEL_BUTTON;
    int button = 0;
    HRESULT hr = g_task_dialog ? g_task_dialog(&cfg, &button, NULL, NULL) : E_NOTIMPL;
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
        int r = MessageBoxW(g_main, *wm ? wm : wt, wt, MB_OKCANCEL | (destructive ? MB_ICONWARNING : MB_ICONQUESTION));
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
    MessageBoxW(g_main, wm, wt, MB_OK | MB_ICONINFORMATION);
    free(wt); free(wm);
}

// MARK: - Window

static void set_active(bool active) {
    if (g_store.active == active) return;
    g_store.active = active;
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
        store_init(hwnd);
        g_connected_layout = true;   // forces the first rebuild
        rebuild_for_connection();
        store_restore();
        return 0;
    case WM_SIZE:
        if (wp == SIZE_MINIMIZED) set_active(false);
        else { set_active(GetForegroundWindow() == hwnd || g_store.active); layout(); }
        return 0;
    case WM_ACTIVATEAPP: set_active(wp != 0 && !IsIconic(hwnd)); return 0;
    case WM_PAINT: { PAINTSTRUCT ps; HDC hdc = BeginPaint(hwnd, &ps); fill_rect(hdc, &ps.rcPaint, theme.canvas); paint_divider(hdc); EndPaint(hwnd, &ps); return 0; }
    case WM_ERASEBKGND: return 1;
    case WM_GETMINMAXINFO: { MINMAXINFO *mmi = (MINMAXINFO *)lp; mmi->ptMinTrackSize.x = px(420); mmi->ptMinTrackSize.y = px(360); return 0; }
    case WM_APP_STORE_CHANGED: rebuild_for_connection(); return 0;
    case WM_APP_REQUEST_DONE: case WM_APP_ASYNC_DONE: store_handle_message(msg, wp, lp); return 0;
    case WM_SETTINGCHANGE: case WM_THEMECHANGED:
        theme_refresh(); theme_apply_window(hwnd);
        if (g_pairing) { SendMessageW(pane_hwnd(g_pairing), WM_THEMECHANGED, 0, 0); pane_relayout(g_pairing); }
        if (g_sidebar) { SendMessageW(pane_hwnd(g_sidebar), WM_THEMECHANGED, 0, 0); pane_relayout(g_sidebar); }
        if (g_detail) { SendMessageW(pane_hwnd(g_detail), WM_THEMECHANGED, 0, 0); pane_relayout(g_detail); }
        if (g_panel) { SendMessageW(pane_hwnd(g_panel), WM_THEMECHANGED, 0, 0); pane_relayout(g_panel); }
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
        layout();
        return 0;
    }
    case WM_CLOSE: DestroyWindow(hwnd); return 0;
    case WM_DESTROY:
        pane_destroy(g_pairing); pane_destroy(g_sidebar); pane_destroy(g_detail); pane_destroy(g_panel);
        g_pairing = g_sidebar = g_detail = g_panel = NULL;
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
        if (m.message == WM_KEYDOWN && (GetKeyState(VK_MENU) & 0x8000) && m.wParam == VK_LEFT) {
            // Alt+Left goes back in whichever pane has the focus.
            HWND focus = GetFocus();
            if (focus && (g_detail && (focus == pane_hwnd(g_detail) || IsChild(pane_hwnd(g_detail), focus)))) { SendMessageW(pane_hwnd(g_detail), WM_KEYDOWN, VK_LEFT, 0); continue; }
        }
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    CoUninitialize();
    return 0;
}
