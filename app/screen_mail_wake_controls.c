#include "screen_mail_wake_internal.h"

static char *edit_text(HWND edit) {
    int n = GetWindowTextLengthW(edit); wchar_t *w = xcalloc((size_t)n + 1, sizeof *w);
    GetWindowTextW(edit, w, n + 1); char *text = wide_to_utf8(w); free(w); return text;
}
void mail_wake_capture(MailWakeScreen *s, int field) {
    if (!s->edits[field]) return;
    char *text = edit_text(s->edits[field]);
    if (field < MAIL_WAKE_CAPS) { free(s->draft.caps[field]); s->draft.caps[field] = text; }
    else {
        int row = (field - MAIL_WAKE_CAPS) / MAIL_WAKE_FILTERS, f = (field - MAIL_WAKE_CAPS) % MAIL_WAKE_FILTERS;
        Json *r = (Json *)json_at(s->draft.rules, (size_t)row);
        if (json_is_object(r)) json_set_str(r, mail_wake_filter_keys[f], text);
        free(text);
    }
}
void mail_wake_fill_controls(MailWakeScreen *s) {
    s->filling = true;
    for (int i = 0; i < MW_FIELDS; i++) {
        if (!s->edits[i]) continue;
        const char *text = i < MAIL_WAKE_CAPS ? s->draft.caps[i]
            : json_str(json_get(json_at(s->draft.rules, (size_t)(i - MAIL_WAKE_CAPS) / MAIL_WAKE_FILTERS), mail_wake_filter_keys[(i - MAIL_WAKE_CAPS) % MAIL_WAKE_FILTERS]));
        wchar_t *wide = utf8_to_wide(text ? text : ""); SetWindowTextW(s->edits[i], wide); free(wide);
    }
    s->filling = false;
}
static void focus_next(MailWakeScreen *s, int field, bool backwards) {
    for (int n = 0; n < MW_FIELDS; n++) {
        field = (field + (backwards ? MW_FIELDS - 1 : 1)) % MW_FIELDS;
        if (s->laid[field] && IsWindowEnabled(s->edits[field]) && mail_wake_editable(s)) {
            RECT content = pane_content_rect(s->base.pane);
            int scroll = pane_scroll_y(s->base.pane), height = content.bottom - content.top;
            if (s->rects[field].top < scroll || s->rects[field].bottom > scroll + height)
                pane_scroll_to(s->base.pane, s->rects[field].top);
            SetFocus(s->edits[field]); return;
        }
    }
    SetFocus(pane_hwnd(s->base.pane));
}
static LRESULT CALLBACK field_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref) {
    MailWakeScreen *s = (MailWakeScreen *)ref; bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    if (msg == WM_KEYDOWN) {
        if (wp == VK_TAB) { focus_next(s, (int)id, (GetKeyState(VK_SHIFT) & 0x8000) != 0); return 0; }
        if (wp == VK_ESCAPE) { SetFocus(GetParent(hwnd)); return 0; }
        if (ctrl && wp == 'A') { SendMessageW(hwnd, EM_SETSEL, 0, -1); return 0; }
        if ((ctrl && wp == 'S') || wp == VK_RETURN) { mail_wake_save(s); return 0; }
    }
    if (msg == WM_CHAR) {
        if (wp == '\t' || wp == '\r' || wp == 0x13 || wp == 0x01 || wp == 0x1b) return 0;
    }
    if (msg == WM_MOUSEWHEEL) { SendMessageW(GetParent(hwnd), msg, wp, lp); return 0; }
    if (msg == WM_NCDESTROY) RemoveWindowSubclass(hwnd, field_proc, id);
    return DefSubclassProc(hwnd, msg, wp, lp);
}
void mail_wake_ensure_controls(MailWakeScreen *s) {
    if (s->edits[0] || !s->base.pane) return;
    for (int i = 0; i < MW_FIELDS; i++) {
        HWND e = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_TABSTOP | ES_AUTOHSCROLL,
            0, 0, 10, 10, pane_hwnd(s->base.pane), (HMENU)(INT_PTR)(MW_FIELD_ID + i), GetModuleHandleW(NULL), NULL);
        SendMessageW(e, WM_SETFONT, (WPARAM)font(FONT_BODY), TRUE);
        SendMessageW(e, EM_SETLIMITTEXT, 4096, 0);
        SetWindowSubclass(e, field_proc, (UINT_PTR)i, (DWORD_PTR)s); theme_apply_control(e); s->edits[i] = e;
    }
    mail_wake_fill_controls(s);
}
void mail_wake_place(Screen *base, const RECT *content, int scroll) {
    MailWakeScreen *s = (MailWakeScreen *)base;
    RECT client; GetClientRect(pane_hwnd(base->pane), &client);
    int margin = (client.right - client.left - pane_content_width(base->pane)) / 2;
    for (int i = 0; i < MW_FIELDS; i++) {
        HWND e = s->edits[i]; if (!e) continue;
        if (!s->shown || !s->laid[i] || !mail_wake_current(s)) { ShowWindow(e, SW_HIDE); continue; }
        RECT r = {content->left + margin + s->rects[i].left, content->top + s->rects[i].top - scroll,
            content->left + margin + s->rects[i].right, content->top + s->rects[i].bottom - scroll}, visible;
        if (!IntersectRect(&visible, &r, content)) { ShowWindow(e, SW_HIDE); continue; }
        MoveWindow(e, r.left, r.top, r.right - r.left, r.bottom - r.top, TRUE);
        bool clipped = !EqualRect(&visible, &r);
        if (clipped) SetWindowRgn(e, CreateRectRgn(visible.left - r.left, visible.top - r.top, visible.right - r.left, visible.bottom - r.top), TRUE);
        else if (s->clipped[i]) SetWindowRgn(e, NULL, TRUE);
        s->clipped[i] = clipped; EnableWindow(e, mail_wake_editable(s)); ShowWindow(e, SW_SHOWNA);
    }
}
