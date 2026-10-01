// Settings, as the dashboard's settings page: a sidebar of its own (Back to sessions, Devices and clients, the projects
// and the database pool, each with ＋ New) and a project's form, its sections and fields laid out as the dashboard's,
// saved through /settings/projects. Those routes need an Admin token; any other token gets a sentence saying so.
// A database server's form is screen_db_servers.c.
#include "screens.h"
#include "str.h"
#include <commctrl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void sign_out(void) {
    if (!app_confirm("Sign out of this dashboard?", "The device token and the saved conversations are removed from this computer. The token itself is revoked from Connection.", "Sign out", true)) return;
    store_forget();
}
static bool in_rect(const RECT *r, POINT pt) { return pt.x >= r->left && pt.x < r->right && pt.y >= r->top && pt.y < r->bottom; }
static int row_id(const Json *row) { return json_int_or(json_get(row, "id"), 0); }
static char *form_id(int id) { return id > 0 ? xstrfmt("settings-project:%d", id) : xstrdup("settings-project:new"); }
static bool is_form_id(const char *id) { return id && (str_has_prefix(id, "settings-project:") || str_has_prefix(id, "settings-db:")); }

/// Why the settings cannot be shown here, as a new string; NULL when they can.
static char *settings_unavailable(void) {
    if (store_supports("settings_projects")) return NULL;
    bool listed = false;
    for (size_t i = 0; i < g_store.route_count; i++) if (str_has_suffix(g_store.routes[i].path, "settings/projects")) listed = true;
    const char *permission = g_store.has_device && g_store.device.permission ? g_store.device.permission : "unknown";
    return listed
        ? xstrfmt("Project settings need an Admin token, and this device's token is %s. Create an Admin token on the web dashboard under Settings \xE2\x86\x92 Devices and clients and connect with it.", permission)
        : xstrdup("This server does not offer project settings (GET /settings/projects) on its client API. Update the server to manage projects here.");
}

// MARK: - The sidebar

enum { ACT_BACK = 1000, ACT_DEVICES, ACT_NEW_PROJECT, ACT_OPEN_PROJECT, ACT_NEW_SERVER, ACT_OPEN_SERVER };
enum { MENU_UP = 1, MENU_DOWN };

typedef struct {
    Screen base;
    Json *projects;     // the server's Project rows, in their order
    Json *defaults;     // what a new one starts from
    Json *servers;      // the database pool: `list`, the server's DbServer rows, and `defaults`
    bool loaded, servers_loaded;
    bool open_first_server;   // a server was removed: the next read of the pool opens the first one left
    char *error, *servers_error;
    Request *req, *req_order, *req_servers;
    RECT signout_rc;
} SettingsScreen;

/// The settings sidebar on screen, for a form to tell when the list changed.
static SettingsScreen *g_settings;

