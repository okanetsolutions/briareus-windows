#include "project_ssh_internal.h"
#include "str.h"
#include <commctrl.h>
#include <stdlib.h>
#include <stdio.h>

enum { SSH_ACTION = 700, ID_SEARCH = 1600 };
typedef struct { Screen base; ProjectSsh *ssh; HWND search; RECT search_rc; bool shown, filling; } SshScreen;

static void search_text(SshScreen *s) {
    if (!s->search) return;
    int n = GetWindowTextLengthW(s->search);
    wchar_t *current = xcalloc((size_t)n + 1, sizeof *current);
    GetWindowTextW(s->search, current, n + 1);
    char *text = wide_to_utf8(current); free(current);
    bool same = str_eq(text, s->ssh->query); free(text);
    if (same) return;
    wchar_t *wide = utf8_to_wide(s->ssh->query);
    s->filling = true; SetWindowTextW(s->search, wide); s->filling = false; free(wide);
}
static void destroy(Screen *base) {
    SshScreen *s = (SshScreen *)base;
    if (s->search) DestroyWindow(s->search);
    project_ssh_free(s->ssh); free(base->id); free(s);
}
static void layout(Screen *base, Doc *doc) {
    SshScreen *s = (SshScreen *)base;
    servers_ssh_scope_changed();
    search_text(s);
    if (project_ssh_offered()) {
        doc_field_label(doc, 0, doc->width, "Search servers or projects", theme.secondary, NULL);
        SetRect(&s->search_rc, 0, doc->y, doc->width, doc->y + px(30));
        doc_space(doc, px(38));
        if (!str_empty(s->ssh->query)) doc_button(doc, 0, doc->width, "Clear search", BUTTON_PLAIN, SSH_ACTION + A_CLEAR, 0, true);
    } else SetRectEmpty(&s->search_rc);
    project_ssh_layout(s->ssh, doc, doc->width);
}
static void header(Screen *base, HeaderInfo *info) {
    SshScreen *s = (SshScreen *)base;
    snprintf(info->title, sizeof info->title, "SSH servers");
    project_ssh_header(s->ssh, info);
    if (s->ssh->loading) snprintf(info->subtitle, sizeof info->subtitle, "Loading servers…");
}
static void action(Screen *base, int act, intptr_t arg, POINT point) {
    SshScreen *s = (SshScreen *)base;
    project_ssh_action(s->ssh, act, arg, point);
    if (act == SSH_ACTION + A_CLEAR) { search_text(s); if (s->search) SetFocus(s->search); }
}
static void command(Screen *base, int id, int code, HWND control) {
    SshScreen *s = (SshScreen *)base;
    if (id != ID_SEARCH || code != EN_CHANGE || s->filling) return;
    int n = GetWindowTextLengthW(control); wchar_t *wide = xcalloc((size_t)n + 1, sizeof *wide);
    GetWindowTextW(control, wide, n + 1);
    char *text = wide_to_utf8(wide); free(wide);
    set_string(&s->ssh->query, text); free(text); pane_relayout(base->pane);
}
static LRESULT CALLBACK search_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR data) {
    (void)id; SshScreen *s = (SshScreen *)data;
    if (msg == WM_KEYDOWN) {
        bool ctrl = GetKeyState(VK_CONTROL) < 0;
        if (ctrl && wp == 'A') { SendMessageW(hwnd, EM_SETSEL, 0, -1); return 0; }
        if (wp == VK_ESCAPE) { action(&s->base, SSH_ACTION + A_CLEAR, 0, (POINT){0}); SetFocus(pane_hwnd(s->base.pane)); return 0; }
        if (wp == VK_F5) { project_ssh_refresh(s->ssh); return 0; }
        if (wp == VK_TAB || wp == VK_RETURN) {
            Term *t = project_ssh_shown_term(s->ssh);
            if (t) term_focus(t); else SetFocus(pane_hwnd(s->base.pane));
            return 0;
        }
    }
    if (msg == WM_CHAR && (wp == VK_ESCAPE || wp == VK_RETURN || wp == VK_TAB || wp == 1)) return 0;
    return DefSubclassProc(hwnd, msg, wp, lp);
}
static void place(Screen *base, const RECT *content, int scroll) {
    SshScreen *s = (SshScreen *)base;
    bool show = s->shown && project_ssh_offered();
    if (show && !s->search) {
        s->search = CreateWindowExW(0, L"EDIT", L"Search servers or projects", WS_CHILD | WS_TABSTOP | ES_AUTOHSCROLL,
            0, 0, 10, 10, pane_hwnd(base->pane), (HMENU)(INT_PTR)ID_SEARCH, GetModuleHandleW(NULL), NULL);
        theme_apply_control(s->search);
        SetWindowSubclass(s->search, search_proc, 1, (DWORD_PTR)s);
        SendMessageW(s->search, WM_SETFONT, (WPARAM)font(FONT_BODY), TRUE);
        SendMessageW(s->search, EM_SETCUEBANNER, TRUE, (LPARAM)L"Search label, host, username or project");
        search_text(s);
    }
    if (s->search) {
        if (show && content) {
            RECT r = s->search_rc;
            int margin = (content->right - content->left - pane_content_width(base->pane)) / 2;
            OffsetRect(&r, content->left + margin, content->top - scroll);
            RECT visible;
            if (IntersectRect(&visible, &r, content)) { pane_place_control(s->search, &visible); ShowWindow(s->search, SW_SHOWNA); }
            else ShowWindow(s->search, SW_HIDE);
        } else ShowWindow(s->search, SW_HIDE);
    }
    project_ssh_place(s->ssh, content, scroll, show);
}
static void visible(Screen *base, bool shown) {
    SshScreen *s = (SshScreen *)base; s->shown = shown;
    if (shown) project_ssh_refresh(s->ssh);
    else place(base, NULL, 0);
}
static bool key(Screen *base, WPARAM vk, bool ctrl, bool shift) {
    (void)shift; SshScreen *s = (SshScreen *)base;
    if (((ctrl && vk == 'F') || vk == VK_TAB) && s->search) { SetFocus(s->search); return true; }
    if (vk == VK_ESCAPE && !str_empty(s->ssh->query)) { action(base, SSH_ACTION + A_CLEAR, 0, (POINT){0}); return true; }
    return false;
}
static void refresh(Screen *base) { project_ssh_refresh(((SshScreen *)base)->ssh); }
static const ScreenVTable vt = {
    .destroy = destroy, .layout = layout, .header = header, .action = action, .command = command,
    .place = place, .visible = visible, .key = key, .refresh = refresh,
};
Screen *ssh_servers_screen_new(void) {
    SshScreen *s = xcalloc(1, sizeof *s);
    s->base.vt = &vt; s->base.id = xstrdup("ssh-servers");
    s->ssh = project_ssh_new(NULL, &s->base, SSH_ACTION);
    return &s->base;
}
