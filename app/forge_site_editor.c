#include "forge_site_internal.h"

static wchar_t *to_edit_text(const char *text);
static void paint_editor_box(Doc *doc, Item *it, Canvas *cv, const RECT *rc);
static LRESULT CALLBACK editor_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref);

// MARK: - The editors' text

/// Forge's text with "\n" line ends, as the edit control's with "\r\n", and back.
static wchar_t *to_edit_text(const char *text) {
    Str s; str_init(&s);
    for (const char *p = text ? text : ""; *p; p++) {
        if (*p == '\r') continue;
        if (*p == '\n') str_appendc(&s, '\r');
        str_appendc(&s, *p);
    }
    wchar_t *w = utf8_to_wide(s.data ? s.data : "");
    str_free(&s);
    return w;
}

char *forge_site_editor_text(HWND e) {
    int n = GetWindowTextLengthW(e);
    wchar_t *w = xcalloc((size_t)n + 1, sizeof *w);
    GetWindowTextW(e, w, n + 1);
    char *u = wide_to_utf8(w);
    SecureZeroMemory(w, (size_t)n * sizeof *w);
    free(w);
    char *q = u;
    for (const char *p = u; *p; p++) if (*p != '\r') *q++ = *p;
    *q = 0;
    return u;
}

void forge_site_fill_editor(ForgeSite *v, int ed) {
    if (!v->edits[ed]) return;
    v->filling = true;
    wchar_t *w = to_edit_text(v->saved[ed]);
    SetWindowTextW(v->edits[ed], w);
    free(w);
    v->filling = false;
    v->changed[ed] = false;
}

bool forge_site_editor_changed(ForgeSite *v, int ed) {
    if (!v->loaded[ed] || !v->edits[ed]) return false;
    char *now = forge_site_editor_text(v->edits[ed]);
    bool differs = !str_eq(now, v->saved[ed] ? v->saved[ed] : "");
    free(now);
    return differs || (ed == ED_SCRIPT && v->auto_source != v->saved_auto_source);
}

void forge_site_note_change(ForgeSite *v, int ed) {
    if (v->filling) return;
    bool changed = forge_site_editor_changed(v, ed);
    if (changed != v->changed[ed]) { v->changed[ed] = changed; forge_site_refresh_view(v); }
    if (v->notice[ed]) { set_string(&v->notice[ed], NULL); forge_site_refresh_view(v); }
}

// MARK: - Layout

static void paint_editor_box(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    (void)doc;
    ForgeSite *v = it->data;
    HWND e = v->edits[it->arg];
    fill_round_rect(cv, rc, px(6), theme.field, e && GetFocus() == e ? theme.accent_dim : theme.line);
}

/// The editor `ed` in a box from `doc->y` to the bottom of the pane.
void forge_site_layout_editor(ForgeSite *v, Doc *doc, int x, int w, int ed) {
    RECT view = pane_content_rect(v->host->pane);
    int h = (view.bottom - view.top) - doc->y - px(40);
    if (h < px(260)) h = px(260);
    RECT box = { x, doc->y, x + w, doc->y + h };
    Item *it = doc_item(doc, doc_add(doc, &box, paint_editor_box));
    it->data = v; it->arg = ed; it->action = v->base + A_FOCUS;
    v->rects[ed] = (RECT){ box.left + px(8), box.top + px(6), box.right - px(4), box.bottom - px(4) };
    v->laid[ed] = true;
    doc->y = box.bottom;
}

// MARK: - The editors

void forge_site_place(ForgeSite *v, const RECT *content, int scroll_y, bool shown) {
    v->shown = shown;
    int m = shown && content ? forge_site_margin_of(v->host->pane) : 0;
    for (int ed = 0; ed < ED_COUNT; ed++) {
        HWND e = v->edits[ed];
        if (!e) continue;
        if (!shown || !content || !v->laid[ed] || forge_site_open_editor(v) != ed) { ShowWindow(e, SW_HIDE); continue; }
        RECT r = { content->left + m + v->rects[ed].left, content->top + v->rects[ed].top - scroll_y, content->left + m + v->rects[ed].right, content->top + v->rects[ed].bottom - scroll_y };
        RECT visible;
        if (!IntersectRect(&visible, &r, content)) { ShowWindow(e, SW_HIDE); continue; }
        MoveWindow(e, r.left, r.top, r.right - r.left, r.bottom - r.top, TRUE);
        bool clipped = !EqualRect(&visible, &r);
        if (clipped) SetWindowRgn(e, CreateRectRgn(visible.left - r.left, visible.top - r.top, visible.right - r.left, visible.bottom - r.top), TRUE);
        else if (v->clipped[ed]) SetWindowRgn(e, NULL, TRUE);
        v->clipped[ed] = clipped;
        ShowWindow(e, SW_SHOWNA);
    }
}

static LRESULT CALLBACK editor_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref) {
    ForgeSite *v = (ForgeSite *)ref;
    bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    switch (msg) {
    case WM_KEYDOWN:
        if (ctrl && wp == 'S') { forge_site_save(v); return 0; }
        if (ctrl && wp == 'A') { SendMessageW(hwnd, EM_SETSEL, 0, -1); return 0; }
        if (wp == VK_ESCAPE) { SetFocus(GetParent(hwnd)); return 0; }
        break;
    case WM_CHAR:
        if (wp == 0x13 || wp == 0x01 || wp == 0x1B) return 0;
        break;
    case WM_NCDESTROY: RemoveWindowSubclass(hwnd, editor_proc, id); break;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

void forge_site_ensure_editors(ForgeSite *v) {
    if (v->edits[0] || !v->host->pane) return;
    // Created with the pane as parent, so their notifications reach the board (see forge_site_command).
    HWND owner = pane_hwnd(v->host->pane);
    for (int ed = 0; ed < ED_COUNT; ed++) {
        DWORD style = WS_CHILD | WS_TABSTOP | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_AUTOHSCROLL | ES_WANTRETURN;
        HWND e = CreateWindowExW(0, L"EDIT", L"", style, 0, 0, 10, 10, owner, (HMENU)(INT_PTR)(ID_EDITOR + ed), GetModuleHandleW(NULL), NULL);
        SendMessageW(e, WM_SETFONT, (WPARAM)font(FONT_MONO_SMALL), TRUE);
        SendMessageW(e, EM_SETLIMITTEXT, 1024 * 1024, 0);
        SetWindowSubclass(e, editor_proc, (UINT_PTR)ed, (DWORD_PTR)v);
        theme_apply_control(e);
        v->edits[ed] = e;
        if (v->loaded[ed]) forge_site_fill_editor(v, ed);
    }
}