typedef struct { const char *text; bool selected; } NavData;
static void paint_back(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    bool hovered = doc->hover >= 0 && doc_item(doc, doc->hover) == it;
    RECT t = { rc->left + px(6), rc->top, rc->right, rc->bottom };
    draw_text(cv, "\xE2\x86\x90 Back to sessions", &t, FONT_CAPTION, hovered ? theme.ink : theme.muted, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
}
static void paint_nav(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    NavData *d = it->data;
    bool hovered = doc->hover >= 0 && doc_item(doc, doc->hover) == it;
    if (hovered || d->selected) fill_round_rect(cv, rc, px(6), theme.raise, theme.raise);
    RECT t = { rc->left + px(8), rc->top, rc->right - px(8), rc->bottom };
    draw_text(cv, d->text, &t, FONT_SUBHEADLINE, theme.ink, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

typedef struct { char *label, *repo; bool enabled, db, selected; } ProjectRowData;
static void project_row_free(void *p) { ProjectRowData *d = p; free(d->label); free(d->repo); free(d); }
static void paint_project_row(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    ProjectRowData *d = it->data;
    bool hovered = doc->hover >= 0 && doc_item(doc, doc->hover) == it;
    if (hovered || d->selected) fill_round_rect(cv, rc, px(6), theme.raise, theme.raise);
    int x = rc->left + px(8), top = rc->top + px(6), lh = px(22);
    // `.dot.idle` for a project sessions can start on, the plain grey dot for one switched off.
    draw_status_dot(cv, x + px(3), top + lh / 2, d->enabled ? "idle" : "");
    RECT t = { x + px(7) + px(7), top, rc->right - px(8), top + lh };
    draw_text(cv, d->label, &t, FONT_SUBHEADLINE, theme.ink, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    int y2 = top + lh, right = rc->right - px(8);
    if (d->db) {
        // `rounded border border-line px-1 text-[11px]`: the database pool's tag.
        int h; int bw = text_width(cv, "db", FONT_CAPTION2) + px(12) + 2;
        int rw = text_width(cv, d->repo, FONT_CAPTION);
        int bx = x + (rw < right - x - bw - px(8) ? rw : right - x - bw - px(8)) + px(8);
        draw_chip(cv, bx, y2 + (px(18) - h) / 2, "db", theme.muted, hovered || d->selected ? theme.raise : theme.sidebar, &h);
        right = bx - px(8);
    }
    RECT r = { x, y2, right, y2 + px(18) };
    draw_text(cv, d->repo, &r, FONT_CAPTION, theme.muted, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

static void settings_open_row(SettingsScreen *s, size_t index) {
    const Json *row = json_at(json_get(s->projects, "list"), index);
    if (json_is_object(row)) app_show_detail(project_settings_screen_new(row, json_get(s->projects, "defaults")));
}
static const Json *settings_rows(SettingsScreen *s) { return json_get(s->projects, "list"); }

static void settings_load(SettingsScreen *s);
static void settings_done(void *owner, Request *req) {
    SettingsScreen *s = owner;
    s->loaded = true;
    if (!req->ok) { char *t = request_error_text(req); set_string(&s->error, t); free(t); pane_relayout(s->base.pane); return; }
    set_string(&s->error, NULL);
    json_free(s->projects);
    s->projects = json_object();
    json_object_set(s->projects, "list", json_clone(json_get(req->result, "projects")));
    json_object_set(s->projects, "defaults", json_clone(json_get(req->result, "defaults")));
    pane_relayout(s->base.pane);
    // The dashboard's settings page opens on its first project, or on a new one when there is none; so does this,
    // unless a settings form or Devices and clients is already up.
    Screen *root = pane_root(app_detail_pane());
    if (root && (is_form_id(root->id) || str_eq(root->id, "connection"))) return;
    if (json_count(settings_rows(s))) settings_open_row(s, 0);
    else app_show_detail(project_settings_screen_new(NULL, json_get(s->projects, "defaults")));
}
static void settings_load(SettingsScreen *s) {
    if (s->req || !store_supports("settings_projects")) { s->loaded = true; return; }
    store_call("settings_projects", json_object(), 0, s, settings_done, 0, &s->req);
}
static const Json *server_rows(SettingsScreen *s) { return json_get(s->servers, "list"); }
static void servers_open_row(SettingsScreen *s, size_t index) {
    const Json *row = json_at(server_rows(s), index);
    if (json_is_object(row)) app_show_detail(db_server_settings_screen_new(row, json_get(s->servers, "defaults")));
}
static void servers_done(void *owner, Request *req) {
    SettingsScreen *s = owner;
    s->servers_loaded = true;
    if (!req->ok) { char *t = request_error_text(req); set_string(&s->servers_error, t); free(t); pane_relayout(s->base.pane); return; }
    set_string(&s->servers_error, NULL);
    json_free(s->servers);
    s->servers = json_object();
    json_object_set(s->servers, "list", json_clone(json_get(req->result, "servers")));
    json_object_set(s->servers, "defaults", json_clone(json_get(req->result, "defaults")));
    pane_relayout(s->base.pane);
    // After a server was removed, the first one left takes its place, as a project's deletion opens the first project.
    if (!s->open_first_server) return;
    s->open_first_server = false;
    Screen *root = pane_root(app_detail_pane());
    if (root && (is_form_id(root->id) || str_eq(root->id, "connection"))) return;
    if (json_count(server_rows(s))) servers_open_row(s, 0);
    else app_show_detail(db_server_settings_screen_new(NULL, json_get(s->servers, "defaults")));
}
static void servers_load(SettingsScreen *s) {
    if (s->req_servers || !store_supports("settings_db_servers")) { s->servers_loaded = true; return; }
    store_call("settings_db_servers", json_object(), 0, s, servers_done, 0, &s->req_servers);
}
void settings_db_servers_changed(bool open_first) {
    if (!g_settings) return;
    request_cancel(&g_settings->req_servers);
    g_settings->open_first_server = open_first;
    servers_load(g_settings);
}
const Json *settings_project_rows(void) { return g_settings ? settings_rows(g_settings) : NULL; }
const Json *settings_db_server_rows(void) { return g_settings ? server_rows(g_settings) : NULL; }
size_t settings_pool_capacity(void) {
    const Json *rows = settings_db_server_rows();
    size_t n = 0;
    for (size_t i = 0; i < json_count(rows); i++) if (json_bool_is(json_get(json_at(rows, i), "enabled"), true)) n++;
    return n;
}

void settings_projects_changed(int select_id) {
    (void)select_id;   // the form's own id is what the sidebar highlights
    if (!g_settings) return;
    request_cancel(&g_settings->req);
    settings_load(g_settings);
}

static void order_done(void *owner, Request *req) {
    SettingsScreen *s = owner;
    if (!req->ok) { char *t = request_error_text(req); set_string(&s->error, t); free(t); }
    else if (json_is_array(json_get(req->result, "projects"))) {
        json_object_set(s->projects, "list", json_clone(json_get(req->result, "projects")));
        set_string(&s->error, NULL);
    }
    pane_relayout(s->base.pane);
}
/// Moves a project up or down the list, which is also the order the dashboard's sidebar and composer use.
static void settings_move(SettingsScreen *s, size_t index, int delta) {
    const Json *rows = settings_rows(s);
    size_t n = json_count(rows);
    if (s->req_order || index >= n || (delta < 0 && index == 0) || (delta > 0 && index + 1 >= n)) return;
    size_t other = delta < 0 ? index - 1 : index + 1;
    Json *ids = json_array();
    for (size_t i = 0; i < n; i++) {
        size_t from = i == index ? other : i == other ? index : i;
        json_array_push(ids, json_number(row_id(json_at(rows, from))));
    }
    Json *args = json_object(); json_object_set(args, "ids", ids);
    store_call("order_projects", args, 0, s, order_done, 0, &s->req_order);
}

static void settings_destroy(Screen *base) {
    SettingsScreen *s = (SettingsScreen *)base;
    if (g_settings == s) g_settings = NULL;
    request_cancel(&s->req); request_cancel(&s->req_order); request_cancel(&s->req_servers);
    json_free(s->projects); json_free(s->servers); free(s->error); free(s->servers_error);
    screen_release(base);
}
/// A section's summary, as the dashboard's `.side-sec`: its title, a muted note after it, and ＋ New when `action` is set.
static void section_head(Doc *doc, int w, const char *title, const char *note, int action) {
    int y = doc->y, h = px(20), nw = action ? text_width(doc->cv, "\xEF\xBC\x8B New", FONT_CAPTION) + px(8) : 0;
    RECT tr = { px(8), y, w - px(8) - nw, y + h };
    doc_text_at(doc, &tr, title, FONT_CAPTION_SEMIBOLD, theme.muted, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    int nx = px(8) + text_width(doc->cv, title, FONT_CAPTION_SEMIBOLD) + px(6);
    if (note && nx < tr.right) {
        RECT nr = { nx, y, tr.right, y + h };
        doc_text_at(doc, &nr, note, FONT_CAPTION2, theme.muted, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
    if (action) {
        RECT nr = { w - px(4) - nw, y, w - px(4), y + h };
        Item *it = doc_item(doc, doc_text_at(doc, &nr, "\xEF\xBC\x8B New", FONT_CAPTION, theme.muted, DT_CENTER | DT_VCENTER | DT_SINGLELINE));
        it->action = action; it->hand = true;
    }
    doc->y = y + h;
    doc_space(doc, px(4));
}
/// The database pool under the projects: each server with its dot and host:port, and ＋ New.
static void layout_servers(SettingsScreen *s, Doc *doc, int w, const char *selected) {
    if (!store_supports("settings_db_servers")) return;
    doc_space(doc, px(16));
    // One open session with a database per server in the pool, so the pool's size heads the section, as the sessions
    // it lets run at once (the dashboard's "· 2 parallel sessions with a database", cut to the sidebar's width).
    size_t n = settings_pool_capacity();
    char *note = n ? xstrfmt("\xC2\xB7 %zu session%s", n, n == 1 ? "" : "s") : NULL;
    section_head(doc, w, "Database pool", note, store_supports("create_db_server") ? ACT_NEW_SERVER : 0);
    free(note);
    if (s->servers_error) { doc_notice(doc, px(8), w - px(16), s->servers_error); doc_space(doc, px(8)); }
    const Json *rows = server_rows(s);
    for (size_t i = 0; i < json_count(rows); i++) {
        const Json *row = json_at(rows, i);
        ProjectRowData *d = xcalloc(1, sizeof *d);
        const char *host = json_str(json_get(row, "host"));
        d->repo = xstrfmt("%s:%d", host ? host : "", json_int_or(json_get(row, "port"), 0));
        d->label = xstrdup(json_str_nonempty(json_get(row, "label")) ? json_str(json_get(row, "label")) : d->repo);
        d->enabled = json_bool_is(json_get(row, "enabled"), true);
        char *id = xstrfmt("settings-db:%d", row_id(row));
        d->selected = str_eq(selected, id);
        free(id);
        doc_custom(doc, 0, w, px(6) + px(22) + px(18) + px(6), paint_project_row, d, project_row_free, ACT_OPEN_SERVER, (intptr_t)i);
    }
    if (str_eq(selected, "settings-db:new")) {
        ProjectRowData *d = xcalloc(1, sizeof *d);
        d->label = xstrdup("New database server"); d->repo = xstrdup("not saved yet"); d->selected = true;
        doc_custom(doc, 0, w, px(6) + px(22) + px(18) + px(6), paint_project_row, d, project_row_free, 0, 0);
    }
    if (s->servers_loaded && !json_count(rows) && !s->servers_error) doc_text(doc, px(8), w - px(16), "No servers yet. Add one so sessions can claim a database of their own.", FONT_FOOTNOTE, theme.muted, DT_WORDBREAK);
    if (!s->servers_loaded) doc_loading(doc, 0, w, "Loading the database pool\xE2\x80\xA6");
}

static void settings_layout(Screen *base, Doc *doc) {
    SettingsScreen *s = (SettingsScreen *)base;
    int w = doc->width;
    const char *selected = pane_selected_id(base->pane);
    doc_space(doc, px(8));
    doc_custom(doc, 0, w, px(24), paint_back, NULL, NULL, ACT_BACK, 0);
    doc_space(doc, px(8));
    NavData *nav = xcalloc(1, sizeof *nav); nav->text = "Devices and clients"; nav->selected = str_eq(selected, "connection");
    doc_custom(doc, 0, w, px(36), paint_nav, nav, free, ACT_DEVICES, 0);
    doc_space(doc, px(16));
    char *why = settings_unavailable();
    section_head(doc, w, "Projects", NULL, why ? 0 : ACT_NEW_PROJECT);
    if (why) { doc_text(doc, px(8), w - px(16), why, FONT_FOOTNOTE, theme.muted, DT_LEFT | DT_WORDBREAK); free(why); doc_space(doc, px(8)); return; }
    if (s->error) { doc_notice(doc, px(8), w - px(16), s->error); doc_space(doc, px(8)); }
    const Json *rows = settings_rows(s);
    for (size_t i = 0; i < json_count(rows); i++) {
        const Json *row = json_at(rows, i);
        ProjectRowData *d = xcalloc(1, sizeof *d);
        const char *repo = json_str(json_get(row, "repo"));
        d->repo = xstrdup(repo ? repo : "");
        d->label = xstrdup(json_str_nonempty(json_get(row, "label")) ? json_str(json_get(row, "label")) : d->repo);
        d->enabled = !json_bool_is(json_get(row, "enabled"), false);
        d->db = json_bool_is(json_get(row, "dbPoolEnabled"), true);
        char *id = form_id(row_id(row));
        d->selected = str_eq(selected, id);
        free(id);
        doc_custom(doc, 0, w, px(6) + px(22) + px(18) + px(6), paint_project_row, d, project_row_free, ACT_OPEN_PROJECT, (intptr_t)i);
    }
    // A project being added shows as its own row until it is saved.
    if (str_eq(selected, "settings-project:new")) {
        ProjectRowData *d = xcalloc(1, sizeof *d);
        d->label = xstrdup("New project"); d->repo = xstrdup("not saved yet"); d->selected = true;
        doc_custom(doc, 0, w, px(6) + px(22) + px(18) + px(6), paint_project_row, d, project_row_free, 0, 0);
    }
    if (s->loaded && !json_count(rows) && !s->error) doc_text(doc, px(8), w - px(16), "No projects yet. \xEF\xBC\x8B New adds a repository sessions can be started against.", FONT_FOOTNOTE, theme.muted, DT_WORDBREAK);
    if (!s->loaded) doc_loading(doc, 0, w, "Loading projects\xE2\x80\xA6");
    layout_servers(s, doc, w, selected);
    doc_space(doc, px(8));
}
static void settings_header(Screen *base, HeaderInfo *info) { (void)base; (void)info; }

/// The foot, as the dashboard's settings page has it: `Settings` and `⎋ Sign out`, 12px muted, above a border.
static int settings_footer_height(Screen *base, int width) { (void)base; (void)width; return px(6) + 1 + px(10) + px(18) + px(2) + px(10); }
static void settings_footer_paint(Screen *base, Canvas *cv, const RECT *rc) {
    SettingsScreen *s = (SettingsScreen *)base;
    fill_rect(cv, rc, theme.sidebar);
    int top = rc->top + px(6);
    draw_line(cv, rc->left + px(10), top, rc->right - px(10), top, theme.line);
    int y = top + 1 + px(10), h = px(18), left = rc->left + px(16), right = rc->right - px(16);
    const char *out = "\xE2\x8E\x8B Sign out";
    int ow = text_width(cv, out, FONT_CAPTION);
    RECT a = { left, y, right - ow - px(8), y + h }, c = { right - ow, y, right, y + h };
    draw_text(cv, "Settings", &a, FONT_CAPTION, theme.muted, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    draw_text(cv, out, &c, FONT_CAPTION, theme.muted, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    InflateRect(&c, px(4), px(4));
    s->signout_rc = c;
}
static void settings_footer_click(Screen *base, POINT pt) { SettingsScreen *s = (SettingsScreen *)base; if (in_rect(&s->signout_rc, pt)) sign_out(); }

static void settings_back(SettingsScreen *s) {
    // The settings forms go with the sidebar that opened them, unless one keeps its unsaved changes.
    Screen *root = pane_root(app_detail_pane());
    if (root && (is_form_id(root->id) || str_eq(root->id, "connection"))) {
        app_clear_detail();
        if (pane_root(app_detail_pane()) == root) return;
    }
    pane_pop(s->base.pane);
}
static void settings_action(Screen *base, int action, intptr_t arg, POINT pt) {
    (void)pt;
    SettingsScreen *s = (SettingsScreen *)base;
    switch (action) {
    case ACT_BACK: settings_back(s); break;
    case ACT_DEVICES: app_show_detail(connection_screen_new()); break;
    case ACT_NEW_PROJECT: app_show_detail(project_settings_screen_new(NULL, json_get(s->projects, "defaults"))); break;
    case ACT_OPEN_PROJECT: settings_open_row(s, (size_t)arg); break;
    case ACT_NEW_SERVER: app_show_detail(db_server_settings_screen_new(NULL, json_get(s->servers, "defaults"))); break;
    case ACT_OPEN_SERVER: servers_open_row(s, (size_t)arg); break;
    }
}
static void settings_context(Screen *base, int action, intptr_t arg, POINT pt) {
    SettingsScreen *s = (SettingsScreen *)base;
    if (action != ACT_OPEN_PROJECT || !store_supports("order_projects")) return;
    size_t n = json_count(settings_rows(s)), i = (size_t)arg;
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING | (i > 0 && !s->req_order ? 0 : MF_GRAYED), MENU_UP, L"Move up");
    AppendMenuW(menu, MF_STRING | (i + 1 < n && !s->req_order ? 0 : MF_GRAYED), MENU_DOWN, L"Move down");
    int chosen = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, pt.x, pt.y, 0, pane_hwnd(base->pane), NULL);
    DestroyMenu(menu);
    if (chosen == MENU_UP) settings_move(s, i, -1);
    else if (chosen == MENU_DOWN) settings_move(s, i, 1);
}
static void settings_visible(Screen *base, bool shown) {
    SettingsScreen *s = (SettingsScreen *)base;
    if (shown && !s->loaded && !s->req) settings_load(s);
    if (shown && !s->servers_loaded && !s->req_servers) servers_load(s);
}
static void settings_refresh(Screen *base) {
    SettingsScreen *s = (SettingsScreen *)base;
    request_cancel(&s->req); request_cancel(&s->req_servers);
    settings_load(s); servers_load(s);
}
static bool settings_key(Screen *base, WPARAM vk, bool ctrl, bool shift) {
    (void)ctrl; (void)shift;
    // Backspace and Escape leave Settings as ← Back to sessions does, so a form with changes is asked first.
    if (vk == VK_BACK || vk == VK_ESCAPE) { settings_back((SettingsScreen *)base); return true; }
    return false;
}

static const ScreenVTable settings_vt = {
    .destroy = settings_destroy, .layout = settings_layout, .header = settings_header, .action = settings_action,
    .context = settings_context, .visible = settings_visible, .refresh = settings_refresh, .key = settings_key,
    .footer_height = settings_footer_height, .footer_paint = settings_footer_paint, .footer_click = settings_footer_click,
};
Screen *settings_screen_new(void) {
    SettingsScreen *s = xcalloc(1, sizeof *s);
    s->base.vt = &settings_vt; s->base.id = xstrdup("settings");
    s->projects = json_object(); s->servers = json_object();
    g_settings = s;
    return &s->base;
}

// MARK: - The project form

typedef enum { K_TEXT, K_LIST, K_AREA, K_NUMBER, K_BOOL } FieldKind;
/// One of the form's fields: the Project key it edits, how, and the words around it. `rows` sizes a multi-line box.
typedef struct { const char *key; FieldKind kind; const char *label, *cue, *hint; int rows; bool mono; } FieldDef;

enum {
    F_REPO, F_LABEL, F_LOCAL_DIR,
    F_SETUP, F_PHP,
    F_DB_NAME, F_DB_EXT, F_DB_POOL, F_DB_RESTORE,
    F_REVIEW_AUTHOR, F_PUBLISH, F_TEST_SHEET, F_TEST_RUN, F_QA_NOTES, F_SHEET_STEPS, F_FEEDBACK_STEPS,
    F_BUDGET, F_IS_SELF,
    F_ENV,
    F_RUN, F_PROFILES,
    F_COUNT
};
static const FieldDef FIELDS[F_COUNT] = {
    [F_REPO] = { "repo", K_TEXT, "Repository", "owner/name", "Cloned over HTTPS with the machine's own git credentials.", 0, false },
    [F_LABEL] = { "label", K_TEXT, "Label", "shown in the project dropdown", NULL, 0, false },
    [F_LOCAL_DIR] = { "localDir", K_TEXT, "Local checkout", "/home/you/www/your-checkout",
        "This machine's own checkout of the repo. A session started in Local mode works directly in it: no clone, no setup steps, no pooled database, and the tree is used exactly as it stands. Leave empty to keep Local mode off for this project.", 0, true },
    [F_SETUP] = { "setupCommands", K_LIST, "Setup commands", NULL,
        "One shell command per line, run in the checkout in order before the session starts. The first failure aborts the session.", 6, true },
    [F_PHP] = { "phpBinDir", K_TEXT, "PHP bin directory", "/usr/bin (or ~/.phpenv/versions/8.4/bin)",
        "Prepended to PATH for everything this project runs. Leave empty for the machine's PHP.", 0, true },
    [F_DB_NAME] = { "dbPoolDatabase", K_TEXT, "Database", "my_app",
        "What DB_DATABASE is set to in the session's environment. With the pool on, created on the claimed server if it does not exist yet. With the pool off, every session gets a database of its own, this name plus the session id, created on the server the .env template points at, and written into the checkout's .env. Leave empty to use the .env template as-is. Migrations and seeding belong in the setup commands.", 0, true },
    [F_DB_EXT] = { "dbExtensions", K_LIST, "Postgres extensions", NULL,
        "One extension name per line, created in the session's database right after it is created. A fresh Postgres database carries only what template1 does, so migrations that declare a vector column fail without this. The extension itself must already be installed on the server. Ignored on MySQL.", 2, true },
    [F_DB_POOL] = { "dbPoolEnabled", K_BOOL, "Give each session a database server of its own" },
    [F_DB_RESTORE] = { "dbRestoreSql", K_TEXT, "Restore from .sql", "/home/you/dumps/my_app.sql",
        "A dump on this machine, piped into the database every time a server is claimed, right after it is created, before the setup steps run. Leave empty to skip. The servers themselves are added under Database pool in the sidebar.", 0, true },
    [F_REVIEW_AUTHOR] = { "reviewAuthor", K_TEXT, "PR author", "github-username", NULL, 0, true },
    [F_PUBLISH] = { "reviewPublishInstructions", K_AREA, "Publish steps", NULL,
        "Sent to the agent as its own turn after a \xE2\x8C\x95 Code review: this text and nothing else. Leave empty to run no turn after the review.", 4, false },
    [F_TEST_SHEET] = { "reviewTestSheet", K_BOOL, "Write a test sheet when the \xF0\x9F\x8E\xAC QA errand is started" },
    [F_TEST_RUN] = { "reviewTestRun", K_BOOL, "Execute the test sheet and record a video of each scenario" },
    [F_QA_NOTES] = { "qaNotes", K_AREA, "QA notes", NULL,
        "Appended to the test sheet and test run prompts: logins, tenants, URLs, whatever a tester needs that the repository does not say.", 4, false },
    [F_SHEET_STEPS] = { "testSheetInstructions", K_AREA, "Test sheet closing steps", NULL,
        "Appended to the end of the \xF0\x9F\x93\x8B Test sheet prompt: what this project wants done once the sheet is on the pull request. Leave empty to add nothing.", 4, false },
    [F_FEEDBACK_STEPS] = { "feedbackInstructions", K_AREA, "Feedback closing steps", NULL,
        "Appended to the end of the \xE2\x9A\x99 Implement feedback prompt: what this project wants done once the review comments are implemented. Leave empty to add nothing.", 4, false },
    [F_BUDGET] = { "workerBudgetUsd", K_NUMBER, "Budget (USD)", "no cap",
        "What one orchestration may spend \xE2\x80\x94 the supervisor's turns plus every worker's \xE2\x80\x94 before it pauses and waits for you. Only turns whose provider reports a cost count. Leave empty for no cap.", 0, false },
    [F_IS_SELF] = { "isSelf", K_BOOL, "This project is the dashboard itself" },
    [F_ENV] = { "envTemplate", K_AREA, ".env template", NULL,
        "Written into the checkout as .env before the setup steps run, on every session. Leave empty to use whatever the repository ships.", 10, true },
    [F_RUN] = { "runCommands", K_LIST, "Run commands", NULL,
        "What \xE2\x96\xB6 Run executes, in the checkout, one shell command per line, chained, so the last one is the server that keeps running. {port} is the session's app port, {dir} the checkout.", 4, true },
    [F_PROFILES] = { "runProfiles", K_AREA, "Run profiles", NULL,
        "Optional named configurations \xE2\x96\xB6 Run can serve instead, the first being the default. env: is set over the session's environment (a DB_DATABASE of its own is created on the session's server), before: runs ahead of the run commands, and each of the tenants: gets a hostname of its own on the session's port. Extra placeholders: {profile}, {database}, {host} and {host:<tenant>}.", 8, true },
};

/// The provider, model and effort pickers: the code review's, each errand step's, and the orchestrator's workers'.
/// A flat row edits three keys of the project; a step edits its entry in `stepRuntimes`.
enum { R_REVIEW, R_TEST_SHEET, R_TEST_RUN, R_WORKER, R_COUNT };
typedef struct { const char *provider_key, *model_key, *effort_key, *step, *none, *provider_label; } RuntimeDef;
static const RuntimeDef RUNTIMES[R_COUNT] = {
    [R_REVIEW] = { "reviewProviderId", "reviewModel", "reviewEffort", NULL, "Pick a provider", "Provider" },
    [R_TEST_SHEET] = { NULL, NULL, NULL, "testSheet", "Same as the code review", "Provider" },
    [R_TEST_RUN] = { NULL, NULL, NULL, "testRun", "Same as the code review", "Provider" },
    [R_WORKER] = { "workerProviderId", "workerModel", "workerEffort", NULL, "Same as the orchestrator", "Worker provider" },
};
typedef struct { int provider_id; char *model, *effort; } RuntimePick;   // provider 0: the row's `none`

/// The dashboard's sections, as tabs along the top of the form, the way the pull request page lays out its own.
enum { T_PROJECT, T_DATABASE, T_REVIEW, T_ORCHESTRATOR, T_ENV, T_RUN, T_COUNT };
static const struct { const char *title; wchar_t glyph; } TABS[T_COUNT] = {
    [T_PROJECT] = { "Project", 0xE8B7 }, [T_DATABASE] = { "Database", 0xE1D3 },
    [T_REVIEW] = { "Code review", 0xE721 }, [T_ORCHESTRATOR] = { "Orchestrator", 0xE716 }, [T_ENV] = { "Checkout .env", 0xE8D7 },
    [T_RUN] = { "Run", 0xE768 },
};
/// The open tab stays open from one project to the next, so comparing a setting across projects is one click each.
static int g_tab;
static int field_tab(int f) {
    // The project and how a checkout of it is set up, on one tab.
    if (f <= F_PHP) return T_PROJECT;
    if (f <= F_DB_RESTORE) return T_DATABASE;
    if (f <= F_FEEDBACK_STEPS) return T_REVIEW;
    if (f <= F_IS_SELF) return T_ORCHESTRATOR;
    if (f == F_ENV) return T_ENV;
    return T_RUN;
}
static int runtime_tab(int r) { return r == R_WORKER ? T_ORCHESTRATOR : T_REVIEW; }

enum { ACT_SAVE = 1100, ACT_CLONE, ACT_DELETE, ACT_TAB, ACT_TOGGLE, ACT_PICK, ACT_FOCUS };
enum { ID_FIELD = 2000 };

typedef struct {
    Screen base;
    Json *row;             // what the form was filled from: the saved row, the defaults, or a clone's values
    int id;                // 0 until the project is saved
    HWND edits[F_COUNT];
    RECT rects[F_COUNT];   // each edit's place in content coordinates, from the last layout
    bool laid[F_COUNT];    // laid out in the last layout: its tab is open
    bool clipped[F_COUNT];
    int lines[F_COUNT];    // a long box's lines, which it grows to show, as the dashboard's textareas do
    bool bools[F_COUNT];
    RuntimePick picks[R_COUNT];
    bool has_catalog; RuntimeCatalog catalog;
    Request *req_save, *req_delete, *req_runtimes;
    bool dirty, filling, shown;
    bool tab_dot;          // the open tab showed its unsaved-changes dot at the last layout
    int focused, focus_first;   // the field with the focus, and one to focus once the controls exist; -1 for none
    char *error;
} FormScreen;

/// A long box grows to this many lines, and scrolls inside past it.
enum { MAX_ROWS = 40 };
static int line_count(const char *text) { int n = 1; for (const char *c = text; c && *c; c++) if (*c == '\n') n++; return n; }
static bool is_edit(int f) { return FIELDS[f].kind != K_BOOL; }
static bool is_multiline(int f) { return FIELDS[f].kind == K_LIST || FIELDS[f].kind == K_AREA; }
/// The lines a long box shows: its own number, or as many as it holds up to MAX_ROWS.
static int shown_rows(FormScreen *s, int f) { int n = s->lines[f] > FIELDS[f].rows ? s->lines[f] : FIELDS[f].rows; return n < MAX_ROWS ? n : MAX_ROWS; }
/// Whether the row has this key. A server that dropped a setting, or predates one, gets neither the box nor the key.
static bool row_has(FormScreen *s, const char *key) {
    if (!json_count(s->row)) return true;
    for (size_t i = 0; i < json_count(s->row); i++) if (str_eq(json_key(s->row, i), key)) return true;
    return false;
}
static bool field_offered(FormScreen *s, int f) { return row_has(s, FIELDS[f].key); }
static bool field_enabled(FormScreen *s, int f) { return f != F_DB_RESTORE || s->bools[F_DB_POOL]; }

static void pick_clear(RuntimePick *p) { free(p->model); free(p->effort); memset(p, 0, sizeof *p); }
static void pick_set(RuntimePick *p, int provider_id, const char *model, const char *effort) {
    char *m = model ? xstrdup(model) : NULL, *e = effort ? xstrdup(effort) : NULL;
    pick_clear(p);
    p->provider_id = provider_id; p->model = m; p->effort = e;
}

// MARK: Reading and writing the fields

static char *edit_text(HWND edit) {
    int n = GetWindowTextLengthW(edit);
    wchar_t *w = xmalloc(((size_t)n + 1) * sizeof *w);
    GetWindowTextW(edit, w, n + 1);
    char *text = wide_to_utf8(w); free(w);
    char *lf = str_replace(text, "\r\n", "\n"); free(text);
    return lf;
}
static void set_edit_text(HWND edit, const char *text) {
    char *crlf = str_replace(text ? text : "", "\n", "\r\n");
    wchar_t *w = utf8_to_wide(crlf);
    SetWindowTextW(edit, w);
    free(w); free(crlf);
}
/// A field's value in the row, as the edit shows it: a list one item per line, a number as typed, empty for null.
static char *field_text(const Json *row, int f) {
    const Json *v = json_get(row, FIELDS[f].key);
    if (FIELDS[f].kind == K_LIST) {
        Str out; str_init(&out);
        for (size_t i = 0; i < json_count(v); i++) { const char *line = json_str(json_at(v, i)); if (!line) continue; if (out.len) str_appendc(&out, '\n'); str_appendz(&out, line); }
        return out.data ? str_detach(&out) : xstrdup("");
    }
    double n;
    if (FIELDS[f].kind == K_NUMBER) return json_num(v, &n) && isfinite(n) ? xstrfmt("%.10g", n) : xstrdup("");
    return xstrdup(json_str(v) ? json_str(v) : "");
}
/// A picker's runtime as the row has it saved.
static void row_pick(FormScreen *s, int r, RuntimePick *out) {
    const RuntimeDef *d = &RUNTIMES[r];
    const Json *src = d->step ? json_get(json_get(s->row, "stepRuntimes"), d->step) : s->row;
    const char *pk = d->step ? "providerId" : d->provider_key, *mk = d->step ? "model" : d->model_key, *ek = d->step ? "effort" : d->effort_key;
    pick_set(out, json_int_or(json_get(src, pk), 0), json_str(json_get(src, mk)), json_str(json_get(src, ek)));
}
static void read_picks(FormScreen *s) { for (int r = 0; r < R_COUNT; r++) row_pick(s, r, &s->picks[r]); }
/// Fills every field from `s->row`. The edits only once they exist; the flags and pickers at once.
static void form_fill(FormScreen *s) {
    for (int f = 0; f < F_COUNT; f++) if (!is_edit(f)) s->bools[f] = json_bool_is(json_get(s->row, FIELDS[f].key), true);
    read_picks(s);
    s->filling = true;
    for (int f = 0; f < F_COUNT; f++) {
        if (!s->edits[f]) continue;
        char *text = field_text(s->row, f);
        set_edit_text(s->edits[f], text);
        s->lines[f] = line_count(text);
        free(text);
    }
    s->filling = false;
    s->dirty = false;
}

/// The body a save sends: the row the form came from with every field as it stands now. NULL with `*why` when a field
/// cannot be sent as it is.
static Json *form_body(FormScreen *s, char **why, int *tab) {
    Json *body = json_is_object(s->row) ? json_clone(s->row) : json_object();
    // What the server sets itself, and the order, which belongs to the list's Move up and Move down rather than to one project.
    static const char *const server_keys[] = { "id", "createdAt", "updatedAt", "sortOrder" };
    for (size_t i = 0; i < sizeof server_keys / sizeof *server_keys; i++) json_object_remove(body, server_keys[i]);
    for (int f = 0; f < F_COUNT; f++) {
        const char *key = FIELDS[f].key;
        if (!field_offered(s, f)) continue;
        if (FIELDS[f].kind == K_BOOL) { json_set_bool(body, key, s->bools[f]); continue; }
        if (!s->edits[f]) continue;
        char *text = edit_text(s->edits[f]);
        switch (FIELDS[f].kind) {
        case K_TEXT: { char *t = str_trim(text); json_set_str(body, key, t); free(t); break; }
        case K_AREA: json_set_str(body, key, text); break;
        case K_LIST: {
            // One item per line, blank lines dropped, as the dashboard reads its textareas.
            Json *list = json_array();
            size_t n; char **lines = str_split(text, '\n', &n);
            for (size_t i = 0; i < n; i++) { char *t = str_trim(lines[i]); if (*t) json_array_push(list, json_string(t)); free(t); }
            str_array_free(lines, n);
            json_object_set(body, key, list);
            break;
        }
        case K_NUMBER: {
            char *t = str_trim(text);
            if (!*t) json_object_set(body, key, json_null());
            else {
                char *end; double n = strtod(t, &end);
                if (*end || !isfinite(n) || n < 0) {
                    *tab = field_tab(f);
                    *why = xstrfmt("%s must be a number of dollars, such as 25 or 12.5, or empty for no cap.", FIELDS[f].label);
                    free(t); free(text); json_free(body);
                    return NULL;
                }
                json_set_num(body, key, n);
            }
            free(t);
            break;
        }
        case K_BOOL: break;
        }
        free(text);
    }
    char *repo = str_trim(json_str(json_get(body, "repo")) ? json_str(json_get(body, "repo")) : "");
    bool repo_ok = *repo && strchr(repo, '/') && repo[0] != '/' && repo[strlen(repo) - 1] != '/';
    free(repo);
    if (!repo_ok) { *why = xstrdup("Enter the repository as owner/name."); *tab = T_PROJECT; json_free(body); return NULL; }
    // The runtimes. A step left on "Same as the code review" is no entry at all rather than empty strings.
    bool has_steps = row_has(s, "stepRuntimes");
    Json *steps = json_is_object(json_get(body, "stepRuntimes")) ? json_clone(json_get(body, "stepRuntimes")) : json_object();
    for (int r = 0; r < R_COUNT; r++) {
        const RuntimeDef *d = &RUNTIMES[r];
        const RuntimePick *p = &s->picks[r];
        if (!row_has(s, d->step ? "stepRuntimes" : d->provider_key)) continue;
        if (d->step) {
            if (!p->provider_id) { json_object_remove(steps, d->step); continue; }
            Json *entry = json_object();
            json_set_num(entry, "providerId", p->provider_id);
            json_set_str(entry, "model", p->model ? p->model : "");
            json_set_str(entry, "effort", p->effort ? p->effort : "");
            json_object_set(steps, d->step, entry);
            continue;
        }
        if (p->provider_id) json_set_num(body, d->provider_key, p->provider_id); else json_object_set(body, d->provider_key, json_null());
        json_set_str(body, d->model_key, p->provider_id && p->model ? p->model : "");
        json_set_str(body, d->effort_key, p->provider_id && p->effort ? p->effort : "");
    }
    if (has_steps) json_object_set(body, "stepRuntimes", steps); else json_free(steps);
    return body;
}

static bool tab_changed(FormScreen *s, int t);
static void form_changed(FormScreen *s) {
    if (s->filling) return;
    if (!s->dirty) { s->dirty = true; pane_header_changed(s->base.pane); }
    // The open tab's dot comes and goes as its fields leave and return to what was saved.
    bool changed = tab_changed(s, g_tab);
    if (changed != s->tab_dot) { s->tab_dot = changed; pane_relayout(s->base.pane); }
}

// MARK: Layout

/// Whether a tab holds a change not saved yet, for the dot after its title.
static bool tab_changed(FormScreen *s, int t) {
    for (int f = 0; f < F_COUNT; f++) {
        if (field_tab(f) != t || !field_offered(s, f)) continue;
        if (!is_edit(f)) { if (s->bools[f] != json_bool_is(json_get(s->row, FIELDS[f].key), true)) return true; continue; }
        if (!s->edits[f]) continue;
        char *now = edit_text(s->edits[f]), *saved = field_text(s->row, f);
        bool differs = !str_eq(now, saved);
        free(now); free(saved);
        if (differs) return true;
    }
    for (int r = 0; r < R_COUNT; r++) {
        if (runtime_tab(r) != t) continue;
        RuntimePick saved = { 0 };
        row_pick(s, r, &saved);
        const RuntimePick *p = &s->picks[r];
        bool differs = p->provider_id != saved.provider_id
            || (p->provider_id && (!str_eq(p->model ? p->model : "", saved.model ? saved.model : "") || !str_eq(p->effort ? p->effort : "", saved.effort ? saved.effort : "")));
        pick_clear(&saved);
        if (differs) return true;
    }
    return false;
}
/// GitHub's `tabnav` over the form: one tab per section of the dashboard's form, a dot after any with unsaved changes.
static void layout_tabs(FormScreen *s, Doc *doc, int x, int w) {
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

static void paint_box(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    FormScreen *s = it->data;
    int f = (int)it->arg;
    bool on = field_enabled(s, f);
    COLORREF border = s->focused == f ? theme.accent_dim : theme.line;
    // A disabled edit paints the same fill, so only the border says the box is off.
    fill_round_rect(cv, rc, px(6), theme.raise, on ? border : blend(border, theme.canvas, 0.5));
}
/// A labelled box with its edit, and the hint under it; advances.
static void field(FormScreen *s, Doc *doc, int x, int w, int f) {
    if (!field_offered(s, f)) return;
    const FieldDef *d = &FIELDS[f];
    bool on = field_enabled(s, f);
    doc_text(doc, x, w, d->label, FONT_FOOTNOTE, on ? theme.ink : theme.muted, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
    doc_space(doc, px(6));
    FontId fid = d->mono ? FONT_MONO : FONT_BODY;
    int fh = font_height(doc->cv, fid);
    int h = is_multiline(f) ? shown_rows(s, f) * fh + px(16) : px(36);
    RECT box = { x, doc->y, x + w, doc->y + h };
    int i = doc_add(doc, &box, paint_box);
    Item *it = doc_item(doc, i);
    it->data = s; it->arg = f; it->action = ACT_FOCUS;
    RECT er = is_multiline(f) ? (RECT){ x + px(10), box.top + px(8), x + w - px(3), box.bottom - px(8) }
                              : (RECT){ x + px(10), box.top + (h - fh) / 2, x + w - px(10), box.top + (h - fh) / 2 + fh };
    s->rects[f] = er; s->laid[f] = true;
    if (s->edits[f]) EnableWindow(s->edits[f], on);
    doc->y = box.bottom;
    if (d->hint) { doc_space(doc, px(6)); doc_text(doc, x, w, d->hint, FONT_CAPTION, theme.muted, DT_LEFT | DT_WORDBREAK); }
    doc_space(doc, px(14));
}

static void paint_check(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    FormScreen *s = it->data;
    int f = (int)it->arg;
    bool hovered = doc->hover >= 0 && doc_item(doc, doc->hover) == it;
    int size = px(15), top = rc->top + (rc->bottom - rc->top - size) / 2;
    RECT b = { rc->left, top, rc->left + size, top + size };
    bool on = s->bools[f];
    fill_round_rect(cv, &b, px(3), on ? theme.accent : theme.field, on ? theme.accent : hovered ? theme.accent_dim : theme.line_strong);
    if (on) draw_glyph(cv, 0xE73E, &b, FONT_ICON_SMALL, theme.on_accent);
    RECT t = { b.right + px(8), rc->top, rc->right, rc->bottom };
    draw_text(cv, FIELDS[f].label, &t, FONT_FOOTNOTE, theme.ink, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}
static void check(FormScreen *s, Doc *doc, int x, int w, int f) {
    if (!field_offered(s, f)) return;
    int i = doc_custom(doc, x, w, px(26), paint_check, s, NULL, ACT_TOGGLE, f);
    doc_item(doc, i)->hand = true;
    doc_space(doc, px(10));
}

typedef struct { char *text; bool enabled; } SelectData;
static void select_free(void *p) { SelectData *d = p; free(d->text); free(d); }
static void paint_select(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    SelectData *d = it->data;
    bool hovered = d->enabled && doc->hover >= 0 && doc_item(doc, doc->hover) == it;
    fill_round_rect(cv, rc, px(6), d->enabled ? theme.raise : blend(theme.raise, theme.canvas, 0.5), hovered ? theme.accent_dim : theme.line);
    RECT t = { rc->left + px(12), rc->top, rc->right - px(28), rc->bottom };
    draw_text(cv, d->text, &t, FONT_FOOTNOTE, d->enabled ? theme.ink : theme.muted, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    RECT c = { rc->right - px(26), rc->top, rc->right - px(10), rc->bottom };
    draw_glyph(cv, 0xE70D, &c, FONT_ICON_SMALL, d->enabled ? theme.ink : theme.muted);
}
static char *pick_text(FormScreen *s, int r, int part) {
    const RuntimePick *p = &s->picks[r];
    if (part == 0) {
        if (!p->provider_id) return xstrdup(RUNTIMES[r].none);
        const RuntimeProvider *pr = s->has_catalog ? runtime_catalog_provider(&s->catalog, p->provider_id) : NULL;
        return pr ? xstrdup(pr->label) : xstrfmt("Provider #%d%s", p->provider_id, s->has_catalog ? " (unavailable)" : "");
    }
    if (!p->provider_id) return xstrdup("\xE2\x80\x94");
    if (part == 1) {
        RuntimeChoice c = { p->provider_id, p->model, p->effort };
        const RuntimeModel *m = s->has_catalog ? runtime_catalog_model(&s->catalog, &c) : NULL;
        return xstrdup(m ? runtime_model_title(m) : !str_empty(p->model) ? p->model : "\xE2\x80\x94");
    }
    return xstrdup(!str_empty(p->effort) ? p->effort : "\xE2\x80\x94");
}
/// Provider, model and effort side by side, as the dashboard's `.runtime-row`; advances.
static void runtime_row(FormScreen *s, Doc *doc, int x, int w, int r) {
    if (!row_has(s, RUNTIMES[r].step ? "stepRuntimes" : RUNTIMES[r].provider_key)) return;
    int gap = px(14), cw = (w - 2 * gap) / 3, top = doc->y, h = px(36);
    static const char *const labels[] = { NULL, "Model", "Effort" };
    for (int part = 0; part < 3; part++) {
        int cx = x + part * (cw + gap), width = part == 2 ? w - 2 * (cw + gap) : cw;
        bool enabled = part == 0 || s->picks[r].provider_id != 0;
        RECT lr = { cx, top, cx + width, top + font_height(doc->cv, FONT_FOOTNOTE) };
        doc_text_at(doc, &lr, part ? labels[part] : RUNTIMES[r].provider_label, FONT_FOOTNOTE, enabled ? theme.ink : theme.muted, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
        int by = lr.bottom + px(6);
        RECT box = { cx, by, cx + width, by + h };
        int i = doc_add(doc, &box, paint_select);
        Item *it = doc_item(doc, i);
        SelectData *d = xcalloc(1, sizeof *d); d->text = pick_text(s, r, part); d->enabled = enabled;
        it->data = d; it->free_data = select_free;
        if (enabled) { it->action = ACT_PICK; it->arg = r * 3 + part; it->hand = true; }
        doc->y = box.bottom;
    }
    doc_space(doc, px(14));
}
static void note(Doc *doc, int x, int w, const char *text) {
    doc_text(doc, x, w, text, FONT_CAPTION, theme.muted, DT_LEFT | DT_WORDBREAK);
    doc_space(doc, px(10));
}

static void form_layout(Screen *base, Doc *doc) {
    FormScreen *s = (FormScreen *)base;
    memset(s->laid, 0, sizeof s->laid);
    // The whole pane, as the pull request page uses it, rather than the dashboard's 720px column.
    int col = doc->width, x = 0;
    doc_space(doc, px(8));
    char *why = settings_unavailable();
    if (why) { doc_space(doc, px(10)); doc_text(doc, x, col, why, FONT_FOOTNOTE, theme.muted, DT_LEFT | DT_WORDBREAK); free(why); doc_space(doc, px(12)); return; }
    layout_tabs(s, doc, x, col);
    if (s->error) { doc_notice_box(doc, x, col, s->error); doc_space(doc, px(16)); }
    switch (g_tab) {
    case T_PROJECT: {
        int gap = px(14), half = (col - gap) / 2, top = doc->y;
        field(s, doc, x, half, F_REPO);
        int left_bottom = doc->y;
        doc->y = top;
        field(s, doc, x + half + gap, col - half - gap, F_LABEL);
        if (doc->y < left_bottom) doc->y = left_bottom;
        field(s, doc, x, col, F_LOCAL_DIR);
        field(s, doc, x, col, F_SETUP);
        field(s, doc, x, col, F_PHP);
        note(doc, x, col, "This project's own prompt wording is edited under Prompts on the web dashboard; saving here keeps it as it is.");
        break;
    }
    case T_DATABASE:
        field(s, doc, x, col, F_DB_NAME); field(s, doc, x, col, F_DB_EXT);
        check(s, doc, x, col, F_DB_POOL); field(s, doc, x, col, F_DB_RESTORE);
        break;
    case T_REVIEW:
        field(s, doc, x, (col - px(28)) / 3, F_REVIEW_AUTHOR);
        runtime_row(s, doc, x, col, R_REVIEW);
        field(s, doc, x, col, F_PUBLISH);
        // Each step runs as a turn of its own, on the code review's runtime unless it names one; a step switched off has none.
        check(s, doc, x, col, F_TEST_SHEET);
        if (s->bools[F_TEST_SHEET]) runtime_row(s, doc, x + px(23), col - px(23), R_TEST_SHEET);
        check(s, doc, x, col, F_TEST_RUN);
        if (s->bools[F_TEST_RUN]) runtime_row(s, doc, x + px(23), col - px(23), R_TEST_RUN);
        doc_space(doc, px(4));
        field(s, doc, x, col, F_QA_NOTES); field(s, doc, x, col, F_SHEET_STEPS); field(s, doc, x, col, F_FEEDBACK_STEPS);
        break;
    case T_ORCHESTRATOR:
        runtime_row(s, doc, x, col, R_WORKER);
        field(s, doc, x, col, F_BUDGET);
        note(doc, x, col, "The orchestrator's standing instructions for this project live under Prompts on the web dashboard, in the \xE2\x80\x9C" "Orchestrator instructions\xE2\x80\x9D template.");
        check(s, doc, x, col, F_IS_SELF);
        note(doc, x, col, "Tick it on the repository whose code is running right now, and on no other. An orchestrator on any project that finds a flaw in the tooling running it (a briefing, a worker tool, a loop) can then send a fix worker here, review loop armed, and merge its pull request once the loop approves and the checks are green. The running dashboard keeps its code until you redeploy.");
        break;
    case T_ENV: field(s, doc, x, col, F_ENV); break;
    case T_RUN: field(s, doc, x, col, F_RUN); field(s, doc, x, col, F_PROFILES); break;
    }
    doc_space(doc, px(40));
}

static void form_header(Screen *base, HeaderInfo *info) {
    FormScreen *s = (FormScreen *)base;
    const char *label = json_str_nonempty(json_get(s->row, "label")), *repo = json_str_nonempty(json_get(s->row, "repo"));
    if (s->id) {
        snprintf(info->title, sizeof info->title, "%s", label ? label : repo ? repo : "Project");
        snprintf(info->subtitle, sizeof info->subtitle, "%s", repo ? repo : "");
    } else {
        snprintf(info->title, sizeof info->title, "New project");
        snprintf(info->subtitle, sizeof info->subtitle, "A project is a repository a session can be started against.");
    }
    if (!store_supports("settings_projects")) return;
    bool busy = s->req_save || s->req_delete;
    HeaderButton *b = &info->buttons[info->button_count++];
    snprintf(b->label, sizeof b->label, "%s", s->req_save ? "Saving\xE2\x80\xA6" : "Save");
    b->glyph = 0xE74E; b->action = ACT_SAVE; b->prominent = true; b->tip = "Save this project (Ctrl+S)";
    b->enabled = !busy && (s->dirty || !s->id) && store_supports(s->id ? "update_project" : "create_project");
    if (!s->id) return;
    HeaderButton *c = &info->buttons[info->button_count++];
    c->glyph = 0xE8C8; c->action = ACT_CLONE; c->enabled = !busy && store_supports("create_project"); c->tip = "Clone into a new project";
    HeaderButton *d = &info->buttons[info->button_count++];
    d->glyph = 0xE74D; d->action = ACT_DELETE; d->destructive = true; d->enabled = !busy && store_supports("delete_project"); d->tip = "Delete this project";
}

// MARK: The edits

static int margin_of(Pane *pane) { RECT rc; GetClientRect(pane_hwnd(pane), &rc); return (rc.right - rc.left - pane_content_width(pane)) / 2; }

static void form_place(Screen *base, const RECT *content, int scroll_y) {
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
        // A box half under the header or the window's edge shows only its visible part.
        bool clipped = !EqualRect(&visible, &r);
        if (clipped) SetWindowRgn(e, CreateRectRgn(visible.left - r.left, visible.top - r.top, visible.right - r.left, visible.bottom - r.top), TRUE);
        else if (s->clipped[f]) SetWindowRgn(e, NULL, TRUE);
        s->clipped[f] = clipped;
        ShowWindow(e, SW_SHOWNA);
    }
}

static void form_save(FormScreen *s);
/// The next or previous edit that is laid out and enabled, for Tab.
static int next_field(FormScreen *s, int from, int step) {
    for (int k = 1; k <= F_COUNT; k++) {
        int f = ((from + step * k) % F_COUNT + F_COUNT) % F_COUNT;
        if (s->edits[f] && s->laid[f] && field_enabled(s, f)) return f;
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
        if (ctrl && wp == 'S') { form_save(s); return 0; }
        if (ctrl && wp == 'A') { SendMessageW(hwnd, EM_SETSEL, 0, -1); return 0; }
        if (wp == VK_ESCAPE) { SetFocus(GetParent(hwnd)); return 0; }
        if (wp == VK_RETURN && !is_multiline(f)) { SetFocus(s->edits[next_field(s, f, 1)]); return 0; }
        break;
    case WM_CHAR:
        // The control characters of the keys handled above, which an edit would otherwise beep at or type.
        if (wp == '\t' || wp == 0x13 || wp == 0x01 || wp == 0x1B || (wp == '\r' && !is_multiline(f))) return 0;
        break;
    case WM_MOUSEWHEEL: {
        // The form scrolls under the pointer unless a long box has more of its own to show.
        bool own = is_multiline(f) && SendMessageW(hwnd, EM_GETLINECOUNT, 0, 0) > shown_rows(s, f);
        if (!own) { SendMessageW(GetParent(hwnd), msg, wp, lp); return 0; }
        break;
    }
    case WM_NCDESTROY: RemoveWindowSubclass(hwnd, field_proc, id); break;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}
static void form_ensure_controls(FormScreen *s) {
    if (s->edits[F_REPO]) return;
    HWND owner = pane_hwnd(s->base.pane);
    for (int f = 0; f < F_COUNT; f++) {
        if (!is_edit(f)) continue;
        DWORD style = WS_CHILD | WS_TABSTOP | (is_multiline(f) ? ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN : ES_AUTOHSCROLL);
        HWND e = CreateWindowExW(0, L"EDIT", L"", style, 0, 0, 10, 10, owner, (HMENU)(INT_PTR)(ID_FIELD + f), GetModuleHandleW(NULL), NULL);
        SendMessageW(e, WM_SETFONT, (WPARAM)font(FIELDS[f].mono ? FONT_MONO : FONT_BODY), TRUE);
        SendMessageW(e, EM_SETLIMITTEXT, 0, 0);
        if (FIELDS[f].cue) { wchar_t *w = utf8_to_wide(FIELDS[f].cue); SendMessageW(e, EM_SETCUEBANNER, TRUE, (LPARAM)w); free(w); }
        SetWindowSubclass(e, field_proc, (UINT_PTR)f, (DWORD_PTR)s);
        theme_apply_control(e);
        s->edits[f] = e;
    }
    form_fill(s);
}

// MARK: Runtimes

static void runtimes_done(void *owner, Request *req) {
    FormScreen *s = owner;
    RuntimeCatalog c;
    if (!req->ok || !runtime_catalog_parse(req->result, &c)) return;
    if (s->has_catalog) runtime_catalog_free(&s->catalog);
    s->catalog = c; s->has_catalog = true;
    pane_relayout(s->base.pane);
}
/// The providers the pickers offer. The list is the server's for any project; it is asked with this one, or another.
static void load_runtimes(FormScreen *s) {
    if (!store_supports("runtimes")) return;
    const char *repo = json_str_nonempty(json_get(s->row, "repo"));
    if (!s->id || !repo) {
        size_t n; const Project *all = projects_list(&n);
        repo = n ? all[0].repo : NULL;
        if (!repo && g_settings) repo = json_str_nonempty(json_get(json_at(settings_rows(g_settings), 0), "repo"));
    }
    if (!repo) return;
    Json *args = json_object(); json_set_str(args, "repo", repo);
    store_call("runtimes", args, 0, s, runtimes_done, 0, &s->req_runtimes);
}

static void append_item(HMENU menu, UINT id, const char *text, bool checked, bool enabled) {
    wchar_t *w = utf8_to_wide(text);
    AppendMenuW(menu, MF_STRING | (checked ? MF_CHECKED : 0) | (enabled ? 0 : MF_GRAYED), id, w);
    free(w);
}
static int popup(FormScreen *s, HMENU menu, POINT pt) {
    int chosen = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, pt.x, pt.y, 0, pane_hwnd(s->base.pane), NULL);
    DestroyMenu(menu);
    return chosen;
}
static void pick(FormScreen *s, int r, int part, POINT pt) {
    RuntimePick *p = &s->picks[r];
    const RuntimeProvider *pr = s->has_catalog ? runtime_catalog_provider(&s->catalog, p->provider_id) : NULL;
    HMENU m = CreatePopupMenu();
    if (part == 0) {
        append_item(m, 1, RUNTIMES[r].none, p->provider_id == 0, true);
        size_t n = s->has_catalog ? s->catalog.provider_count : 0;
        if (n || (p->provider_id && !pr)) AppendMenuW(m, MF_SEPARATOR, 0, NULL);
        for (size_t i = 0; i < n; i++) {
            const RuntimeProvider *o = &s->catalog.providers[i];
            char *label = runtime_provider_available(o) ? xstrdup(o->label) : xstrfmt("%s (unavailable)", o->label);
            append_item(m, (UINT)i + 2, label, o->id == p->provider_id, runtime_provider_available(o) || o->id == p->provider_id);
            free(label);
        }
        // A configured provider the list no longer carries stays picked rather than collapsing to the first choice.
        if (p->provider_id && !pr) { char *label = xstrfmt("Provider #%d (unavailable)", p->provider_id); append_item(m, 9999, label, true, false); free(label); }
        int chosen = popup(s, m, pt);
        if (chosen == 1 && p->provider_id) { pick_clear(p); form_changed(s); }
        else if (chosen >= 2 && (size_t)chosen - 2 < n && s->catalog.providers[chosen - 2].id != p->provider_id) {
            // A new provider starts on its default model and that model's default effort.
            RuntimeChoice c;
            if (runtime_catalog_choice(&s->catalog, s->catalog.providers[chosen - 2].id, NULL, &c)) { pick_set(p, c.provider_id, c.model, c.effort); runtime_choice_free(&c); form_changed(s); }
        }
    } else if (part == 1) {
        size_t n = pr ? pr->model_count : 0;
        for (size_t i = 0; i < n; i++) append_item(m, (UINT)i + 1, runtime_model_title(&pr->models[i]), str_eq(pr->models[i].id, p->model), true);
        if (!n) append_item(m, 9999, s->has_catalog ? "No models listed" : "Loading the providers\xE2\x80\xA6", false, false);
        int chosen = popup(s, m, pt);
        if (chosen >= 1 && (size_t)chosen - 1 < n && !str_eq(pr->models[chosen - 1].id, p->model)) {
            // The effort carries over when the new model offers it, else that model's default.
            const RuntimeModel *model = &pr->models[chosen - 1];
            bool offered = false;
            for (size_t k = 0; k < model->effort_count; k++) if (str_eq(model->efforts[k], p->effort)) offered = true;
            const char *effort = offered ? p->effort : model->default_effort ? model->default_effort : model->effort_count ? model->efforts[0] : "";
            char *keep = xstrdup(effort);
            pick_set(p, p->provider_id, model->id, keep);
            free(keep);
            form_changed(s);
        }
    } else {
        RuntimeChoice c = { p->provider_id, p->model, p->effort };
        size_t n = 0;
        const char *const *efforts = s->has_catalog ? runtime_catalog_efforts(&s->catalog, &c, &n) : NULL;
        for (size_t i = 0; i < n; i++) append_item(m, (UINT)i + 1, efforts[i], str_eq(efforts[i], p->effort), true);
        if (!n) append_item(m, 9999, s->has_catalog ? "This model takes no effort" : "Loading the providers\xE2\x80\xA6", false, false);
        int chosen = popup(s, m, pt);
        if (chosen >= 1 && (size_t)chosen - 1 < n && !str_eq(efforts[chosen - 1], p->effort)) {
            char *effort = xstrdup(efforts[chosen - 1]);
            pick_set(p, p->provider_id, p->model, effort);
            free(effort);
            form_changed(s);
        }
    }
    pane_relayout(s->base.pane);
}

// MARK: Saving, cloning, deleting

static void form_set_id(FormScreen *s, int id) {
    s->id = id;
    free(s->base.id); s->base.id = form_id(id);
    pane_set_selected_id(app_sidebar_pane(), s->base.id);
}
static void save_done(void *owner, Request *req) {
    FormScreen *s = owner;
    const Json *row = req->ok ? json_get(req->result, "project") : NULL;
    if (!json_is_object(row)) {
        char *text = req->ok ? xstrdup("The server returned an unexpected response.") : request_error_text(req);
        set_string(&s->error, text); free(text);
        pane_relayout(s->base.pane); pane_header_changed(s->base.pane); pane_scroll_to_top(s->base.pane);
        return;
    }
    // The server's word on what was saved: it may have tidied a value, and a new project now has an id.
    json_free(s->row); s->row = json_clone(row);
    set_string(&s->error, NULL);
    form_set_id(s, row_id(row));
    form_fill(s);
    pane_relayout(s->base.pane); pane_header_changed(s->base.pane);
    settings_projects_changed(s->id);
}
static void form_save(FormScreen *s) {
    if (s->req_save || s->req_delete || !store_supports(s->id ? "update_project" : "create_project")) return;
    char *why = NULL; int tab = g_tab;
    Json *body = form_body(s, &why, &tab);
    if (!body) {
        // The tab holding the field at fault comes up with the reason above it.
        set_string(&s->error, why); free(why);
        g_tab = tab;
        pane_relayout(s->base.pane); pane_scroll_to_top(s->base.pane);
        return;
    }
    if (s->id) json_set_num(body, "id", s->id);
    store_call(s->id ? "update_project" : "create_project", body, 0, s, save_done, 0, &s->req_save);
    pane_header_changed(s->base.pane);
}
static void delete_done(void *owner, Request *req) {
    FormScreen *s = owner;
    if (!req->ok) {
        char *text = request_error_text(req); set_string(&s->error, text); free(text);
        pane_relayout(s->base.pane); pane_header_changed(s->base.pane); pane_scroll_to_top(s->base.pane);
        return;
    }
    // The list says the project is gone, and the sidebar opens the first one left in its place.
    s->dirty = false;
    app_clear_detail();
    settings_projects_changed(0);
}
static void form_delete(FormScreen *s) {
    if (!s->id || s->req_save || s->req_delete) return;
    const char *name = json_str_nonempty(json_get(s->row, "repo"));
    char *title = xstrfmt("Delete %s?", name ? name : "this project");
    bool ok = app_confirm(title, "Sessions already started against it keep their history, but no new one can be.", "Delete", true);
    free(title);
    if (!ok) return;
    Json *args = json_object(); json_set_num(args, "id", s->id);
    store_call("delete_project", args, 0, s, delete_done, 0, &s->req_delete);
    pane_header_changed(s->base.pane);
}
static void form_clone(FormScreen *s) {
    char *why = NULL; int tab = g_tab;
    Json *copy = form_body(s, &why, &tab);
    if (!copy) { set_string(&s->error, why); free(why); g_tab = tab; pane_relayout(s->base.pane); pane_scroll_to_top(s->base.pane); return; }
    // The copy carries what the form holds now, saved or not; the repository is unique, so it starts without one.
    json_set_str(copy, "repo", "");
    s->dirty = false;
    Screen *clone = project_settings_screen_new(copy, NULL);
    ((FormScreen *)clone)->focus_first = F_REPO;
    g_tab = T_PROJECT;
    json_free(copy);
    app_show_detail(clone);
}

// MARK: The screen

static void form_destroy(Screen *base) {
    FormScreen *s = (FormScreen *)base;
    request_cancel(&s->req_save); request_cancel(&s->req_delete); request_cancel(&s->req_runtimes);
    for (int f = 0; f < F_COUNT; f++) if (s->edits[f]) DestroyWindow(s->edits[f]);
    for (int r = 0; r < R_COUNT; r++) pick_clear(&s->picks[r]);
    if (s->has_catalog) runtime_catalog_free(&s->catalog);
    json_free(s->row); free(s->error);
    screen_release(base);
}
static void form_action(Screen *base, int action, intptr_t arg, POINT pt) {
    FormScreen *s = (FormScreen *)base;
    switch (action) {
    case ACT_SAVE: form_save(s); break;
    case ACT_CLONE: form_clone(s); break;
    case ACT_DELETE: form_delete(s); break;
    case ACT_TAB:
        if (arg < 0 || arg >= T_COUNT) break;
        // The focus leaves with the boxes of the tab that closes.
        if (s->focused >= 0) SetFocus(pane_hwnd(base->pane));
        g_tab = (int)arg;
        pane_relayout(base->pane); pane_scroll_to_top(base->pane);
        break;
    case ACT_TOGGLE: if (arg >= 0 && arg < F_COUNT) { s->bools[arg] = !s->bools[arg]; form_changed(s); pane_relayout(base->pane); } break;
    case ACT_PICK: if (arg >= 0 && arg < R_COUNT * 3) pick(s, (int)arg / 3, (int)arg % 3, pt); break;
    case ACT_FOCUS: if (arg >= 0 && arg < F_COUNT && s->edits[arg] && field_enabled(s, (int)arg)) SetFocus(s->edits[arg]); break;
    }
}
static void form_command(Screen *base, int id, int code, HWND control) {
    (void)control;
    FormScreen *s = (FormScreen *)base;
    int f = id - ID_FIELD;
    if (f < 0 || f >= F_COUNT || !s->edits[f]) return;
    switch (code) {
    case EN_CHANGE:
        form_changed(s);
        if (is_multiline(f)) {
            char *text = edit_text(s->edits[f]);
            int n = line_count(text);
            free(text);
            if (n != s->lines[f]) { s->lines[f] = n; pane_relayout(base->pane); }
        }
        break;
    case EN_SETFOCUS: {
        s->focused = f;
        // Tabbing onto a box out of view brings it into view.
        RECT content = pane_content_rect(base->pane);
        int top = s->rects[f].top - px(40), bottom = s->rects[f].bottom + px(16), y = pane_scroll_y(base->pane);
        if (top < y || bottom > y + (content.bottom - content.top)) pane_scroll_to(base->pane, top);
        pane_repaint(base->pane);
        break;
    }
    case EN_KILLFOCUS: if (s->focused == f) s->focused = -1; pane_repaint(base->pane); break;
    }
}
static bool form_key(Screen *base, WPARAM vk, bool ctrl, bool shift) {
    (void)shift;
    if (ctrl && vk == 'S') { form_save((FormScreen *)base); return true; }
    return false;
}
static void form_visible(Screen *base, bool shown) {
    FormScreen *s = (FormScreen *)base;
    s->shown = shown;
    if (shown) {
        form_ensure_controls(s);
        // Asked once the screen is up: a screen made and then refused (its predecessor kept its changes) asks nothing.
        if (!s->has_catalog && !s->req_runtimes) load_runtimes(s);
        if (s->focus_first >= 0) { int f = s->focus_first; s->focus_first = -1; SetFocus(s->edits[f]); }
    } else for (int f = 0; f < F_COUNT; f++) if (s->edits[f]) ShowWindow(s->edits[f], SW_HIDE);
}
static bool form_can_leave(Screen *base) {
    FormScreen *s = (FormScreen *)base;
    if (!s->dirty) return true;
    const char *name = json_str_nonempty(json_get(s->row, "label"));
    char *message = s->id ? xstrfmt("The changes to %s have not been saved.", name ? name : "this project") : xstrdup("The new project has not been saved.");
    bool leave = app_confirm("Discard unsaved changes?", message, "Discard", true);
    free(message);
    if (leave) s->dirty = false;
    return leave;
}

static const ScreenVTable form_vt = {
    .destroy = form_destroy, .layout = form_layout, .header = form_header, .action = form_action, .place = form_place,
    .visible = form_visible, .command = form_command, .key = form_key, .can_leave = form_can_leave,
};
Screen *project_settings_screen_new(const Json *row, const Json *defaults) {
    FormScreen *s = xcalloc(1, sizeof *s);
    s->base.vt = &form_vt;
    s->focused = -1; s->focus_first = -1;
    // A saved row, a clone's values without an id, or a new project from the server's defaults.
    s->row = json_is_object(row) ? json_clone(row) : json_is_object(defaults) ? json_clone(defaults) : json_object();
    s->id = row_id(s->row);
    s->base.id = form_id(s->id);
    // A new project starts where its repository is typed.
    if (!s->id) { s->focus_first = F_REPO; g_tab = T_PROJECT; }
    form_fill(s);
    return &s->base;
}
