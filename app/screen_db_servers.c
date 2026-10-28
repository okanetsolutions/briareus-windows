#include "screen_db_servers_internal.h"

static int row_id(const Json *row);
static char *form_id(int id);
static void form_set_id(DbForm *s, int id);
static void show_error(DbForm *s, const char *text);
static void save_done(void *owner, Request *req);
static void test_done(void *owner, Request *req);
static void form_test(DbForm *s);
static void delete_done(void *owner, Request *req);
static void form_delete(DbForm *s);
static void form_clone(DbForm *s);
static void form_destroy(Screen *base);
static void form_action(Screen *base, int action, intptr_t arg, POINT pt);
static void form_command(Screen *base, int id, int code, HWND control);
static bool form_key(Screen *base, WPARAM vk, bool ctrl, bool shift);
static void form_visible(Screen *base, bool shown);
static bool form_can_leave(Screen *base);

// One server of the database pool, as the dashboard's database server form, its fields on tabs along the top as the
// project form lays out its own: Server (where it is, how sessions sign in to it, and Test connection) and Pool. Saved
// through /settings/db-servers, which needs an Admin token as the project routes do.

static int row_id(const Json *row) { return json_int_or(json_get(row, "id"), 0); }

static char *form_id(int id) { return id > 0 ? xstrfmt("settings-db:%d", id) : xstrdup("settings-db:new"); }

const FieldDef screen_db_servers_FIELDS[F_COUNT] = {
    [F_LABEL] = { "label", "Label", "defaults to host:port", false, false, false, "A label left empty is host:port." },
    [F_HOST] = { "host", "Host", "127.0.0.1", false, false, true, "Host and port are unique in the pool." },
    [F_PORT] = { "port", "Port", "3306", true, false, true },
    [F_USERNAME] = { "username", "Username", "root", false, false, true,
        "The user sessions connect as. It needs the rights to create the databases its projects name." },
    [F_PASSWORD] = { "password", "Password", "empty = no password", false, true, true },
};

const char *const screen_db_servers_ENABLED_KEY = "enabled";

/// The open tab stays open from one server to the next, as the project form's does.
int screen_db_servers_g_tab;

bool screen_db_servers_row_has(DbForm *s, const char *key) {
    if (!json_count(s->row)) return true;
    for (size_t i = 0; i < json_count(s->row); i++) if (str_eq(json_key(s->row, i), key)) return true;
    return false;
}

// MARK: - Saving, testing, cloning, deleting

static void form_set_id(DbForm *s, int id) {
    s->id = id;
    free(s->base.id); s->base.id = form_id(id);
    settings_select_id(s->base.id);
}

static void show_error(DbForm *s, const char *text) {
    set_string(&s->error, text);
    pane_relayout(s->base.pane); pane_header_changed(s->base.pane); pane_scroll_to_top(s->base.pane);
}

static void save_done(void *owner, Request *req) {
    DbForm *s = owner;
    const Json *row = req->ok ? json_get(req->result, "server") : NULL;
    if (!json_is_object(row)) {
        char *text = request_error_or_unexpected(req);
        show_error(s, text); free(text);
        return;
    }
    // The server's word on what was saved: an empty label became host:port, and a new server now has an id.
    json_free(s->row); s->row = json_clone(row);
    set_string(&s->error, NULL);
    form_set_id(s, row_id(row));
    screen_db_servers_form_fill(s);
    pane_relayout(s->base.pane); pane_header_changed(s->base.pane);
    settings_db_servers_changed(false);
}

void screen_db_servers_form_save(DbForm *s) {
    if (s->req_save || s->req_delete || !store_supports(s->id ? "update_db_server" : "create_db_server")) return;
    char *why = NULL;
    Json *body = screen_db_servers_form_body(s, &why);
    // Only the port can be refused here, and it is on the Server tab.
    if (!body) { screen_db_servers_g_tab = T_SERVER; show_error(s, why); free(why); return; }
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
    Json *body = screen_db_servers_form_body(s, &why);
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
    Json *copy = screen_db_servers_form_body(s, &why);
    if (!copy) { screen_db_servers_g_tab = T_SERVER; show_error(s, why); free(why); return; }
    // host:port is unique in the pool, so the copy starts without a port; an empty label becomes host:port on save.
    json_set_str(copy, "label", "");
    json_object_set(copy, "port", json_null());
    s->dirty = false;
    Screen *clone = db_server_settings_screen_new(copy, NULL);
    ((DbForm *)clone)->focus_first = F_PORT;
    screen_db_servers_g_tab = T_SERVER;
    json_free(copy);
    app_show_detail(clone);
}

