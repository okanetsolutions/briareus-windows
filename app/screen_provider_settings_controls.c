#include "screen_provider_settings_internal.h"

static int margin_of(Pane *pane);
static int next_field(FormScreen *s, int from, int step);
static LRESULT CALLBACK field_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref);
static int popup_menu(FormScreen *s, HMENU menu, POINT pt);
static void append_item(HMENU menu, UINT id, const char *text, bool checked);

static const struct { const char *title; wchar_t glyph; } TABS[T_COUNT] = {
    [T_PROVIDER] = { "Provider", 0xE713 },
    [T_MODELS] = { "Models", 0xE8FD }, [T_STATUS] = { "Status", 0xE9D9 },
};

int screen_provider_settings_open_tab(FormScreen *s) { return screen_provider_settings_g_tab == T_STATUS && !s->id ? T_PROVIDER : screen_provider_settings_g_tab; }

void screen_provider_settings_layout_tabs(FormScreen *s, Doc *doc, int x, int w) {
    int h = px(42), tx = x, ty = doc->y, open = screen_provider_settings_open_tab(s);
    for (int t = 0; t < T_COUNT; t++) {
        if (t == T_STATUS && !s->id) continue;
        bool changed = s->dirty && screen_provider_settings_tab_changed(s, t);
        if (t == open) s->tab_dot = changed;
        char *title = changed ? xstrfmt("%s \xE2\x80\xA2", TABS[t].title) : xstrdup(TABS[t].title);
        doc_tab(doc, &tx, &ty, x, x + w, h, TABS[t].glyph, title, NULL, t == open, ACT_TAB, t);
        free(title);
    }
    doc->y = ty + h;
    doc_rule(doc, x, w);
    doc_space(doc, px(18));
}

// MARK: The edits

static int margin_of(Pane *pane) { RECT rc; GetClientRect(pane_hwnd(pane), &rc); return (rc.right - rc.left - pane_content_width(pane)) / 2; }

void screen_provider_settings_form_place(Screen *base, const RECT *content, int scroll_y) {
    FormScreen *s = (FormScreen *)base;
    int m = margin_of(base->pane);
    for (int f = 0; f < F_COUNT; f++) {
        HWND e = s->edits[f];
        if (!e) continue;
        if (!s->shown || !s->laid[f]) { ShowWindow(e, SW_HIDE); continue; }
        RECT r = { content->left + m + s->rects[f].left, content->top + s->rects[f].top - scroll_y, content->left + m + s->rects[f].right, content->top + s->rects[f].bottom - scroll_y };
        RECT visible;
        if (!IntersectRect(&visible, &r, content)) { ShowWindow(e, SW_HIDE); continue; }
        MoveWindow(e, r.left, r.top, r.right - r.left, r.bottom - r.top, TRUE);
        bool clipped = !EqualRect(&visible, &r);
        if (clipped) SetWindowRgn(e, CreateRectRgn(visible.left - r.left, visible.top - r.top, visible.right - r.left, visible.bottom - r.top), TRUE);
        else if (s->clipped[f]) SetWindowRgn(e, NULL, TRUE);
        s->clipped[f] = clipped;
        ShowWindow(e, SW_SHOWNA);
    }
}

static int next_field(FormScreen *s, int from, int step) {
    for (int k = 1; k <= F_COUNT; k++) {
        int f = ((from + step * k) % F_COUNT + F_COUNT) % F_COUNT;
        if (s->edits[f] && s->laid[f]) return f;
    }
    return from;
}

