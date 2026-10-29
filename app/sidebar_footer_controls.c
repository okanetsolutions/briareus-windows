#include "screen_projects_internal.h"
#include "canvas.h"
#include <commctrl.h>

static const int actions[] = { ACT_SSH, ACT_SETTINGS, ACT_SIGN_OUT };
static const wchar_t *names[] = { L"SSH servers", L"Settings", L"Sign out" };
static const wchar_t *tips[] = { L"SSH servers (Ctrl+Shift+H)", L"Settings", L"Sign out" };
static const wchar_t glyphs[] = { 0xE756, 0xE713, 0xE8AC };

static LRESULT CALLBACK button_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref) {
    FooterRects *f = (FooterRects *)ref;
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps; HDC dc = BeginPaint(hwnd, &ps); RECT rc; GetClientRect(hwnd, &rc);
        Canvas *cv = canvas_begin_dc(dc, &rc);
        POINT pt; GetCursorPos(&pt); ScreenToClient(hwnd, &pt);
        bool hot = PtInRect(&rc, pt), focused = GetFocus() == hwnd;
        fill_rect(cv, &rc, theme.sidebar);
        fill_round_rect(cv, &rc, px(7), (SendMessageW(hwnd, BM_GETSTATE, 0, 0) & BST_PUSHED) ? theme.accent_dim : theme.raise, hot || focused ? theme.accent : theme.line);
        draw_glyph(cv, glyphs[id], &rc, FONT_ICON_SMALL, theme.ink);
        canvas_end_dc(cv); EndPaint(hwnd, &ps); return 0;
    }
    if (msg == WM_SETCURSOR) { SetCursor(LoadCursorW(NULL, IDC_HAND)); return TRUE; }
    if (msg == WM_KEYDOWN && wp == VK_TAB) {
        int step = GetKeyState(VK_SHIFT) & 0x8000 ? -1 : 1;
        int next = (int)id + step;
        while (next >= 0 && next < 3) {
            if (IsWindowVisible(f->buttons[next])) { SetFocus(f->buttons[next]); return 0; }
            next += step;
        }
        SetFocus(GetParent(hwnd)); return 0;
    }
    if (msg == WM_CHAR && (wp == VK_TAB || wp == VK_ESCAPE)) return 0;
    if (msg == WM_KEYDOWN && wp == VK_ESCAPE) { SetFocus(GetParent(hwnd)); return 0; }
    if (msg == WM_SETFOCUS || msg == WM_KILLFOCUS || msg == WM_MOUSEMOVE || msg == WM_MOUSELEAVE)
        InvalidateRect(hwnd, NULL, FALSE);
    if (msg == WM_NCDESTROY) RemoveWindowSubclass(hwnd, button_proc, id);
    return DefSubclassProc(hwnd, msg, wp, lp);
}
void screen_projects_footer_controls(Screen *s, FooterRects *f) {
    RECT *rects[] = { &f->ssh_rc, &f->settings_rc, &f->signout_rc };
    HWND parent = pane_hwnd(s->pane);
    for (int i = 0; i < 3; i++) {
        if (!f->buttons[i]) {
            f->buttons[i] = CreateWindowExW(0, L"BUTTON", names[i], WS_CHILD | WS_TABSTOP | BS_PUSHBUTTON,
                0, 0, 0, 0, parent, (HMENU)(INT_PTR)actions[i], GetModuleHandleW(NULL), NULL);
            SetWindowSubclass(f->buttons[i], button_proc, (UINT_PTR)i, (DWORD_PTR)f);
            theme_apply_control(f->buttons[i]);
        }
        RECT r = *rects[i];
        MoveWindow(f->buttons[i], r.left, r.top, r.right - r.left, r.bottom - r.top, TRUE);
        ShowWindow(f->buttons[i], IsRectEmpty(&r) ? SW_HIDE : SW_SHOWNA);
        InvalidateRect(f->buttons[i], NULL, FALSE);
    }
    if (!f->tip) {
        f->tip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, NULL, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
            0, 0, 0, 0, parent, NULL, GetModuleHandleW(NULL), NULL);
        for (int i = 0; i < 3; i++) {
            TTTOOLINFOW ti = { TTTOOLINFOW_V2_SIZE, TTF_IDISHWND | TTF_SUBCLASS, parent, (UINT_PTR)f->buttons[i] };
            ti.lpszText = (wchar_t *)tips[i]; SendMessageW(f->tip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
        }
    }
}
void screen_projects_footer_hide(FooterRects *f) {
    for (int i = 0; i < 3; i++) if (f->buttons[i]) ShowWindow(f->buttons[i], SW_HIDE);
}
void screen_projects_footer_free(FooterRects *f) {
    if (f->tip) DestroyWindow(f->tip);
    for (int i = 0; i < 3; i++) if (f->buttons[i]) DestroyWindow(f->buttons[i]);
}
void screen_projects_footer_command(Screen *s, int id, int code, HWND control) {
    if (code != BN_CLICKED || !control) return;
    for (int i = 0; i < 3; i++) if (id == actions[i]) { screen_projects_sidebar_common_action(s->pane, id); return; }
}

bool screen_projects_footer_key(FooterRects *f, WPARAM vk, bool ctrl, bool shift) {
    if (ctrl || vk != VK_TAB) return false;
    for (int n = 0; n < 3; n++) {
        int i = shift ? 2 - n : n;
        if (f->buttons[i] && IsWindowVisible(f->buttons[i])) { SetFocus(f->buttons[i]); return true; }
    }
    return false;
}