// MARK: - The screen

static void form_release(DbForm *s) {
    for (int f = 0; f < F_COUNT; f++) if (s->edits[f]) DestroyWindow(s->edits[f]);
    json_free(s->row); free(s->error); free(s->test_text);
    screen_release(&s->base);
}
static void form_destroy(Screen *base) {
    DbForm *s = (DbForm *)base;
    request_cancel(&s->req_save); request_cancel(&s->req_delete); request_cancel(&s->req_test);
    if (s->modal) { s->retired = true; return; }
    form_release(s);
}

static void form_action(Screen *base, int action, intptr_t arg, POINT pt) {
    (void)pt;
    DbForm *s = (DbForm *)base;
    switch (action) {
    case ACT_SAVE: screen_db_servers_form_save(s); break;
    case ACT_CLONE: form_clone(s); break;
    case ACT_DELETE: form_delete(s); break;
    case ACT_TEST: form_test(s); break;
    case ACT_TAB:
        if (arg < 0 || arg >= T_COUNT) break;
        if (s->focused >= 0) SetFocus(pane_hwnd(base->pane));
        screen_db_servers_g_tab = (int)arg;
        pane_relayout(base->pane); pane_scroll_to_top(base->pane);
        break;
    case ACT_TOGGLE: s->enabled = !s->enabled; screen_db_servers_form_changed(s); pane_relayout(base->pane); break;
    case ACT_FOCUS: if (arg >= 0 && arg < F_COUNT && s->edits[arg]) SetFocus(s->edits[arg]); break;
    }
}

static void form_command(Screen *base, int id, int code, HWND control) {
    (void)control;
    DbForm *s = (DbForm *)base;
    int f = id - ID_FIELD;
    if (f < 0 || f >= F_COUNT || !s->edits[f]) return;
    switch (code) {
    case EN_CHANGE: screen_db_servers_form_changed(s); break;
    case EN_SETFOCUS: s->focused = f; pane_repaint(base->pane); break;
    case EN_KILLFOCUS: if (s->focused == f) s->focused = -1; pane_repaint(base->pane); break;
    }
}

static bool form_key(Screen *base, WPARAM vk, bool ctrl, bool shift) {
    (void)shift;
    if (ctrl && vk == 'S') { screen_db_servers_form_save((DbForm *)base); return true; }
    return false;
}

static void form_visible(Screen *base, bool shown) {
    DbForm *s = (DbForm *)base;
    s->shown = shown;
    if (shown) {
        screen_db_servers_form_ensure_controls(s);
        if (s->focus_first >= 0) { int f = s->focus_first; s->focus_first = -1; SetFocus(s->edits[f]); }
    } else for (int f = 0; f < F_COUNT; f++) if (s->edits[f]) ShowWindow(s->edits[f], SW_HIDE);
}

static bool form_can_leave(Screen *base) {
    DbForm *s = (DbForm *)base;
    if (s->modal) return false;
    if (!s->dirty) return true;
    const char *name = json_str_nonempty(json_get(s->row, "label"));
    char *message = s->id ? xstrfmt("The changes to %s have not been saved.", name ? name : "this server") : xstrdup("The new database server has not been saved.");
    s->modal = true;
    bool leave = app_confirm("Discard unsaved changes?", message, "Discard", true);
    free(message);
    s->modal = false;
    if (s->retired) { form_release(s); return true; }
    if (leave) s->dirty = false;
    return leave;
}

static const ScreenVTable form_vt = {
    .destroy = form_destroy, .layout = screen_db_servers_form_layout, .header = screen_db_servers_form_header, .action = form_action, .place = screen_db_servers_form_place,
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
    if (!s->id) { s->focus_first = F_HOST; screen_db_servers_g_tab = T_SERVER; }
    screen_db_servers_form_fill(s);
    return &s->base;
}
