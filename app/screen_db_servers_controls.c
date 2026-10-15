#include "screen_db_servers_internal.h"

static char *edit_text(HWND edit);
static void set_edit_text(HWND edit, const char *text);
static char *field_text(const Json *row, int f);
static int margin_of(Pane *pane);
static int next_field(DbForm *s, int from, int step);
static LRESULT CALLBACK field_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref);

static char *edit_text(HWND edit) {
    int n = GetWindowTextLengthW(edit);
    wchar_t *w = xmalloc(((size_t)n + 1) * sizeof *w);
    GetWindowTextW(edit, w, n + 1);
    char *text = wide_to_utf8(w); free(w);
    return text;
}

static void set_edit_text(HWND edit, const char *text) { wchar_t *w = utf8_to_wide(text ? text : ""); SetWindowTextW(edit, w); free(w); }

/// A field's value in the row as its box shows it: a port as digits, empty for null.
static char *field_text(const Json *row, int f) {
    const Json *v = json_get(row, screen_db_servers_FIELDS[f].key);
    double n;
    if (screen_db_servers_FIELDS[f].number) return json_num(v, &n) && n > 0 ? xstrfmt("%.0f", n) : xstrdup("");
    return xstrdup(json_str(v) ? json_str(v) : "");
}

void screen_db_servers_form_fill(DbForm *s) {
    s->enabled = json_bool_is(json_get(s->row, screen_db_servers_ENABLED_KEY), true);
    s->filling = true;
    for (int f = 0; f < F_COUNT; f++) {
        if (!s->edits[f]) continue;
        char *text = field_text(s->row, f);
        set_edit_text(s->edits[f], text);
        free(text);
    }
    s->filling = false;
    s->dirty = false;
}

/// The body a save or a test sends: the row the form came from with every field as it stands now. NULL with `*why`
/// when a field cannot be sent as it is.
Json *screen_db_servers_form_body(DbForm *s, char **why) {
    Json *body = json_is_object(s->row) ? json_clone(s->row) : json_object();
    static const char *const server_keys[] = { "id", "createdAt", "updatedAt", "sortOrder", "claimedBy" };
    for (size_t i = 0; i < sizeof server_keys / sizeof *server_keys; i++) json_object_remove(body, server_keys[i]);
    if (screen_db_servers_row_has(s, screen_db_servers_ENABLED_KEY)) json_set_bool(body, screen_db_servers_ENABLED_KEY, s->enabled);
    for (int f = 0; f < F_COUNT; f++) {
        if (!s->edits[f] || !screen_db_servers_row_has(s, screen_db_servers_FIELDS[f].key)) continue;
        char *text = edit_text(s->edits[f]);
        // A password is sent as typed; spaces may belong to it.
        char *t = screen_db_servers_FIELDS[f].secret ? xstrdup(text) : str_trim(text);
        free(text);
        if (!screen_db_servers_FIELDS[f].number) { json_set_str(body, screen_db_servers_FIELDS[f].key, t); free(t); continue; }
        if (!*t) { json_object_set(body, screen_db_servers_FIELDS[f].key, json_null()); free(t); continue; }
        char *end; long port = strtol(t, &end, 10);
        bool ok = !*end && port >= 1 && port <= 65535;
        free(t);
        if (!ok) { *why = xstrdup("The port must be a whole number from 1 to 65535."); json_free(body); return NULL; }
        json_set_num(body, screen_db_servers_FIELDS[f].key, (double)port);
    }
    return body;
}

bool screen_db_servers_tab_changed(DbForm *s, int t) {
    if (t != T_SERVER) return false;
    if (screen_db_servers_row_has(s, screen_db_servers_ENABLED_KEY) && s->enabled != json_bool_is(json_get(s->row, screen_db_servers_ENABLED_KEY), true)) return true;
    for (int f = 0; f < F_COUNT; f++) {
        if (!s->edits[f] || !screen_db_servers_row_has(s, screen_db_servers_FIELDS[f].key)) continue;
        char *now = edit_text(s->edits[f]), *saved = field_text(s->row, f);
        bool differs = !str_eq(now, saved);
        free(now); free(saved);
        if (differs) return true;
    }
    return false;
}

