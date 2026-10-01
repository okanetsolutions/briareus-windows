// One server of the database pool, as the dashboard's database server form, its fields on tabs along the top as the
// project form lays out its own: Server (where it is, how sessions sign in to it, and Test connection) and Pool. Saved
// through /settings/db-servers, which needs an Admin token as the project routes do.
#include "screens.h"
#include "str.h"
#include <commctrl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int row_id(const Json *row) { return json_int_or(json_get(row, "id"), 0); }
static char *form_id(int id) { return id > 0 ? xstrfmt("settings-db:%d", id) : xstrdup("settings-db:new"); }

// MARK: - The fields

enum { F_LABEL, F_HOST, F_PORT, F_USERNAME, F_PASSWORD, F_COUNT };
/// One of the form's boxes: the DbServer key it edits and the words around it.
typedef struct { const char *key, *label, *cue; bool number, secret, mono; } FieldDef;
static const FieldDef FIELDS[F_COUNT] = {
    [F_LABEL] = { "label", "Label", "defaults to host:port", false, false, false },
    [F_HOST] = { "host", "Host", "127.0.0.1", false, false, true },
    [F_PORT] = { "port", "Port", "3306", true, false, true },
    [F_USERNAME] = { "username", "Username", "root", false, false, true },
    [F_PASSWORD] = { "password", "Password", "empty = no password", false, true, true },
};
static const char *const ENABLED_KEY = "enabled";

/// Every field is on Server; Pool only reads.
enum { T_SERVER, T_POOL, T_COUNT };
static const struct { const char *title; wchar_t glyph; } TABS[T_COUNT] = {
    [T_SERVER] = { "Server", 0xE1D3 }, [T_POOL] = { "Pool", 0xE716 },
};
/// The open tab stays open from one server to the next, as the project form's does.
static int g_tab;

enum { ACT_SAVE = 1200, ACT_CLONE, ACT_DELETE, ACT_TAB, ACT_TOGGLE, ACT_FOCUS, ACT_TEST };
enum { ID_FIELD = 2100 };

typedef struct {
    Screen base;
    Json *row;             // what the form was filled from: the saved row, the defaults, or a clone's values
    int id;                // 0 until the server is saved
    HWND edits[F_COUNT];
    RECT rects[F_COUNT];   // each edit's place in content coordinates, from the last layout
    bool laid[F_COUNT], clipped[F_COUNT];
    bool enabled;          // In the pool
    Request *req_save, *req_delete, *req_test;
    bool dirty, filling, shown, tab_dot;
    int focused, focus_first;
    char *error;
    char *test_text; bool test_ok;   // what the last Test connection said, green when the server answered
} DbForm;