static LRESULT CALLBACK field_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref) {
    FormScreen *s = (FormScreen *)ref;
    int f = (int)id;
    bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    switch (msg) {
    case WM_KEYDOWN:
        if (wp == VK_TAB) { SetFocus(s->edits[next_field(s, f, (GetKeyState(VK_SHIFT) & 0x8000) ? -1 : 1)]); return 0; }
        if (ctrl && wp == 'S') { screen_provider_settings_form_save(s); return 0; }
        if (ctrl && wp == 'A') { SendMessageW(hwnd, EM_SETSEL, 0, -1); return 0; }
        if (wp == VK_ESCAPE) { SetFocus(GetParent(hwnd)); return 0; }
        // Enter in the login code finishes the login; elsewhere it moves on.
        if (wp == VK_RETURN && f == F_LOGIN_CODE) { screen_provider_settings_login_finish(s); return 0; }
        if (wp == VK_RETURN && !screen_provider_settings_is_multiline(f)) { SetFocus(s->edits[next_field(s, f, 1)]); return 0; }
        break;
    case WM_CHAR:
        if (form_shortcut_character(wp, screen_provider_settings_is_multiline(f))) return 0;
        break;
    case WM_MOUSEWHEEL: {
        bool own = screen_provider_settings_is_multiline(f) && SendMessageW(hwnd, EM_GETLINECOUNT, 0, 0) > screen_provider_settings_shown_rows(s, f);
        if (!own) { SendMessageW(GetParent(hwnd), msg, wp, lp); return 0; }
        break;
    }
    case WM_NCDESTROY: RemoveWindowSubclass(hwnd, field_proc, id); break;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

void screen_provider_settings_form_ensure_controls(FormScreen *s) {
    if (s->edits[F_LABEL]) return;
    HWND owner = pane_hwnd(s->base.pane);
    for (int f = 0; f < F_COUNT; f++) {
        DWORD style = WS_CHILD | WS_TABSTOP | (screen_provider_settings_is_multiline(f) ? ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN : ES_AUTOHSCROLL) | (screen_provider_settings_FIELDS[f].secret ? ES_PASSWORD : 0);
        HWND e = CreateWindowExW(0, L"EDIT", L"", style, 0, 0, 10, 10, owner, (HMENU)(INT_PTR)(ID_FIELD + f), GetModuleHandleW(NULL), NULL);
        SendMessageW(e, WM_SETFONT, (WPARAM)font(screen_provider_settings_FIELDS[f].mono ? FONT_MONO : FONT_BODY), TRUE);
        SendMessageW(e, EM_SETLIMITTEXT, 0, 0);
        if (screen_provider_settings_FIELDS[f].cue) { wchar_t *w = utf8_to_wide(screen_provider_settings_FIELDS[f].cue); SendMessageW(e, EM_SETCUEBANNER, TRUE, (LPARAM)w); free(w); }
        SetWindowSubclass(e, field_proc, (UINT_PTR)f, (DWORD_PTR)s);
        theme_apply_control(e);
        s->edits[f] = e;
    }
    screen_provider_settings_form_fill(s);
}

static int popup_menu(FormScreen *s, HMENU menu, POINT pt) {
    int chosen = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, pt.x, pt.y, 0, pane_hwnd(s->base.pane), NULL);
    DestroyMenu(menu);
    return chosen;
}

static void append_item(HMENU menu, UINT id, const char *text, bool checked) {
    wchar_t *w = utf8_to_wide(text);
    AppendMenuW(menu, MF_STRING | (checked ? MF_CHECKED : 0), id, w);
    free(w);
}

void screen_provider_settings_pick_binary(FormScreen *s, POINT pt) {
    HMENU m = CreatePopupMenu();
    for (size_t i = 0; i < BINARY_COUNT; i++) append_item(m, (UINT)i + 1, screen_provider_settings_BINARIES[i].title, str_eq(screen_provider_settings_BINARIES[i].id, s->binary));
    int chosen = popup_menu(s, m, pt);
    if (chosen < 1 || chosen > BINARY_COUNT || str_eq(screen_provider_settings_BINARIES[chosen - 1].id, s->binary)) return;
    screen_provider_settings_cancel_login(s);
    set_string(&s->binary, screen_provider_settings_BINARIES[chosen - 1].id);
    // Another CLI's login flow and probe verdict say nothing about this one.
    set_string(&s->test_result, NULL); set_string(&s->device_code, NULL); s->code_wanted = false;
    screen_provider_settings_form_changed(s);
    pane_relayout(s->base.pane);
}

void screen_provider_settings_pick_mode(FormScreen *s, POINT pt) {
    HMENU m = CreatePopupMenu();
    append_item(m, 1, "Login", !s->token);
    append_item(m, 2, "API token", s->token);
    int chosen = popup_menu(s, m, pt);
    if (!chosen || (chosen == 2) == s->token) return;
    // Switching to Login drops the endpoint and its token when saved; switching back brings back what the boxes hold.
    screen_provider_settings_cancel_login(s);
    s->token = chosen == 2;
    set_string(&s->test_result, NULL);
    screen_provider_settings_form_changed(s);
    pane_relayout(s->base.pane);
}