void screen_db_servers_form_changed(DbForm *s) {
    if (s->filling) return;
    if (!s->dirty) { s->dirty = true; pane_header_changed(s->base.pane); }
    bool changed = screen_db_servers_tab_changed(s, screen_db_servers_g_tab);
    if (changed != s->tab_dot) { s->tab_dot = changed; pane_relayout(s->base.pane); }
}

// MARK: - The edits

static int margin_of(Pane *pane) { RECT rc; GetClientRect(pane_hwnd(pane), &rc); return (rc.right - rc.left - pane_content_width(pane)) / 2; }

void screen_db_servers_form_place(Screen *base, const RECT *content, int scroll_y) {
    DbForm *s = (DbForm *)base;
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

static int next_field(DbForm *s, int from, int step) {
    for (int k = 1; k <= F_COUNT; k++) {
        int f = ((from + step * k) % F_COUNT + F_COUNT) % F_COUNT;
        if (s->edits[f] && s->laid[f]) return f;
    }
    return from;
}

static LRESULT CALLBACK field_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref) {
    DbForm *s = (DbForm *)ref;
    int f = (int)id;
    bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    switch (msg) {
    case WM_KEYDOWN:
        if (wp == VK_TAB) { SetFocus(s->edits[next_field(s, f, (GetKeyState(VK_SHIFT) & 0x8000) ? -1 : 1)]); return 0; }
        if (ctrl && wp == 'S') { screen_db_servers_form_save(s); return 0; }
        if (ctrl && wp == 'A') { SendMessageW(hwnd, EM_SETSEL, 0, -1); return 0; }
        if (wp == VK_ESCAPE) { SetFocus(GetParent(hwnd)); return 0; }
        if (wp == VK_RETURN) { SetFocus(s->edits[next_field(s, f, 1)]); return 0; }
        break;
    case WM_CHAR:
        if (wp == '\t' || wp == 0x13 || wp == 0x01 || wp == 0x1B || wp == '\r') return 0;
        break;
    case WM_MOUSEWHEEL: SendMessageW(GetParent(hwnd), msg, wp, lp); return 0;
    case WM_NCDESTROY: RemoveWindowSubclass(hwnd, field_proc, id); break;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

void screen_db_servers_form_ensure_controls(DbForm *s) {
    if (s->edits[F_LABEL]) return;
    HWND owner = pane_hwnd(s->base.pane);
    for (int f = 0; f < F_COUNT; f++) {
        DWORD style = WS_CHILD | WS_TABSTOP | ES_AUTOHSCROLL | (screen_db_servers_FIELDS[f].secret ? ES_PASSWORD : 0) | (screen_db_servers_FIELDS[f].number ? ES_NUMBER : 0);
        HWND e = CreateWindowExW(0, L"EDIT", L"", style, 0, 0, 10, 10, owner, (HMENU)(INT_PTR)(ID_FIELD + f), GetModuleHandleW(NULL), NULL);
        SendMessageW(e, WM_SETFONT, (WPARAM)font(screen_db_servers_FIELDS[f].mono ? FONT_MONO : FONT_BODY), TRUE);
        if (screen_db_servers_FIELDS[f].number) SendMessageW(e, EM_SETLIMITTEXT, 5, 0);
        { wchar_t *w = utf8_to_wide(screen_db_servers_FIELDS[f].cue); SendMessageW(e, EM_SETCUEBANNER, TRUE, (LPARAM)w); free(w); }
        SetWindowSubclass(e, field_proc, (UINT_PTR)f, (DWORD_PTR)s);
        theme_apply_control(e);
        s->edits[f] = e;
    }
    screen_db_servers_form_fill(s);
}