static bool row_has(DbForm *s, const char *key) {
    if (!json_count(s->row)) return true;
    for (size_t i = 0; i < json_count(s->row); i++) if (str_eq(json_key(s->row, i), key)) return true;
    return false;
}

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
    const Json *v = json_get(row, FIELDS[f].key);
    double n;
    if (FIELDS[f].number) return json_num(v, &n) && n > 0 ? xstrfmt("%.0f", n) : xstrdup("");
    return xstrdup(json_str(v) ? json_str(v) : "");
}
static void form_fill(DbForm *s) {
    s->enabled = json_bool_is(json_get(s->row, ENABLED_KEY), true);
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
static Json *form_body(DbForm *s, char **why) {
    Json *body = json_is_object(s->row) ? json_clone(s->row) : json_object();
    static const char *const server_keys[] = { "id", "createdAt", "updatedAt", "sortOrder", "claimedBy" };
    for (size_t i = 0; i < sizeof server_keys / sizeof *server_keys; i++) json_object_remove(body, server_keys[i]);
    if (row_has(s, ENABLED_KEY)) json_set_bool(body, ENABLED_KEY, s->enabled);
    for (int f = 0; f < F_COUNT; f++) {
        if (!s->edits[f] || !row_has(s, FIELDS[f].key)) continue;
        char *text = edit_text(s->edits[f]);
        // A password is sent as typed; spaces may belong to it.
        char *t = FIELDS[f].secret ? xstrdup(text) : str_trim(text);
        free(text);
        if (!FIELDS[f].number) { json_set_str(body, FIELDS[f].key, t); free(t); continue; }
        if (!*t) { json_object_set(body, FIELDS[f].key, json_null()); free(t); continue; }
        char *end; long port = strtol(t, &end, 10);
        bool ok = !*end && port >= 1 && port <= 65535;
        free(t);
        if (!ok) { *why = xstrdup("The port must be a whole number from 1 to 65535."); json_free(body); return NULL; }
        json_set_num(body, FIELDS[f].key, (double)port);
    }
    return body;
}

static bool tab_changed(DbForm *s, int t) {
    if (t != T_SERVER) return false;
    if (row_has(s, ENABLED_KEY) && s->enabled != json_bool_is(json_get(s->row, ENABLED_KEY), true)) return true;
    for (int f = 0; f < F_COUNT; f++) {
        if (!s->edits[f] || !row_has(s, FIELDS[f].key)) continue;
        char *now = edit_text(s->edits[f]), *saved = field_text(s->row, f);
        bool differs = !str_eq(now, saved);
        free(now); free(saved);
        if (differs) return true;
    }
    return false;
}
static void form_changed(DbForm *s) {
    if (s->filling) return;
    if (!s->dirty) { s->dirty = true; pane_header_changed(s->base.pane); }
    bool changed = tab_changed(s, g_tab);
    if (changed != s->tab_dot) { s->tab_dot = changed; pane_relayout(s->base.pane); }
}

// MARK: - Layout

static void layout_tabs(DbForm *s, Doc *doc, int x, int w) {
    int h = px(42), tx = x, ty = doc->y;
    for (int t = 0; t < T_COUNT; t++) {
        bool changed = s->dirty && tab_changed(s, t);
        if (t == g_tab) s->tab_dot = changed;
        char *title = changed ? xstrfmt("%s \xE2\x80\xA2", TABS[t].title) : xstrdup(TABS[t].title);
        doc_tab(doc, &tx, &ty, x, x + w, h, TABS[t].glyph, title, NULL, t == g_tab, ACT_TAB, t);
        free(title);
    }
    doc->y = ty + h;
    doc_rule(doc, x, w);
    doc_space(doc, px(18));
}

static void paint_box(Doc *doc, Item *it, HDC hdc, const RECT *rc) {
    DbForm *s = it->data;
    (void)doc;
    fill_round_rect(hdc, rc, px(6), theme.raise, s->focused == (int)it->arg ? theme.accent_dim : theme.line);
}
/// A labelled box with its edit at (x, cursor); advances.
static void field(DbForm *s, Doc *doc, int x, int w, int f) {
    if (!row_has(s, FIELDS[f].key)) return;
    doc_text(doc, x, w, FIELDS[f].label, FONT_FOOTNOTE, theme.ink, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
    doc_space(doc, px(6));
    int fh = font_height(doc->hdc, FIELDS[f].mono ? FONT_MONO : FONT_BODY), h = px(36);
    RECT box = { x, doc->y, x + w, doc->y + h };
    Item *it = doc_item(doc, doc_add(doc, &box, paint_box));
    it->data = s; it->arg = f; it->action = ACT_FOCUS;
    s->rects[f] = (RECT){ x + px(10), box.top + (h - fh) / 2, x + w - px(10), box.top + (h - fh) / 2 + fh };
    s->laid[f] = true;
    doc->y = box.bottom;
    doc_space(doc, px(14));
}
/// Two fields side by side, the second `right_w` wide (the rest when 0); advances past the taller.
static void field_pair(DbForm *s, Doc *doc, int x, int w, int left, int right, int right_w) {
    int gap = px(14), rw = right_w ? right_w : (w - gap) / 2, top = doc->y;
    field(s, doc, x, w - gap - rw, left);
    int bottom = doc->y;
    doc->y = top;
    field(s, doc, x + w - rw, rw, right);
    if (doc->y < bottom) doc->y = bottom;
}

static void paint_check(Doc *doc, Item *it, HDC hdc, const RECT *rc) {
    DbForm *s = it->data;
    bool hovered = doc->hover >= 0 && doc_item(doc, doc->hover) == it;
    int size = px(15), top = rc->top + (rc->bottom - rc->top - size) / 2;
    RECT b = { rc->left, top, rc->left + size, top + size };
    fill_round_rect(hdc, &b, px(3), s->enabled ? theme.accent : theme.field, s->enabled ? theme.accent : hovered ? theme.accent_dim : theme.line_strong);
    if (s->enabled) draw_glyph(hdc, 0xE73E, &b, FONT_ICON_SMALL, theme.on_accent);
    RECT t = { b.right + px(8), rc->top, rc->right, rc->bottom };
    draw_text(hdc, "In the pool: sessions may claim this server", &t, FONT_FOOTNOTE, theme.ink, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}
static void note(Doc *doc, int x, int w, const char *text) {
    doc_text(doc, x, w, text, FONT_CAPTION, theme.muted, DT_LEFT | DT_WORDBREAK);
    doc_space(doc, px(10));
}

/// Test connection and what it last said beside it.
static void layout_test(DbForm *s, Doc *doc, int x, int w) {
    if (!store_supports("test_db_server")) return;
    int top = doc->y;
    int i = doc_button(doc, x, 0, s->req_test ? "Connecting\xE2\x80\xA6" : "Test connection", BUTTON_BORDERED, ACT_TEST, 0, !s->req_test);
    RECT b = doc_item(doc, i)->rc;
    int bottom = doc->y;
    if (s->test_text) {
        doc->y = top;
        COLORREF color = s->test_ok ? theme.ok : theme.danger;
        int tx = b.right + px(10);
        RECT r = { tx, b.top, x + w, b.bottom };
        // One line beside the button when it fits, wrapped under it when it does not.
        if (text_width(doc->hdc, s->test_text, FONT_CAPTION) <= r.right - r.left) doc_text_at(doc, &r, s->test_text, FONT_CAPTION, color, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        else { doc->y = bottom + px(8); doc_text(doc, x, w, s->test_text, FONT_CAPTION, color, DT_LEFT | DT_WORDBREAK); bottom = doc->y; }
    }
    doc->y = bottom;
    doc_space(doc, px(14));
}

/// The pool as a whole: how many sessions it lets open at once, and the projects that claim from it.
static void layout_pool(DbForm *s, Doc *doc, int x, int w) {
    (void)s;
    note(doc, x, w, "One session holds a server at a time, so the pool is the session cap: as many sessions may be open at once as there are entries in it. The session's project decides which database name is used on it (created if missing); its setup commands are where migrations and seeding happen, on every session.");
    size_t n = settings_pool_capacity(), total = json_count(settings_db_server_rows());
    char *off = total > n ? xstrfmt(" %zu more %s listed but taken out of the pool.", total - n, total - n == 1 ? "is" : "are") : xstrdup("");
    char *count = n ? xstrfmt("%zu server%s in the pool, so %zu session%s with a database may be open at once.%s", n, n == 1 ? "" : "s", n, n == 1 ? "" : "s", off)
                    : xstrfmt("No server is in the pool yet.%s", off);
    doc_text(doc, x, w, count, FONT_FOOTNOTE, theme.ink, DT_LEFT | DT_WORDBREAK);
    free(count); free(off);
    doc_space(doc, px(18));
    doc_text(doc, x, w, "Projects that claim a server", FONT_FOOTNOTE_SEMIBOLD, theme.ink, DT_LEFT | DT_SINGLELINE);
    doc_space(doc, px(8));
    const Json *projects = settings_project_rows();
    size_t claiming = 0;
    for (size_t i = 0; i < json_count(projects); i++) {
        const Json *p = json_at(projects, i);
        if (!json_bool_is(json_get(p, "dbPoolEnabled"), true)) continue;
        const char *repo = json_str_nonempty(json_get(p, "repo")), *label = json_str_nonempty(json_get(p, "label"));
        const char *db = json_str_nonempty(json_get(p, "dbPoolDatabase"));
        char *line = db ? xstrfmt("%s \xC2\xB7 database %s", label ? label : repo ? repo : "Project", db) : xstrdup(label ? label : repo ? repo : "Project");
        doc_label(doc, x, w, 0xE8B7, line, FONT_FOOTNOTE, theme.ink);
        free(line);
        doc_space(doc, px(4));
        claiming++;
    }
    if (!claiming) note(doc, x, w, "None yet. A project claims a server once \xE2\x80\x9CGive each session a database server of its own\xE2\x80\x9D is ticked on its Database tab.");
}

static void form_layout(Screen *base, Doc *doc) {
    DbForm *s = (DbForm *)base;
    memset(s->laid, 0, sizeof s->laid);
    int col = doc->width, x = 0;
    doc_space(doc, px(8));
    if (!store_supports("settings_db_servers")) {
        doc_space(doc, px(10));
        doc_text(doc, x, col, "The database pool needs an Admin token on a server that offers it (GET /settings/db-servers).", FONT_FOOTNOTE, theme.muted, DT_LEFT | DT_WORDBREAK);
        doc_space(doc, px(12));
        return;
    }
    layout_tabs(s, doc, x, col);
    if (s->error) { doc_notice_box(doc, x, col, s->error); doc_space(doc, px(16)); }
    switch (g_tab) {
    case T_SERVER:
        if (row_has(s, ENABLED_KEY)) {
            doc_item(doc, doc_custom(doc, x, col, px(26), paint_check, s, NULL, ACT_TOGGLE, 0))->hand = true;
            doc_space(doc, px(10));
        }
        field(s, doc, x, col, F_LABEL);
        field_pair(s, doc, x, col, F_HOST, F_PORT, px(140));
        note(doc, x, col, "Host and port are unique in the pool. A label left empty is host:port.");
        doc_space(doc, px(8));
        field_pair(s, doc, x, col, F_USERNAME, F_PASSWORD, 0);
        note(doc, x, col, "The user sessions connect as. It needs the rights to create the databases its projects name.");
        layout_test(s, doc, x, col);
        break;
    case T_POOL: layout_pool(s, doc, x, col); break;
    }
    doc_space(doc, px(40));
}

static void form_header(Screen *base, HeaderInfo *info) {
    DbForm *s = (DbForm *)base;
    const char *label = json_str_nonempty(json_get(s->row, "label")), *host = json_str_nonempty(json_get(s->row, "host"));
    char *address = xstrfmt("%s:%d", host ? host : "", json_int_or(json_get(s->row, "port"), 0));
    if (s->id) {
        snprintf(info->title, sizeof info->title, "%s", label ? label : address);
        snprintf(info->subtitle, sizeof info->subtitle, "%s \xC2\xB7 a session claims this server for itself while it runs", address);
    } else {
        snprintf(info->title, sizeof info->title, "New database server");
        snprintf(info->subtitle, sizeof info->subtitle, "A server sessions claim one at a time, each for a database of its own.");
    }
    free(address);
    if (!store_supports("settings_db_servers")) return;
    bool busy = s->req_save || s->req_delete;
    HeaderButton *b = &info->buttons[info->button_count++];
    snprintf(b->label, sizeof b->label, "%s", s->req_save ? "Saving\xE2\x80\xA6" : "Save");
    b->glyph = 0xE74E; b->action = ACT_SAVE; b->prominent = true; b->tip = "Save this server (Ctrl+S)";
    b->enabled = !busy && (s->dirty || !s->id) && store_supports(s->id ? "update_db_server" : "create_db_server");
    if (!s->id) return;
    HeaderButton *c = &info->buttons[info->button_count++];
    c->glyph = 0xE8C8; c->action = ACT_CLONE; c->enabled = !busy && store_supports("create_db_server"); c->tip = "Clone into a new server";
    HeaderButton *d = &info->buttons[info->button_count++];
    d->glyph = 0xE74D; d->action = ACT_DELETE; d->destructive = true; d->enabled = !busy && store_supports("delete_db_server"); d->tip = "Remove this server from the pool";
}

// MARK: - The edits

static int margin_of(Pane *pane) { RECT rc; GetClientRect(pane_hwnd(pane), &rc); return (rc.right - rc.left - pane_content_width(pane)) / 2; }

static void form_place(Screen *base, const RECT *content, int scroll_y) {
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

static void form_save(DbForm *s);
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
        if (ctrl && wp == 'S') { form_save(s); return 0; }
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
static void form_ensure_controls(DbForm *s) {
    if (s->edits[F_LABEL]) return;
    HWND owner = pane_hwnd(s->base.pane);
    for (int f = 0; f < F_COUNT; f++) {
        DWORD style = WS_CHILD | WS_TABSTOP | ES_AUTOHSCROLL | (FIELDS[f].secret ? ES_PASSWORD : 0) | (FIELDS[f].number ? ES_NUMBER : 0);
        HWND e = CreateWindowExW(0, L"EDIT", L"", style, 0, 0, 10, 10, owner, (HMENU)(INT_PTR)(ID_FIELD + f), GetModuleHandleW(NULL), NULL);
        SendMessageW(e, WM_SETFONT, (WPARAM)font(FIELDS[f].mono ? FONT_MONO : FONT_BODY), TRUE);
        if (FIELDS[f].number) SendMessageW(e, EM_SETLIMITTEXT, 5, 0);
        { wchar_t *w = utf8_to_wide(FIELDS[f].cue); SendMessageW(e, EM_SETCUEBANNER, TRUE, (LPARAM)w); free(w); }
        SetWindowSubclass(e, field_proc, (UINT_PTR)f, (DWORD_PTR)s);
        theme_apply_control(e);
        s->edits[f] = e;
    }
    form_fill(s);
}

// MARK: - Saving, testing, cloning, deleting

static void form_set_id(DbForm *s, int id) {
    s->id = id;
    free(s->base.id); s->base.id = form_id(id);
    pane_set_selected_id(app_sidebar_pane(), s->base.id);
}
static void show_error(DbForm *s, const char *text) {
    set_string(&s->error, text);
    pane_relayout(s->base.pane); pane_header_changed(s->base.pane); pane_scroll_to_top(s->base.pane);
}
static void save_done(void *owner, Request *req) {
    DbForm *s = owner;
    const Json *row = req->ok ? json_get(req->result, "server") : NULL;
    if (!json_is_object(row)) {
        char *text = req->ok ? xstrdup("The server returned an unexpected response.") : request_error_text(req);
        show_error(s, text); free(text);
        return;
    }
    // The server's word on what was saved: an empty label became host:port, and a new server now has an id.
    json_free(s->row); s->row = json_clone(row);
    set_string(&s->error, NULL);
    form_set_id(s, row_id(row));
    form_fill(s);
    pane_relayout(s->base.pane); pane_header_changed(s->base.pane);
    settings_db_servers_changed(false);
}
static void form_save(DbForm *s) {
    if (s->req_save || s->req_delete || !store_supports(s->id ? "update_db_server" : "create_db_server")) return;
    char *why = NULL;
    Json *body = form_body(s, &why);
    // Only the port can be refused here, and it is on the Server tab.
    if (!body) { g_tab = T_SERVER; show_error(s, why); free(why); return; }
    if (s->id) json_set_num(body, "id", s->id);
    store_call(s->id ? "update_db_server" : "create_db_server", body, 0, s, save_done, 0, &s->req_save);
    pane_header_changed(s->base.pane);
}

static void test_done(void *owner, Request *req) {
    DbForm *s = owner;
    free(s->test_text);
    s->test_ok = req->ok;
    if (!req->ok) s->test_text = request_error_text(req);
    else {
        const Json *r = req->result, *claimed = json_get(r, "claimedBy");
        int dbs = json_int_or(json_get(r, "databases"), 0);
        const char *version = json_str(json_get(r, "version"));
        // Who holds it, when a session does: its title, else its id.
        const char *by = json_str_nonempty(claimed);
        if (!by && json_is_object(claimed)) by = json_str_nonempty(json_get(claimed, "title")) ? json_str(json_get(claimed, "title")) : json_str_nonempty(json_get(claimed, "id"));
        char *holder = by ? xstrfmt("claimed by session %s", by) : json_is_object(claimed) ? xstrdup("claimed by a session") : xstrdup("free");
        s->test_text = xstrfmt("Healthy: MySQL %s, %d database%s \xC2\xB7 %s", version ? version : "?", dbs, dbs == 1 ? "" : "s", holder);
        free(holder);
    }
    pane_relayout(s->base.pane);
}
/// Does this host answer with these credentials, and is a session on it right now? Probed on the form's values, so a
/// new server can be checked before it is saved.
static void form_test(DbForm *s) {
    if (s->req_test || !store_supports("test_db_server")) return;
    char *why = NULL;
    Json *body = form_body(s, &why);
    if (!body) { free(s->test_text); s->test_text = why; s->test_ok = false; pane_relayout(s->base.pane); return; }
    Json *args = json_object();
    static const char *const keys[] = { "host", "port", "username", "password" };
    for (size_t i = 0; i < sizeof keys / sizeof *keys; i++) { const Json *v = json_get(body, keys[i]); if (v) json_object_set(args, keys[i], json_clone(v)); }
    if (s->id) json_set_num(args, "id", s->id);
    json_free(body);
    free(s->test_text); s->test_text = NULL;
    store_call("test_db_server", args, 0, s, test_done, 0, &s->req_test);
    pane_relayout(s->base.pane);
}

static void delete_done(void *owner, Request *req) {
    DbForm *s = owner;
    if (!req->ok) { char *text = request_error_text(req); show_error(s, text); free(text); return; }
    s->dirty = false;
    app_clear_detail();
    settings_db_servers_changed(true);
}
static void form_delete(DbForm *s) {
    if (!s->id || s->req_save || s->req_delete) return;
    const char *name = json_str_nonempty(json_get(s->row, "label"));
    char *title = xstrfmt("Remove %s from the pool?", name ? name : "this server");
    bool ok = app_confirm(title, "Sessions can no longer claim it. Nothing on the server itself is touched.", "Remove", true);
    free(title);
    if (!ok) return;
    Json *args = json_object(); json_set_num(args, "id", s->id);
    store_call("delete_db_server", args, 0, s, delete_done, 0, &s->req_delete);
    pane_header_changed(s->base.pane);
}
static void form_clone(DbForm *s) {
    char *why = NULL;
    Json *copy = form_body(s, &why);
    if (!copy) { g_tab = T_SERVER; show_error(s, why); free(why); return; }
    // host:port is unique in the pool, so the copy starts without a port; an empty label becomes host:port on save.
    json_set_str(copy, "label", "");
    json_object_set(copy, "port", json_null());
    s->dirty = false;
    Screen *clone = db_server_settings_screen_new(copy, NULL);
    ((DbForm *)clone)->focus_first = F_PORT;
    g_tab = T_SERVER;
    json_free(copy);
    app_show_detail(clone);
}

// MARK: - The screen

static void form_destroy(Screen *base) {
    DbForm *s = (DbForm *)base;
    request_cancel(&s->req_save); request_cancel(&s->req_delete); request_cancel(&s->req_test);
    for (int f = 0; f < F_COUNT; f++) if (s->edits[f]) DestroyWindow(s->edits[f]);
    json_free(s->row); free(s->error); free(s->test_text);
    screen_release(base);
}
static void form_action(Screen *base, int action, intptr_t arg, POINT pt) {
    (void)pt;
    DbForm *s = (DbForm *)base;
    switch (action) {
    case ACT_SAVE: form_save(s); break;
    case ACT_CLONE: form_clone(s); break;
    case ACT_DELETE: form_delete(s); break;
    case ACT_TEST: form_test(s); break;
    case ACT_TAB:
        if (arg < 0 || arg >= T_COUNT) break;
        if (s->focused >= 0) SetFocus(pane_hwnd(base->pane));
        g_tab = (int)arg;
        pane_relayout(base->pane); pane_scroll_to_top(base->pane);
        break;
    case ACT_TOGGLE: s->enabled = !s->enabled; form_changed(s); pane_relayout(base->pane); break;
    case ACT_FOCUS: if (arg >= 0 && arg < F_COUNT && s->edits[arg]) SetFocus(s->edits[arg]); break;
    }
}
static void form_command(Screen *base, int id, int code, HWND control) {
    (void)control;
    DbForm *s = (DbForm *)base;
    int f = id - ID_FIELD;
    if (f < 0 || f >= F_COUNT || !s->edits[f]) return;
    switch (code) {
    case EN_CHANGE: form_changed(s); break;
    case EN_SETFOCUS: s->focused = f; pane_repaint(base->pane); break;
    case EN_KILLFOCUS: if (s->focused == f) s->focused = -1; pane_repaint(base->pane); break;
    }
}
static bool form_key(Screen *base, WPARAM vk, bool ctrl, bool shift) {
    (void)shift;
    if (ctrl && vk == 'S') { form_save((DbForm *)base); return true; }
    return false;
}
static void form_visible(Screen *base, bool shown) {
    DbForm *s = (DbForm *)base;
    s->shown = shown;
    if (shown) {
        form_ensure_controls(s);
        if (s->focus_first >= 0) { int f = s->focus_first; s->focus_first = -1; SetFocus(s->edits[f]); }
    } else for (int f = 0; f < F_COUNT; f++) if (s->edits[f]) ShowWindow(s->edits[f], SW_HIDE);
}
static bool form_can_leave(Screen *base) {
    DbForm *s = (DbForm *)base;
    if (!s->dirty) return true;
    const char *name = json_str_nonempty(json_get(s->row, "label"));
    char *message = s->id ? xstrfmt("The changes to %s have not been saved.", name ? name : "this server") : xstrdup("The new database server has not been saved.");
    bool leave = app_confirm("Discard unsaved changes?", message, "Discard", true);
    free(message);
    if (leave) s->dirty = false;
    return leave;
}

static const ScreenVTable form_vt = {
    .destroy = form_destroy, .layout = form_layout, .header = form_header, .action = form_action, .place = form_place,
    .visible = form_visible, .command = form_command, .key = form_key, .can_leave = form_can_leave,
};
Screen *db_server_settings_screen_new(const Json *row, const Json *defaults) {
    DbForm *s = xcalloc(1, sizeof *s);
    s->base.vt = &form_vt;
    s->focused = -1; s->focus_first = -1;
    s->row = json_is_object(row) ? json_clone(row) : json_is_object(defaults) ? json_clone(defaults) : json_object();
    s->id = row_id(s->row);
    s->base.id = form_id(s->id);
    // A new server starts where its host is typed.
    if (!s->id) { s->focus_first = F_HOST; g_tab = T_SERVER; }
    form_fill(s);
    return &s->base;
}
