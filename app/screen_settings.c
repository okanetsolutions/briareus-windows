// Settings, as the dashboard's settings page: a sidebar of its own (Back to sessions, the mail accounts, the projects, the
// providers, the database pool, the SSH servers, the Forge accounts and the Slack workspaces, each with ＋ New) and a project's form, its
// sections as tabs, or an SSH server's, a Forge account's or a Slack workspace's on one tab, saved through
// /settings/projects, /settings/ssh/servers, /settings/forge/accounts and /settings/slack/workspaces. The provider form is screen_provider_settings.c and a
// database server's screen_db_servers.c. Those routes need an Admin token; any other token gets a sentence saying so.
#include "meeting.h"
#include "mcp.h"
#include "screens.h"
#include "str.h"
#include <commctrl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void sign_out(void) {
    if (!app_confirm("Sign out of this dashboard?", "The device token and the saved conversations are removed from this computer. Revoke the token itself in web Settings.", "Sign out", true)) return;
    store_forget();
}
static bool in_rect(const RECT *r, POINT pt) { return pt.x >= r->left && pt.x < r->right && pt.y >= r->top && pt.y < r->bottom; }
static int row_id(const Json *row) { return json_int_or(json_get(row, "id"), 0); }
static char *form_id(int id) { return id > 0 ? xstrfmt("settings-project:%d", id) : xstrdup("settings-project:new"); }
/// An SSH server's id is the time it was registered in milliseconds, past what an int holds.
static char *ssh_form_id(double id) { return id > 0 ? xstrfmt("settings-ssh:%.0f", id) : xstrdup("settings-ssh:new"); }
static double ssh_row_id(const Json *row) { double id; return json_num(json_get(row, "id"), &id) && isfinite(id) ? id : 0; }
/// A Forge account's id is, like an SSH server's, the time it was added in milliseconds.
static char *forge_form_id(double id) { return id > 0 ? xstrfmt("settings-forge:%.0f", id) : xstrdup("settings-forge:new"); }
/// A Slack workspace's id is also the time it was added in milliseconds.
static char *slack_form_id(double id) { return id > 0 ? xstrfmt("settings-slack:%.0f", id) : xstrdup("settings-slack:new"); }
/// A settings form in the detail pane: a project's, a provider's, a database server's, an SSH server's, a Forge account's,
/// a Slack workspace's or the meeting assistant's.
static bool is_form_id(const char *id) {
    return id && (str_has_prefix(id, "settings-project:") || str_has_prefix(id, "settings-provider:") || str_has_prefix(id, "settings-db:")
                  || str_has_prefix(id, "settings-ssh:") || str_has_prefix(id, "settings-forge:") || str_has_prefix(id, "settings-slack:")
                  || str_has_prefix(id, "settings-mcp:") || str_eq(id, "settings-meeting") || str_eq(id, "mail-settings") || str_has_prefix(id, "mail-settings:"));
}

/// Why `what` cannot be shown here, as a new string; NULL when it can. `path` is the list's route.
static char *unavailable(const char *call, const char *path, const char *what, const char *manage) {
    if (store_supports(call)) return NULL;
    bool listed = false;
    for (size_t i = 0; i < g_store.route_count; i++) if (str_has_suffix(g_store.routes[i].path, path)) listed = true;
    const char *permission = g_store.has_device && g_store.device.permission ? g_store.device.permission : "unknown";
    return listed
        ? xstrfmt("%s need an Admin token, and this device's token is %s. Create an Admin token on the web dashboard under Settings \xE2\x86\x92 Devices and clients and connect with it.", what, permission)
        : xstrfmt("This server does not offer %s (GET /%s) on its client API. Update the server to manage %s here.", what, path, manage);
}
static char *settings_unavailable(void) { return unavailable("settings_projects", "settings/projects", "Project settings", "projects"); }
static char *ssh_unavailable(void) { return unavailable("settings_ssh_servers", "settings/ssh/servers", "SSH servers", "them"); }

// MARK: - The sidebar

enum { ACT_BACK = 1000, ACT_NEW_PROJECT, ACT_OPEN_PROJECT, ACT_NEW_PROVIDER, ACT_OPEN_PROVIDER, ACT_NEW_SERVER, ACT_OPEN_SERVER, ACT_NEW_SSH,
       ACT_OPEN_SSH, ACT_NEW_FORGE, ACT_OPEN_FORGE, ACT_NEW_SLACK, ACT_OPEN_SLACK, ACT_OPEN_MEETING, ACT_NEW_MCP, ACT_OPEN_MCP, ACT_MOVE_UP, ACT_MOVE_DOWN, ACT_NEW_MAIL, ACT_OPEN_MAIL };
enum { MENU_UP = 1, MENU_DOWN };

typedef struct {
    Screen base;
    Json *projects;     // the server's Project rows, in their order
    Json *defaults;     // what a new one starts from
    bool loaded;
    char *error;
    Request *req, *req_order;
    Json *providers;    // the server's Provider rows (`list`) and what a new one starts from (`defaults`)
    bool providers_loaded, open_first_provider;   // the second: open the first row once the list is read (after a delete)
    char *providers_error;
    Request *req_providers;
    Json *servers;      // the database pool: the server's DbServer rows (`list`) and what a new one starts from (`defaults`)
    bool servers_loaded, open_first_server;   // the second: open the first row once the list is read (after a delete)
    char *servers_error;
    Request *req_servers;
    Json *ssh;          // the server's SshServer rows as `list`, and `defaults`
    bool ssh_loaded;
    char *ssh_error;
    Request *req_ssh;
    Json *forge;        // the server's ForgeAccount rows as `list`, and `defaults`
    bool forge_loaded;
    char *forge_error;
    Request *req_forge;
    Json *slack;        // the server's SlackWorkspace rows as `list`, and `defaults`
    bool slack_loaded;
    char *slack_error;
    Request *req_slack;
    Json *mcp; Request *req_mcp; ApiClient *mcp_account; char *mcp_error; bool mcp_loaded;
    Json *mail; Request *req_mail; ApiClient *mail_client; char *mail_error; bool mail_loaded;
    RECT signout_rc;
} SettingsScreen;

/// The settings sidebar on screen, for a form to tell when the list changed.
static SettingsScreen *g_settings;

static void paint_back(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    bool hovered = doc_item_hovered(doc, it);
    RECT t = { rc->left + px(6), rc->top, rc->right, rc->bottom };
    draw_text(cv, "\xE2\x86\x90 Back to sessions", &t, FONT_CAPTION, hovered ? theme.ink : theme.muted, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
}

typedef struct { char *label, *repo; bool enabled, db, selected, movable; } ProjectRowData;   // movable: ↑ and ↓ lie over it
static void project_row_free(void *p) { ProjectRowData *d = p; free(d->label); free(d->repo); free(d); }
/// A project row's ↑ or ↓, laid over the right end of its first line; `row` is the row's item.
typedef struct { int row; bool up, enabled, selected; } MoveData;
static void paint_move(Doc *doc, Item *it, Canvas *cv, const RECT *rc);
/// Whether the mouse is on the row or on one of its arrows.
static bool row_hovered(Doc *doc, Item *it) {
    if (doc_item_hovered(doc, it)) return true;
    Item *h = doc->hover >= 0 ? doc_item(doc, doc->hover) : NULL;
    return h && h->paint == paint_move && doc_item(doc, ((MoveData *)h->data)->row) == it;
}
static void paint_move(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    MoveData *d = it->data;
    // Shown on the row under the mouse and on the open one, as the dashboard's row actions are.
    if (!d->selected && !row_hovered(doc, doc_item(doc, d->row))) return;
    bool hovered = d->enabled && doc_item_hovered(doc, it);
    fill_round_rect(cv, rc, px(4), hovered ? theme.sidebar : theme.raise, hovered ? theme.accent_dim : theme.raise);
    draw_glyph(cv, d->up ? 0xE70E : 0xE70D, rc, FONT_ICON_SMALL, !d->enabled ? blend(theme.muted, theme.raise, 0.5) : hovered ? theme.ink : theme.muted);
}
static void paint_project_row(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    ProjectRowData *d = it->data;
    bool hovered = row_hovered(doc, it);
    if (hovered || d->selected) fill_round_rect(cv, rc, px(6), theme.raise, theme.raise);
    int x = rc->left + px(8), top = rc->top + px(6), lh = px(22);
    // `.dot.idle` for a project sessions can start on, the plain grey dot for one switched off.
    draw_status_dot(cv, x + px(3), top + lh / 2, d->enabled ? "idle" : "");
    // The label stops short of the arrows while they show.
    int label_right = d->movable && (hovered || d->selected) ? rc->right - px(6) - px(22) * 2 - px(2) - px(6) : rc->right - px(8);
    RECT t = { x + px(7) + px(7), top, label_right, top + lh };
    draw_text(cv, d->label, &t, FONT_SUBHEADLINE, theme.ink, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    int y2 = top + lh, right = rc->right - px(8);
    if (d->db) {
        // `rounded border border-line px-1 text-[11px]`: the database pool's tag.
        int h; int bw = draw_chip(NULL, 0, 0, "db", theme.muted, theme.sidebar, &h);
        int rw = text_width(cv, d->repo, FONT_CAPTION);
        int bx = x + (rw < right - x - bw - px(8) ? rw : right - x - bw - px(8)) + px(8);
        draw_chip(cv, bx, y2 + (px(18) - h) / 2, "db", theme.muted, hovered || d->selected ? theme.raise : theme.sidebar, &h);
        right = bx - px(8);
    }
    RECT r = { x, y2, right, y2 + px(18) };
    draw_text(cv, d->repo, &r, FONT_CAPTION, theme.muted, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

typedef struct { char *label, *binary; bool active, login, endpoint, selected, unsaved; } ProviderRowData;
static void provider_row_free(void *p) { ProviderRowData *d = p; free(d->label); free(d->binary); free(d); }
static void paint_provider_row(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    ProviderRowData *d = it->data;
    bool hovered = doc_item_hovered(doc, it);
    COLORREF background = hovered || d->selected ? theme.raise : theme.sidebar;
    if (hovered || d->selected) fill_round_rect(cv, rc, px(6), theme.raise, theme.raise);
    int x = rc->left + px(8), top = rc->top + px(6), lh = px(22);
    draw_status_dot(cv, x + px(3), top + lh / 2, d->active ? "idle" : "");
    RECT t = { x + px(7) + px(7), top, rc->right - px(8), top + lh };
    draw_text(cv, d->label, &t, FONT_SUBHEADLINE, theme.ink, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    // The dashboard's badges: the CLI it runs, and what sets it apart.
    const char *tags[] = { d->binary, d->active || d->unsaved ? NULL : "inactive", d->login ? "own login" : NULL, d->endpoint ? "custom endpoint" : NULL };
    int bx = x, y2 = top + lh, right = rc->right - px(8);
    for (size_t i = 0; i < sizeof tags / sizeof *tags; i++) {
        if (str_empty(tags[i])) continue;
        int bw = text_width(cv, tags[i], FONT_CAPTION2) + px(12) + 2;
        if (bx + bw > right) break;
        int h; draw_chip(cv, bx, y2 + px(1), tags[i], theme.muted, background, &h);
        bx += bw + px(6);
    }
}

static void settings_open_row(SettingsScreen *s, size_t index) {
    const Json *row = json_at(json_get(s->projects, "list"), index);
    if (json_is_object(row)) app_show_detail(project_settings_screen_new(row, json_get(s->projects, "defaults")));
}
static const Json *settings_rows(SettingsScreen *s) { return json_get(s->projects, "list"); }
static const Json *provider_rows(SettingsScreen *s) { return json_get(s->providers, "list"); }
static void settings_open_provider(SettingsScreen *s, size_t index) {
    const Json *row = json_at(provider_rows(s), index);
    if (json_is_object(row)) app_show_detail(provider_settings_screen_new(row, json_get(s->providers, "defaults")));
}

static void section_title(Doc *doc, int w, const char *title, const char *count, int action);
static void mcp_done(void *owner, Request *req) {
    SettingsScreen *s = owner;
    if (req->client != s->mcp_account || req->client != g_store.client || !mcp_settings_supported("settings_mcp_servers")) return;
    s->mcp_loaded = true;
    if (!req->ok || !json_is_array(json_get(req->result, "servers"))) {
        set_string(&s->mcp_error, "MCP servers could not be loaded. Refresh Settings or reconnect.");
        json_free(s->mcp); s->mcp = json_object();
    } else {
        set_string(&s->mcp_error, NULL); json_free(s->mcp); s->mcp = json_clone(req->result);
    }
    pane_relayout(s->base.pane);
}
static void mcp_load(SettingsScreen *s) {
    if (s->mcp_account != g_store.client) {
        request_cancel(&s->req_mcp); api_client_release(s->mcp_account); s->mcp_account = api_client_retain(g_store.client);
        json_free(s->mcp); s->mcp = json_object(); s->mcp_loaded = false; set_string(&s->mcp_error, NULL);
    }
    if (s->req_mcp || !mcp_settings_supported("settings_mcp_servers")) return;
    store_call("settings_mcp_servers", json_object(), 0, s, mcp_done, 0, &s->req_mcp);
}
void settings_mcp_changed(void) {
    if (!g_settings) return;
    request_cancel(&g_settings->req_mcp); mcp_load(g_settings);
}
static void mail_prepare(SettingsScreen *s) {
    if (s->mail_client == g_store.client) return;
    request_cancel(&s->req_mail);
    api_client_release(s->mail_client);
    s->mail_client = api_client_retain(g_store.client);
    json_free(s->mail); s->mail = json_object();
    s->mail_loaded = false;
    set_string(&s->mail_error, NULL);
}
static void mail_done(void *owner, Request *req) {
    SettingsScreen *s = owner;
    if (req->client != s->mail_client || req->client != g_store.client || !mail_settings_offered()) return;
    s->mail_loaded = true;
    if (!req->ok || !json_is_array(json_get(req->result, "accounts"))) {
        set_string(&s->mail_error, "Mail accounts could not be loaded. Refresh Settings or reconnect.");
        json_free(s->mail); s->mail = json_object();
    } else {
        set_string(&s->mail_error, NULL);
        json_free(s->mail); s->mail = json_clone(req->result);
    }
    pane_relayout(s->base.pane);
}
static void mail_load(SettingsScreen *s) {
    if (!mail_settings_offered()) return;
    mail_prepare(s);
    if (s->req_mail || s->mail_loaded) return;
    store_call("settings_mail_accounts", json_object(), 0, s, mail_done, 0, &s->req_mail);
}
void settings_mail_changed(const Json *list) {
    if (!g_settings || !mail_settings_offered()) return;
    if (list && json_is_array(json_get(list, "accounts")) && g_settings->mail_client == g_store.client) {
        request_cancel(&g_settings->req_mail);
        json_free(g_settings->mail);
        g_settings->mail = json_clone(list);
        g_settings->mail_loaded = true;
        set_string(&g_settings->mail_error, NULL);
        pane_relayout(g_settings->base.pane);
        return;
    }
    request_cancel(&g_settings->req_mail);
    g_settings->mail_loaded = false;
    mail_load(g_settings);
}
static void layout_mcp(SettingsScreen *s, Doc *doc, int w, const char *selected) {
    doc_space(doc, px(18));
    section_title(doc, w, "MCP servers", NULL, mcp_settings_supported("create_mcp_server") && s->mcp_account == g_store.client && s->mcp_loaded && !s->mcp_error ? ACT_NEW_MCP : 0);
    if (s->mcp_error) doc_notice(doc, px(8), w - px(16), s->mcp_error);
    if (s->mcp_account != g_store.client || !s->mcp_loaded) { doc_loading(doc, 0, w, "Loading MCP servers…"); return; }
    const Json *rows = json_get(s->mcp, "servers");
    for (size_t i = 0; i < json_count(rows); i++) {
        const Json *row = json_at(rows, i); double id = mcp_server_id(row); if (!id) continue;
        char *label = xstrfmt("%s · %s%s", json_str_or(json_get(row, "transport"), "http"), json_str_or(json_get(row, "status"), "unchecked"), json_bool_is(json_get(row, "enabled"), false) ? " · disabled" : "");
        char *screen_id = xstrfmt("settings-mcp:%.0f", id);
        ProjectRowData *d = xcalloc(1, sizeof *d);
        d->label = xstrdup(json_str_or(json_get(row, "label"), "MCP server")); d->repo = label;
        d->enabled = !json_bool_is(json_get(row, "enabled"), false) && str_eq(json_str(json_get(row, "status")), "ready"); d->selected = str_eq(selected, screen_id);
        doc_custom(doc, 0, w, px(52), paint_project_row, d, project_row_free, ACT_OPEN_MCP, (intptr_t)i);
        free(screen_id);
    }
    if (!json_count(rows) && !s->mcp_error) doc_text(doc, px(8), w - px(16), "No MCP servers yet.", FONT_FOOTNOTE, theme.muted, DT_WORDBREAK);
}
static void settings_load(SettingsScreen *s);
static void settings_done(void *owner, Request *req) {
    SettingsScreen *s = owner;
    s->loaded = true;
    if (!req->ok) { request_error_into(&s->error, req); pane_relayout(s->base.pane); return; }
    set_string(&s->error, NULL);
    json_free(s->projects);
    s->projects = json_object();
    json_object_set(s->projects, "list", json_clone(json_get(req->result, "projects")));
    json_object_set(s->projects, "defaults", json_clone(json_get(req->result, "defaults")));
    pane_relayout(s->base.pane);
    // The dashboard's settings page opens on its first project, or on a new one when there is none; so does this,
    // unless a settings form is already up.
    Screen *root = pane_root(app_detail_pane());
    if (root && is_form_id(root->id)) return;
    if (json_count(settings_rows(s))) settings_open_row(s, 0);
    else app_show_detail(project_settings_screen_new(NULL, json_get(s->projects, "defaults")));
}
static void ssh_done(void *owner, Request *req) {
    SettingsScreen *s = owner;
    s->ssh_loaded = true;
    if (!req->ok) { request_error_into(&s->ssh_error, req); pane_relayout(s->base.pane); return; }
    set_string(&s->ssh_error, NULL);
    json_free(s->ssh);
    s->ssh = json_object();
    json_object_set(s->ssh, "list", json_clone(json_get(req->result, "servers")));
    json_object_set(s->ssh, "defaults", json_clone(json_get(req->result, "defaults")));
    pane_relayout(s->base.pane);
}
static void ssh_load(SettingsScreen *s) {
    if (s->req_ssh || !store_supports("settings_ssh_servers")) { s->ssh_loaded = true; return; }
    store_call("settings_ssh_servers", json_object(), 0, s, ssh_done, 0, &s->req_ssh);
}
static void forge_done(void *owner, Request *req) {
    SettingsScreen *s = owner;
    s->forge_loaded = true;
    if (!req->ok) { request_error_into(&s->forge_error, req); pane_relayout(s->base.pane); return; }
    set_string(&s->forge_error, NULL);
    json_free(s->forge);
    s->forge = json_object();
    json_object_set(s->forge, "list", json_clone(json_get(req->result, "accounts")));
    json_object_set(s->forge, "defaults", json_clone(json_get(req->result, "defaults")));
    pane_relayout(s->base.pane);
}
static void forge_load(SettingsScreen *s) {
    if (s->req_forge || !store_supports("settings_forge_accounts")) { s->forge_loaded = true; return; }
    store_call("settings_forge_accounts", json_object(), 0, s, forge_done, 0, &s->req_forge);
}
static void slack_done(void *owner, Request *req) {
    SettingsScreen *s = owner;
    s->slack_loaded = true;
    if (!req->ok) { request_error_into(&s->slack_error, req); pane_relayout(s->base.pane); return; }
    set_string(&s->slack_error, NULL);
    json_free(s->slack);
    s->slack = json_object();
    json_object_set(s->slack, "list", json_clone(json_get(req->result, "workspaces")));
    json_object_set(s->slack, "defaults", json_clone(json_get(req->result, "defaults")));
    pane_relayout(s->base.pane);
    // The open form marks the projects another workspace already serves from this list.
    Screen *root = pane_root(app_detail_pane());
    if (root && str_has_prefix(root->id, "settings-slack:")) pane_relayout(root->pane);
}
static void slack_load(SettingsScreen *s) {
    if (s->req_slack || !store_supports("settings_slack_workspaces")) { s->slack_loaded = true; return; }
    store_call("settings_slack_workspaces", json_object(), 0, s, slack_done, 0, &s->req_slack);
}
static void projects_load(SettingsScreen *s) {
    if (s->req || !store_supports("settings_projects")) { s->loaded = true; return; }
    store_call("settings_projects", json_object(), 0, s, settings_done, 0, &s->req);
}
static void providers_done(void *owner, Request *req) {
    SettingsScreen *s = owner;
    s->providers_loaded = true;
    if (!req->ok) { request_error_into(&s->providers_error, req); pane_relayout(s->base.pane); return; }
    set_string(&s->providers_error, NULL);
    json_free(s->providers);
    s->providers = json_object();
    json_object_set(s->providers, "list", json_clone(json_get(req->result, "providers")));
    json_object_set(s->providers, "defaults", json_clone(json_get(req->result, "defaults")));
    pane_relayout(s->base.pane);
    // After a delete the first provider left opens in its place, as a project's delete opens the first project left.
    if (!s->open_first_provider) return;
    s->open_first_provider = false;
    Screen *root = pane_root(app_detail_pane());
    if (root && is_form_id(root->id)) return;
    if (json_count(provider_rows(s))) settings_open_provider(s, 0);
    else app_show_detail(provider_settings_screen_new(NULL, json_get(s->providers, "defaults")));
}
static void providers_load(SettingsScreen *s) {
    if (s->req_providers || !store_supports("settings_providers")) { s->providers_loaded = true; return; }
    store_call("settings_providers", json_object(), 0, s, providers_done, 0, &s->req_providers);
}
void settings_providers_changed(bool open_first) {
    if (!g_settings) return;
    g_settings->open_first_provider = open_first;
    request_cancel(&g_settings->req_providers);
    providers_load(g_settings);
}
static const Json *server_rows(SettingsScreen *s) { return json_get(s->servers, "list"); }
static void servers_open_row(SettingsScreen *s, size_t index) {
    const Json *row = json_at(server_rows(s), index);
    if (json_is_object(row)) app_show_detail(db_server_settings_screen_new(row, json_get(s->servers, "defaults")));
}
static void servers_done(void *owner, Request *req) {
    SettingsScreen *s = owner;
    s->servers_loaded = true;
    if (!req->ok) { request_error_into(&s->servers_error, req); pane_relayout(s->base.pane); return; }
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
    if (root && is_form_id(root->id)) return;
    if (json_count(server_rows(s))) servers_open_row(s, 0);
    else app_show_detail(db_server_settings_screen_new(NULL, json_get(s->servers, "defaults")));
}
static void servers_load(SettingsScreen *s) {
    if (s->req_servers || !store_supports("settings_db_servers")) { s->servers_loaded = true; return; }
    store_call("settings_db_servers", json_object(), 0, s, servers_done, 0, &s->req_servers);
}
void settings_db_servers_changed(bool open_first) {
    if (!g_settings) return;
    g_settings->open_first_server = open_first;
    request_cancel(&g_settings->req_servers);
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
static void settings_load(SettingsScreen *s) { projects_load(s); ssh_load(s); forge_load(s); slack_load(s); mcp_load(s); mail_load(s); }
void settings_projects_changed(int select_id) {
    (void)select_id;   // the form's own id is what the sidebar highlights
    if (!g_settings) return;
    request_cancel(&g_settings->req);
    projects_load(g_settings);
}
static void settings_ssh_changed(void) {
    servers_ssh_changed();
    if (!g_settings) return;
    request_cancel(&g_settings->req_ssh);
    ssh_load(g_settings);
}
static const Json *ssh_rows(SettingsScreen *s) { return json_get(s->ssh, "list"); }
static Screen *ssh_settings_screen_new(const Json *row, const Json *defaults);
static void ssh_open_row(SettingsScreen *s, size_t index) {
    const Json *row = json_at(ssh_rows(s), index);
    if (json_is_object(row)) app_show_detail(ssh_settings_screen_new(row, json_get(s->ssh, "defaults")));
}
static void settings_forge_changed(void) {
    if (!g_settings) return;
    request_cancel(&g_settings->req_forge);
    forge_load(g_settings);
}
static const Json *forge_rows(SettingsScreen *s) { return json_get(s->forge, "list"); }
static Screen *forge_settings_screen_new(const Json *row, const Json *defaults);
static void forge_open_row(SettingsScreen *s, size_t index) {
    const Json *row = json_at(forge_rows(s), index);
    if (json_is_object(row)) app_show_detail(forge_settings_screen_new(row, json_get(s->forge, "defaults")));
}
static void settings_slack_changed(void) {
    slack_inbox_settings_changed();
    if (!g_settings) return;
    request_cancel(&g_settings->req_slack);
    slack_load(g_settings);
}
static const Json *slack_rows(SettingsScreen *s) { return json_get(s->slack, "list"); }
static Screen *slack_settings_screen_new(const Json *row, const Json *defaults);
static void slack_open_row(SettingsScreen *s, size_t index) {
    const Json *row = json_at(slack_rows(s), index);
    if (json_is_object(row)) app_show_detail(slack_settings_screen_new(row, json_get(s->slack, "defaults")));
}

static void order_done(void *owner, Request *req) {
    SettingsScreen *s = owner;
    if (!req->ok) { request_error_into(&s->error, req); }
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
    pane_relayout(s->base.pane);   // the arrows are off until the server answers
}

static void settings_destroy(Screen *base) {
    SettingsScreen *s = (SettingsScreen *)base;
    if (g_settings == s) g_settings = NULL;
    request_cancel(&s->req); request_cancel(&s->req_order); request_cancel(&s->req_providers); request_cancel(&s->req_servers);
    request_cancel(&s->req_ssh); request_cancel(&s->req_forge); request_cancel(&s->req_slack);
    json_free(s->projects); free(s->error);
    json_free(s->providers); free(s->providers_error);
    json_free(s->servers); free(s->servers_error);
    json_free(s->ssh); free(s->ssh_error);
    json_free(s->forge); free(s->forge_error);
    json_free(s->slack); free(s->slack_error);
    request_cancel(&s->req_mcp); json_free(s->mcp); free(s->mcp_error); api_client_release(s->mcp_account);
    request_cancel(&s->req_mail); json_free(s->mail); free(s->mail_error); api_client_release(s->mail_client);
    screen_release(base);
}
/// A section's summary: its title, a muted note after it when `note` is set, and its ＋ New when `new_action` is set.
static void section_title(Doc *doc, int w, const char *title, const char *note, int new_action) {
    int y = doc->y, h = px(20);
    RECT tr = { px(8), y, w - px(60), y + h };
    doc_text_at(doc, &tr, title, FONT_CAPTION_SEMIBOLD, theme.muted, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    int nx = px(8) + text_width(doc->cv, title, FONT_CAPTION_SEMIBOLD) + px(6);
    if (note && nx < tr.right) {
        RECT nr = { nx, y, tr.right, y + h };
        doc_text_at(doc, &nr, note, FONT_CAPTION2, theme.muted, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
    if (new_action) {
        int nw = text_width(doc->cv, "\xEF\xBC\x8B New", FONT_CAPTION) + px(8);
        RECT nr = { w - px(4) - nw, y, w - px(4), y + h };
        Item *it = doc_item(doc, doc_text_at(doc, &nr, "\xEF\xBC\x8B New", FONT_CAPTION, theme.muted, DT_CENTER | DT_VCENTER | DT_SINGLELINE));
        it->action = new_action; it->hand = true;
    }
    doc->y = y + h;
    doc_space(doc, px(4));
}
/// The SSH servers under the projects, as the web's settings sidebar lists them: each with its dot, label and project.
static void layout_ssh(SettingsScreen *s, Doc *doc, int w, const char *selected) {
    doc_space(doc, px(8));
    char *why = ssh_unavailable();
    section_title(doc, w, "SSH servers", NULL, why ? 0 : ACT_NEW_SSH);
    if (why) { doc_text(doc, px(8), w - px(16), why, FONT_FOOTNOTE, theme.muted, DT_LEFT | DT_WORDBREAK); free(why); return; }
    if (s->ssh_error) { doc_notice(doc, px(8), w - px(16), s->ssh_error); doc_space(doc, px(8)); }
    const Json *rows = ssh_rows(s);
    int row_h = px(6) + px(22) + px(18) + px(6);
    for (size_t i = 0; i < json_count(rows); i++) {
        const Json *row = json_at(rows, i);
        ProjectRowData *d = xcalloc(1, sizeof *d);
        const char *label = json_str_nonempty(json_get(row, "label")), *repo = json_str(json_get(row, "repo"));
        d->label = xstrdup(label ? label : json_str_nonempty(json_get(row, "host")) ? json_str(json_get(row, "host")) : "SSH server");
        d->repo = xstrdup(repo ? repo : "");
        d->enabled = !json_bool_is(json_get(row, "enabled"), false);
        char *id = ssh_form_id(ssh_row_id(row));
        d->selected = str_eq(selected, id);
        free(id);
        doc_custom(doc, 0, w, row_h, paint_project_row, d, project_row_free, ACT_OPEN_SSH, (intptr_t)i);
    }
    // A server being registered shows as its own row until it is saved.
    if (str_eq(selected, "settings-ssh:new")) {
        ProjectRowData *d = xcalloc(1, sizeof *d);
        d->label = xstrdup("New SSH server"); d->repo = xstrdup("not saved yet"); d->selected = true;
        doc_custom(doc, 0, w, row_h, paint_project_row, d, project_row_free, 0, 0);
    }
    if (s->ssh_loaded && !json_count(rows) && !s->ssh_error) doc_text(doc, px(8), w - px(16), "No SSH servers registered. \xEF\xBC\x8B New lets a project's sessions run commands on one, with approval.", FONT_FOOTNOTE, theme.muted, DT_WORDBREAK);
    if (!s->ssh_loaded) doc_loading(doc, 0, w, "Loading SSH servers\xE2\x80\xA6");
}
/// The Forge accounts under the SSH servers: each with its dot (a token is stored), label, organization and projects.
static void layout_forge(SettingsScreen *s, Doc *doc, int w, const char *selected) {
    doc_space(doc, px(8));
    section_title(doc, w, "Forge accounts", NULL, store_supports("create_forge_account") ? ACT_NEW_FORGE : 0);
    if (s->forge_error) { doc_notice(doc, px(8), w - px(16), s->forge_error); doc_space(doc, px(8)); }
    const Json *rows = forge_rows(s);
    int row_h = px(6) + px(22) + px(18) + px(6);
    for (size_t i = 0; i < json_count(rows); i++) {
        const Json *row = json_at(rows, i);
        ProjectRowData *d = xcalloc(1, sizeof *d);
        const char *label = json_str_nonempty(json_get(row, "label")), *org = json_str_nonempty(json_get(row, "organization"));
        const Json *repos = json_get(row, "repos");
        size_t n = json_count(repos);
        d->label = xstrdup(label ? label : org ? org : "Forge account");
        d->repo = n == 1 && json_str(json_at(repos, 0)) ? xstrfmt("%s \xC2\xB7 %s", org ? org : "", json_str(json_at(repos, 0)))
                                                         : xstrfmt("%s \xC2\xB7 %zu projects", org ? org : "", n);
        d->enabled = json_bool_is(json_get(row, "hasToken"), true);
        char *id = forge_form_id(ssh_row_id(row));
        d->selected = str_eq(selected, id);
        free(id);
        doc_custom(doc, 0, w, row_h, paint_project_row, d, project_row_free, ACT_OPEN_FORGE, (intptr_t)i);
    }
    // An account being added shows as its own row until it is saved.
    if (str_eq(selected, "settings-forge:new")) {
        ProjectRowData *d = xcalloc(1, sizeof *d);
        d->label = xstrdup("New Forge account"); d->repo = xstrdup("not saved yet"); d->selected = true;
        doc_custom(doc, 0, w, row_h, paint_project_row, d, project_row_free, 0, 0);
    }
    if (s->forge_loaded && !json_count(rows) && !s->forge_error) doc_text(doc, px(8), w - px(16), "No Forge accounts yet. \xEF\xBC\x8B New adds a Laravel Forge organization and the projects that may use it.", FONT_FOOTNOTE, theme.muted, DT_WORDBREAK);
    if (!s->forge_loaded) doc_loading(doc, 0, w, "Loading Forge accounts\xE2\x80\xA6");
}
/// The Slack workspaces under the Forge accounts: each with its dot (a token is stored), label, workspace and projects.
static void layout_slack(SettingsScreen *s, Doc *doc, int w, const char *selected) {
    doc_space(doc, px(8));
    section_title(doc, w, "Slack workspaces", NULL, store_supports("create_slack_workspace") ? ACT_NEW_SLACK : 0);
    if (s->slack_error) { doc_notice(doc, px(8), w - px(16), s->slack_error); doc_space(doc, px(8)); }
    const Json *rows = slack_rows(s);
    int row_h = px(6) + px(22) + px(18) + px(6);
    for (size_t i = 0; i < json_count(rows); i++) {
        const Json *row = json_at(rows, i);
        ProjectRowData *d = xcalloc(1, sizeof *d);
        const char *label = json_str_nonempty(json_get(row, "label")), *team = json_str_nonempty(json_get(row, "team"));
        const Json *projects = json_get(row, "projects");
        size_t n = json_count(projects);
        const char *one = n == 1 ? json_str(json_get(json_at(projects, 0), "repo")) : NULL;
        d->label = xstrdup(label ? label : team ? team : "Slack workspace");
        d->repo = one ? xstrfmt("%s \xC2\xB7 %s", team ? team : "", one) : xstrfmt("%s \xC2\xB7 %zu projects", team ? team : "", n);
        d->enabled = json_bool_is(json_get(row, "hasToken"), true);
        char *id = slack_form_id(ssh_row_id(row));
        d->selected = str_eq(selected, id);
        free(id);
        doc_custom(doc, 0, w, row_h, paint_project_row, d, project_row_free, ACT_OPEN_SLACK, (intptr_t)i);
    }
    // A workspace being added shows as its own row until it is saved.
    if (str_eq(selected, "settings-slack:new")) {
        ProjectRowData *d = xcalloc(1, sizeof *d);
        d->label = xstrdup("New Slack workspace"); d->repo = xstrdup("not saved yet"); d->selected = true;
        doc_custom(doc, 0, w, row_h, paint_project_row, d, project_row_free, 0, 0);
    }
    if (s->slack_loaded && !json_count(rows) && !s->slack_error) doc_text(doc, px(8), w - px(16), "No Slack workspaces yet. \xEF\xBC\x8B New connects an operator inbox and optionally lets project sessions send messages.", FONT_FOOTNOTE, theme.muted, DT_WORDBREAK);
    if (!s->slack_loaded) doc_loading(doc, 0, w, "Loading Slack workspaces\xE2\x80\xA6");
}
/// Mail under This computer: each mailbox is a row, the way a project is, and ＋ New starts a sign-in.
static void layout_mail(SettingsScreen *s, Doc *doc, int w, const char *selected) {
    bool can_new = s->mail_loaded && !s->mail_error && store_supports("connect_mail_account");
    if (can_new) {
        bool provider = false;
        const Json *providers = json_get(s->mail, "providers");
        for (size_t i = 0; i < json_count(providers); i++) {
            const char *p = json_str(json_at(providers, i));
            if (str_eq(p, "gmail") || str_eq(p, "outlook")) provider = true;
        }
        can_new = provider;
    }
    section_title(doc, w, "Mail", NULL, can_new ? ACT_NEW_MAIL : 0);
    if (s->mail_error) { doc_notice(doc, px(8), w - px(16), s->mail_error); doc_space(doc, px(8)); }
    if (!s->mail_loaded) { doc_loading(doc, 0, w, "Loading mail accounts\xE2\x80\xA6"); return; }
    const Json *rows = json_get(s->mail, "accounts");
    int row_h = px(6) + px(22) + px(18) + px(6);
    for (size_t i = 0; i < json_count(rows); i++) {
        const Json *row = json_at(rows, i);
        int id = json_int_or(json_get(row, "id"), 0);
        if (id <= 0) continue;
        const char *email = json_str_nonempty(json_get(row, "email"));
        const char *label = json_str_nonempty(json_get(row, "label"));
        const char *provider = json_str_nonempty(json_get(row, "provider"));
        const char *status = json_str_nonempty(json_get(row, "status"));
        bool enabled = json_bool_is(json_get(row, "enabled"), true);
        ProjectRowData *d = xcalloc(1, sizeof *d);
        d->label = xstrdup(label ? label : email ? email : "Mailbox");
        d->repo = label && email ? xstrfmt("%s \xC2\xB7 %s", provider ? provider : "mail", email)
                                 : xstrfmt("%s \xC2\xB7 %s", provider ? provider : "mail", status ? status : "unknown");
        d->enabled = enabled && str_eq(status, "connected");
        char *sid = xstrfmt("mail-settings:%d", id);
        d->selected = str_eq(selected, sid);
        free(sid);
        doc_custom(doc, 0, w, row_h, paint_project_row, d, project_row_free, ACT_OPEN_MAIL, (intptr_t)i);
    }
    if (str_eq(selected, "mail-settings")) {
        ProjectRowData *d = xcalloc(1, sizeof *d);
        d->label = xstrdup("New mailbox"); d->repo = xstrdup("not connected yet"); d->selected = true;
        doc_custom(doc, 0, w, row_h, paint_project_row, d, project_row_free, 0, 0);
    }
    if (!json_count(rows) && !s->mail_error) doc_text(doc, px(8), w - px(16), "No connected mailboxes yet. \xEF\xBC\x8B New signs in to Gmail or Outlook.", FONT_FOOTNOTE, theme.muted, DT_WORDBREAK);
}
static void layout_projects(SettingsScreen *s, Doc *doc, int w, const char *selected);
static void layout_providers(SettingsScreen *s, Doc *doc, int w, const char *selected);
static void layout_servers(SettingsScreen *s, Doc *doc, int w, const char *selected);
static void settings_layout(Screen *base, Doc *doc) {
    SettingsScreen *s = (SettingsScreen *)base;
    int w = doc->width;
    const char *selected = pane_selected_id(base->pane);
    doc_space(doc, px(8));
    doc_custom(doc, 0, w, px(24), paint_back, NULL, NULL, ACT_BACK, 0);
    doc_space(doc, px(16));
    // This computer's own settings come first: they need no Admin token.
    section_title(doc, w, "This computer", NULL, 0);
    ProjectRowData *meeting = xcalloc(1, sizeof *meeting);
    meeting->label = xstrdup("\xF0\x9F\x8E\x99 Meeting assistant");
    meeting->repo = xstrdup(meeting_has_key(MEETING_KEY_ELEVENLABS) ? "ElevenLabs API key saved" : "add an ElevenLabs API key");
    meeting->enabled = meeting_has_key(MEETING_KEY_ELEVENLABS);
    meeting->selected = str_eq(selected, "settings-meeting");
    doc_custom(doc, 0, w, px(6) + px(22) + px(18) + px(6), paint_project_row, meeting, project_row_free, ACT_OPEN_MEETING, 0);
    doc_space(doc, px(16));
    if (mail_settings_offered()) { layout_mail(s, doc, w, selected); doc_space(doc, px(16)); }
    char *why = settings_unavailable();
    section_title(doc, w, "Projects", NULL, why ? 0 : ACT_NEW_PROJECT);
    if (why) { doc_text(doc, px(8), w - px(16), why, FONT_FOOTNOTE, theme.muted, DT_LEFT | DT_WORDBREAK); free(why); layout_ssh(s, doc, w, selected); if (mcp_settings_supported("settings_mcp_servers")) layout_mcp(s, doc, w, selected); doc_space(doc, px(8)); return; }
    layout_projects(s, doc, w, selected);
    // The providers sessions start on, then the database pool, below the projects as on the dashboard; a server without
    // the routes shows neither.
    if (store_supports("settings_providers")) {
        doc_space(doc, px(8));
        section_title(doc, w, "Providers", NULL, store_supports("create_provider") ? ACT_NEW_PROVIDER : 0);
        layout_providers(s, doc, w, selected);
    }
    if (store_supports("settings_db_servers")) {
        doc_space(doc, px(8));
        // One open session with a database per server in the pool, so the pool's size heads the section, as the sessions
        // it lets run at once (the dashboard's "· 2 parallel sessions with a database", cut to the sidebar's width).
        size_t n = settings_pool_capacity();
        char *note = n ? xstrfmt("\xC2\xB7 %zu session%s", n, n == 1 ? "" : "s") : NULL;
        section_title(doc, w, "Database pool", note, store_supports("create_db_server") ? ACT_NEW_SERVER : 0);
        free(note);
        layout_servers(s, doc, w, selected);
    }
    // The SSH servers agents may run commands on, as on the dashboard, then the Forge accounts on a server that has them.
    layout_ssh(s, doc, w, selected);
    if (store_supports("settings_forge_accounts")) layout_forge(s, doc, w, selected);
    if (store_supports("settings_slack_workspaces")) layout_slack(s, doc, w, selected);
    if (mcp_settings_supported("settings_mcp_servers")) layout_mcp(s, doc, w, selected);
}
/// The database pool: each server with its dot and host:port.
static void layout_servers(SettingsScreen *s, Doc *doc, int w, const char *selected) {
    if (s->servers_error) { doc_notice(doc, px(8), w - px(16), s->servers_error); doc_space(doc, px(8)); }
    const Json *rows = server_rows(s);
    int h = px(6) + px(22) + px(18) + px(6);
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
        doc_custom(doc, 0, w, h, paint_project_row, d, project_row_free, ACT_OPEN_SERVER, (intptr_t)i);
    }
    if (str_eq(selected, "settings-db:new")) {
        ProjectRowData *d = xcalloc(1, sizeof *d);
        d->label = xstrdup("New database server"); d->repo = xstrdup("not saved yet"); d->selected = true;
        doc_custom(doc, 0, w, h, paint_project_row, d, project_row_free, 0, 0);
    }
    if (s->servers_loaded && !json_count(rows) && !s->servers_error) doc_text(doc, px(8), w - px(16), "No servers yet. Add one so sessions can claim a database of their own.", FONT_FOOTNOTE, theme.muted, DT_WORDBREAK);
    if (!s->servers_loaded) doc_loading(doc, 0, w, "Loading the database pool\xE2\x80\xA6");
    doc_space(doc, px(8));
}
static void layout_providers(SettingsScreen *s, Doc *doc, int w, const char *selected) {
    if (s->providers_error) { doc_notice(doc, px(8), w - px(16), s->providers_error); doc_space(doc, px(8)); }
    const Json *rows = provider_rows(s);
    int h = px(6) + px(22) + px(18) + px(6);
    for (size_t i = 0; i < json_count(rows); i++) {
        const Json *row = json_at(rows, i);
        ProviderRowData *d = xcalloc(1, sizeof *d);
        const char *label = json_str_nonempty(json_get(row, "label")), *binary = json_str(json_get(row, "binary"));
        d->label = label ? xstrdup(label) : xstrfmt("Provider #%d", row_id(row));
        d->binary = xstrdup(binary ? binary : "");
        d->active = !json_bool_is(json_get(row, "active"), false);
        d->login = json_bool_is(json_get(row, "hasLogin"), true);
        d->endpoint = json_str_nonempty(json_get(row, "baseUrl")) != NULL;
        char *id = xstrfmt("settings-provider:%d", row_id(row));
        d->selected = str_eq(selected, id);
        free(id);
        doc_custom(doc, 0, w, h, paint_provider_row, d, provider_row_free, ACT_OPEN_PROVIDER, (intptr_t)i);
    }
    if (str_eq(selected, "settings-provider:new")) {
        ProviderRowData *d = xcalloc(1, sizeof *d);
        d->label = xstrdup("New provider"); d->binary = xstrdup("not saved yet"); d->unsaved = true; d->selected = true;
        doc_custom(doc, 0, w, h, paint_provider_row, d, provider_row_free, 0, 0);
    }
    if (s->providers_loaded && !json_count(rows) && !s->providers_error) doc_text(doc, px(8), w - px(16), "No providers yet. Add one so sessions can be started.", FONT_FOOTNOTE, theme.muted, DT_WORDBREAK);
    if (!s->providers_loaded) doc_loading(doc, 0, w, "Loading providers\xE2\x80\xA6");
    doc_space(doc, px(8));
}
/// The ↑ and ↓ that move project `i` of `n` up and down the list, at the right end of its row's first line.
static void layout_move_arrows(SettingsScreen *s, Doc *doc, int row, size_t i, size_t n, bool selected) {
    RECT rc = doc_item(doc, row)->rc;
    int size = px(22), right = rc.right - px(6), top = rc.top + px(6);
    for (int k = 0; k < 2; k++) {
        bool up = k == 0, enabled = !s->req_order && (up ? i > 0 : i + 1 < n);
        RECT b = { right - (2 - k) * size - (1 - k) * px(2), top, right - (1 - k) * (size + px(2)), top + size };
        Item *it = doc_item(doc, doc_add(doc, &b, paint_move));
        MoveData *d = xcalloc(1, sizeof *d);
        d->row = row; d->up = up; d->enabled = enabled; d->selected = selected;
        it->data = d; it->free_data = free;
        // A disabled arrow still takes the click, so the end of the list does not open the project under it.
        it->action = enabled ? (up ? ACT_MOVE_UP : ACT_MOVE_DOWN) : ACTION_TIP; it->arg = (intptr_t)i; it->hand = enabled;
        it->tip = xstrdup(up ? "Move up" : "Move down");
    }
}
static void layout_projects(SettingsScreen *s, Doc *doc, int w, const char *selected) {
    if (s->error) { doc_notice(doc, px(8), w - px(16), s->error); doc_space(doc, px(8)); }
    const Json *rows = settings_rows(s);
    // The order is the dashboard sidebar's and the composer's, so it is moved from here.
    bool movable = json_count(rows) > 1 && store_supports("order_projects");
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
        d->movable = movable;
        bool opened = d->selected;
        int item = doc_custom(doc, 0, w, px(6) + px(22) + px(18) + px(6), paint_project_row, d, project_row_free, ACT_OPEN_PROJECT, (intptr_t)i);
        if (movable) layout_move_arrows(s, doc, item, i, json_count(rows), opened);
    }
    // A project being added shows as its own row until it is saved.
    if (str_eq(selected, "settings-project:new")) {
        ProjectRowData *d = xcalloc(1, sizeof *d);
        d->label = xstrdup("New project"); d->repo = xstrdup("not saved yet"); d->selected = true;
        doc_custom(doc, 0, w, px(6) + px(22) + px(18) + px(6), paint_project_row, d, project_row_free, 0, 0);
    }
    if (s->loaded && !json_count(rows) && !s->error) doc_text(doc, px(8), w - px(16), "No projects yet. \xEF\xBC\x8B New adds a repository sessions can be started against.", FONT_FOOTNOTE, theme.muted, DT_WORDBREAK);
    if (!s->loaded) doc_loading(doc, 0, w, "Loading projects\xE2\x80\xA6");
    doc_space(doc, px(8));
}
static void settings_header(Screen *base, HeaderInfo *info) { (void)base; (void)info; }

/// The foot, as the dashboard's settings page has it: `Settings` 12px muted and the `⎋ Sign out` button, above a border.
static int settings_footer_height(Screen *base, int width) { (void)base; (void)width; return px(6) + 1 + px(8) + px(26) + px(8); }
static void settings_footer_paint(Screen *base, Canvas *cv, const RECT *rc) {
    SettingsScreen *s = (SettingsScreen *)base;
    fill_rect(cv, rc, theme.sidebar);
    int top = rc->top + px(6);
    draw_line(cv, rc->left + px(10), top, rc->right - px(10), top, theme.line);
    int y = top + 1 + px(8), h = px(26), left = rc->left + px(16), right = rc->right - px(10);
    const char *out = "\xE2\x8E\x8B Sign out";
    int ow = text_width(cv, out, FONT_CAPTION) + px(20);
    RECT a = { left, y, right - ow - px(8), y + h }, c = { right - ow, y, right, y + h };
    draw_text(cv, "Settings", &a, FONT_CAPTION, theme.muted, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    fill_round_rect(cv, &c, px(7), theme.raise, theme.line);
    draw_text(cv, out, &c, FONT_CAPTION, theme.ink, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    s->signout_rc = c;
}
static void settings_footer_click(Screen *base, POINT pt) { SettingsScreen *s = (SettingsScreen *)base; if (in_rect(&s->signout_rc, pt)) sign_out(); }

static void settings_back(SettingsScreen *s) {
    // The settings forms go with the sidebar that opened them, unless one keeps its unsaved changes.
    Screen *root = pane_root(app_detail_pane());
    if (root && is_form_id(root->id)) {
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
    case ACT_NEW_PROJECT: app_show_detail(project_settings_screen_new(NULL, json_get(s->projects, "defaults"))); break;
    case ACT_OPEN_PROJECT: settings_open_row(s, (size_t)arg); break;
    case ACT_NEW_PROVIDER: app_show_detail(provider_settings_screen_new(NULL, json_get(s->providers, "defaults"))); break;
    case ACT_OPEN_PROVIDER: settings_open_provider(s, (size_t)arg); break;
    case ACT_NEW_SERVER: app_show_detail(db_server_settings_screen_new(NULL, json_get(s->servers, "defaults"))); break;
    case ACT_OPEN_SERVER: servers_open_row(s, (size_t)arg); break;
    case ACT_NEW_SSH: app_show_detail(ssh_settings_screen_new(NULL, json_get(s->ssh, "defaults"))); break;
    case ACT_OPEN_SSH: ssh_open_row(s, (size_t)arg); break;
    case ACT_NEW_FORGE: app_show_detail(forge_settings_screen_new(NULL, json_get(s->forge, "defaults"))); break;
    case ACT_OPEN_FORGE: forge_open_row(s, (size_t)arg); break;
    case ACT_NEW_SLACK: app_show_detail(slack_settings_screen_new(NULL, json_get(s->slack, "defaults"))); break;
    case ACT_OPEN_SLACK: slack_open_row(s, (size_t)arg); break;
    case ACT_NEW_MCP:
        if (mcp_settings_supported("create_mcp_server") && s->mcp_account == g_store.client && s->mcp_loaded && !s->mcp_error) app_show_detail(mcp_settings_screen_new(NULL, json_get(s->mcp, "defaults")));
        break;
    case ACT_OPEN_MCP:
        if (mcp_settings_supported("settings_mcp_servers") && s->mcp_account == g_store.client) {
            const Json *row = json_at(json_get(s->mcp, "servers"), (size_t)arg);
            if (mcp_server_id(row)) app_show_detail(mcp_settings_screen_new(row, NULL));
        }
        break;
    case ACT_NEW_MAIL:
        if (mail_settings_offered() && store_supports("connect_mail_account")) app_show_detail(mail_settings_screen_new());
        break;
    case ACT_OPEN_MAIL: {
        if (!mail_settings_offered()) break;
        const Json *row = json_at(json_get(s->mail, "accounts"), (size_t)arg);
        int id = json_int_or(json_get(row, "id"), 0);
        if (id > 0) app_show_detail(mail_settings_screen_open(id));
        break;
    }
    case ACT_OPEN_MEETING: app_show_detail(meeting_settings_screen_new()); break;
    case ACT_MOVE_UP: settings_move(s, (size_t)arg, -1); break;
    case ACT_MOVE_DOWN: settings_move(s, (size_t)arg, 1); break;
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
    if (!shown) return;
    if (!s->loaded && !s->req) projects_load(s);
    if (!s->providers_loaded && !s->req_providers) providers_load(s);
    if (!s->servers_loaded && !s->req_servers) servers_load(s);
    if (!s->ssh_loaded && !s->req_ssh) ssh_load(s);
    if (!s->forge_loaded && !s->req_forge) forge_load(s);
    if (!s->slack_loaded && !s->req_slack) slack_load(s);
    if (!s->mcp_loaded || s->mcp_account != g_store.client) mcp_load(s);
    if (mail_settings_offered() && (!s->mail_loaded || s->mail_client != g_store.client)) mail_load(s);
}
static void settings_refresh(Screen *base) {
    SettingsScreen *s = (SettingsScreen *)base;
    request_cancel(&s->req); request_cancel(&s->req_ssh); request_cancel(&s->req_forge); request_cancel(&s->req_slack); request_cancel(&s->req_mcp);
    request_cancel(&s->req_mail); s->mail_loaded = false; settings_load(s);
    request_cancel(&s->req_providers); providers_load(s);
    request_cancel(&s->req_servers); servers_load(s);
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
    s->projects = json_object(); s->ssh = json_object(); s->forge = json_object(); s->slack = json_object();
    s->providers = json_object();
    s->servers = json_object();
    s->mcp = json_object();
    s->mail = json_object();
    g_settings = s;
    return &s->base;
}

// MARK: - The project form

typedef enum { K_TEXT, K_LIST, K_AREA, K_NUMBER, K_BOOL, K_BOARD } FieldKind;   // K_BOARD: the Projects board, edited as its address
/// One of the form's fields: the Project key it edits, how, and the words around it. `rows` sizes a multi-line box.
typedef struct { const char *key; FieldKind kind; const char *label, *cue, *hint; int rows; bool mono; } FieldDef;

enum {
    F_REPO, F_LABEL, F_ENABLED, F_LOCAL_DIR, F_BOARD,
    F_SETUP, F_PHP,
    F_DB_NAME, F_DB_EXT, F_DB_POOL, F_DB_RESTORE,
    F_REVIEW_AUTHOR, F_PUBLISH, F_AUTO_LOOP, F_TEST_SHEET, F_TEST_RUN, F_QA_NOTES, F_SHEET_STEPS, F_FEEDBACK_STEPS,
    F_BUDGET, F_IS_SELF,
    F_ENV,
    F_RUN, F_PROFILES,
    F_COUNT
};
static const FieldDef FIELDS[F_COUNT] = {
    [F_REPO] = { "repo", K_TEXT, "Repository", "owner/name", "Cloned over HTTPS with the machine's own git credentials.", 0, false },
    [F_LABEL] = { "label", K_TEXT, "Label", "shown in the project dropdown", NULL, 0, false },
    [F_ENABLED] = { "enabled", K_BOOL, "Active: sessions can be started on this project" },
    [F_LOCAL_DIR] = { "localDir", K_TEXT, "Local checkout", "/home/you/www/your-checkout",
        "This machine's own checkout of the repo. A session started in Local mode works directly in it: no clone, no setup steps, no pooled database, and the tree is used exactly as it stands. Leave empty to keep Local mode off for this project.", 0, true },
    [F_BOARD] = { "projectBoard", K_BOARD, "GitHub Projects board", "https://github.com/orgs/acme/projects/1/views/2",
        "The board the project's \xE2\x96\xA6 Board tab shows: its address on GitHub, with the view whose filter and columns it follows. The server's GitHub token needs Projects: read. Leave empty for no Board tab.", 0, true },
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
    [F_AUTO_LOOP] = { "autonomousReviewLoop", K_BOOL, "Autonomous review loop: fix every finding and review again" },
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
    char *m = xstrdup(model), *e = xstrdup(effort);
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
    if (FIELDS[f].kind == K_BOARD) { char *url = project_board_setting_url(v); return url ? url : xstrdup(""); }
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
        case K_BOARD: {
            char *t = str_trim(text);
            Json *board = *t ? project_board_setting_from_url(t) : json_null();
            free(t);
            if (!board) {
                *tab = field_tab(f);
                *why = xstrfmt("%s must be the board's address on GitHub, such as https://github.com/orgs/acme/projects/1/views/2, or empty for none.", FIELDS[f].label);
                free(text); json_free(body);
                return NULL;
            }
            json_object_set(body, key, board);
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
/// A labelled box with its edit, the hint behind a help icon beside the label; advances.
static void field(FormScreen *s, Doc *doc, int x, int w, int f) {
    if (!field_offered(s, f)) return;
    const FieldDef *d = &FIELDS[f];
    bool on = field_enabled(s, f);
    doc_field_label(doc, x, w, d->label, on ? theme.ink : theme.muted, d->hint);
    doc_space(doc, px(6));
    FontId fid = d->mono ? FONT_MONO : FONT_BODY;
    int fh = edit_line_height(fid);
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
    doc_space(doc, px(14));
}

static void paint_check(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    FormScreen *s = it->data;
    int f = (int)it->arg;
    bool hovered = doc_item_hovered(doc, it);
    // A pixel under the row's middle, so the box sits on the label's capitals rather than its line box.
    int size = px(15), top = rc->top + (rc->bottom - rc->top - size) / 2 + px(1);
    RECT b = { rc->left, top, rc->left + size, top + size };
    bool on = s->bools[f];
    fill_round_rect(cv, &b, px(3), on ? theme.accent : theme.field, on ? theme.accent : hovered ? theme.accent_dim : theme.line_strong);
    if (on) draw_check_mark(cv, &b, theme.on_accent);
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
    bool hovered = d->enabled && doc_item_hovered(doc, it);
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
        check(s, doc, x, col, F_ENABLED);
        if (field_offered(s, F_ENABLED) && !s->bools[F_ENABLED])
            note(doc, x, col, "Inactive: no session can be started on it and it is left out of the project lists; its settings are kept for when it is switched back on.");
        doc_space(doc, px(4));
        field(s, doc, x, col, F_LOCAL_DIR);
        field(s, doc, x, col, F_BOARD);
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
        check(s, doc, x, col, F_AUTO_LOOP);
        if (field_offered(s, F_AUTO_LOOP) && s->bools[F_AUTO_LOOP])
            note(doc, x, col, "Every finding of a session's review-loop round, low severity and parked ones too, goes to \xE2\x9A\x99 Implement feedback on its own, and the fix is pushed and reviewed again until a round comes back clean and code-approved. The loop's round limit and repeated-findings check still stop one that cannot converge. Sessions still need their review loop switched on; a standalone \xE2\x8C\x95 Code review keeps its manual findings.");
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
        char *text = request_error_or_unexpected(req);
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
        request_error_into(&s->error, req);
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

// MARK: - The SSH server form

/// An SSH server's boxes, as the web's form has them, each on one of the tabs below. The database login under them is
/// write-only on the server: the row says only whether one is stored (`hasDbCredentials`), so the username is read back
/// through `GET …/db-credentials` and the password box stays empty, sent only when something is typed in it.
enum { S_LABEL, S_HOST, S_PORT, S_USER, S_KEY, S_DBHOST, S_DBPORT, S_DBUSER, S_DBPASS, S_COUNT };
static const FieldDef SSH_FIELDS[S_COUNT] = {
    [S_LABEL] = { "label", K_TEXT, "Label", "Production web server", "Leave empty to name it user@host:port.", 0, false },
    [S_HOST] = { "host", K_TEXT, "Host", "server.example.com", NULL, 0, true },
    [S_PORT] = { "port", K_NUMBER, "Port", "22", NULL, 0, true },
    [S_USER] = { "username", K_TEXT, "Username", "deploy", NULL, 0, true },
    [S_KEY] = { "identityFile", K_TEXT, "Private key path", "/home/you/.ssh/id_ed25519",
        "Absolute path on the machine running Briareus; leave empty for its default SSH keys or agent. Password prompts are not supported.", 0, true },
    [S_DBHOST] = { "dbHost", K_TEXT, "Database host", "127.0.0.1", "Where the database listens, as seen from this server itself.", 0, true },
    [S_DBPORT] = { "dbPort", K_NUMBER, "Database port", "3306", NULL, 0, true },
    [S_DBUSER] = { "dbUsername", K_TEXT, "Database username", "app", "Empty removes the stored login.", 0, true },
    [S_DBPASS] = { "dbPassword", K_TEXT, "Database password", "", NULL, 0, true },
};
/// The two pickers: the project whose sessions may use the server, and whether its commands wait for approval.
enum { SP_REPO, SP_MODE, SP_COUNT };
static const char *const SSH_PICK_KEYS[SP_COUNT] = { "repo", "permissionMode" };

/// The form's cells, two to a row: what the server is, how it is reached, and what it may run unasked.
enum { C_LABEL, C_REPO, C_HOST, C_PORT, C_USER, C_KEY, C_MODE, C_ENABLED, C_DBHOST, C_DBPORT, C_DBUSER, C_DBPASS };
static const char *mode_title(const char *mode) { return str_eq(mode, "allow") ? "Don\xE2\x80\x99t ask anything" : "Ask for all commands"; }

/// The form's tabs: the server itself, and the database login on it. The open one stays open from one server to the next.
enum { ST_SERVER, ST_DATABASE, ST_COUNT };
static const struct { const char *title; wchar_t glyph; } SSH_TABS[ST_COUNT] = {
    [ST_SERVER] = { "SSH server", 0xE968 }, [ST_DATABASE] = { "Database", 0xE1D3 },
};
static int g_ssh_tab;
static int ssh_field_tab(int f) { return f >= S_DBHOST ? ST_DATABASE : ST_SERVER; }

enum { ACT_SSH_SAVE = 1200, ACT_SSH_CLONE, ACT_SSH_DELETE, ACT_SSH_TOGGLE, ACT_SSH_PICK, ACT_SSH_FOCUS, ACT_SSH_TAB };
enum { ID_SSH_FIELD = 2100 };

typedef struct {
    Screen base;
    Json *row;             // what the form was filled from: the saved row, the defaults, or a clone's values
    double id;             // 0 until the server is saved
    HWND edits[S_COUNT];
    RECT rects[S_COUNT];
    bool laid[S_COUNT], clipped[S_COUNT];
    bool enabled;          // Available to sessions on this project
    char *picks[SP_COUNT];
    Request *req_save, *req_delete, *req_login;
    bool dirty, filling, shown, tab_dot;
    int focused, focus_first;
    char *error;
    int error_tab;         // the tab whose field the last refused save was about
    char *db_user;         // the stored database username, as read back; NULL until known
    char *login_error;     // why it could not be read back
} SshForm;

static char *ssh_field_text(const Json *row, int f) {
    const Json *v = json_get(row, SSH_FIELDS[f].key);
    double n;
    if (SSH_FIELDS[f].kind == K_NUMBER) return json_num(v, &n) && isfinite(n) ? xstrfmt("%.10g", n) : xstrdup("");
    return xstrdup(json_str(v) ? json_str(v) : "");
}
/// A box's text as the form was filled: a saved server's database username is the one read back, never in its row.
static char *ssh_text(SshForm *s, int f) {
    if (f == S_DBUSER && !json_str(json_get(s->row, "dbUsername"))) return xstrdup(s->db_user ? s->db_user : "");
    return ssh_field_text(s->row, f);
}
static bool ssh_has_login(SshForm *s) { return s->id && json_bool_is(json_get(s->row, "hasDbCredentials"), true); }
/// Whether the server stores a database login with each SSH server. One from before that drops the fields without a
/// word, so the Database tab is offered only when its catalog names the route that reads a login back.
static bool ssh_db_offered(void) { return store_supports("ssh_db_credentials"); }
/// The password box's cue says whether leaving it empty keeps a stored password.
static void ssh_password_cue(SshForm *s) {
    if (!s->edits[S_DBPASS]) return;
    const char *cue = ssh_has_login(s) ? "stored; leave empty to keep it" : "empty = no password";
    wchar_t *w = utf8_to_wide(cue); SendMessageW(s->edits[S_DBPASS], EM_SETCUEBANNER, TRUE, (LPARAM)w); free(w);
}
/// A picker's value as the row has it saved; a server without a mode asks, as the server's own default does.
static const char *ssh_saved_pick(const Json *row, int p) {
    const char *v = json_str_nonempty(json_get(row, SSH_PICK_KEYS[p]));
    return v ? v : p == SP_MODE ? "ask" : "";
}
static bool ssh_saved_enabled(const Json *row) { return !json_bool_is(json_get(row, "enabled"), false); }

static void ssh_fill(SshForm *s) {
    s->enabled = ssh_saved_enabled(s->row);
    for (int p = 0; p < SP_COUNT; p++) set_string(&s->picks[p], ssh_saved_pick(s->row, p));
    s->filling = true;
    for (int f = 0; f < S_COUNT; f++) {
        if (!s->edits[f]) continue;
        char *text = ssh_text(s, f);
        set_edit_text(s->edits[f], text);
        free(text);
    }
    s->filling = false;
    s->dirty = false;
    ssh_password_cue(s);
}

/// The body a save sends; NULL with `*why` when a field cannot be sent as it is.
static Json *ssh_body(SshForm *s, char **why) {
    s->error_tab = ST_SERVER;
    Json *body = json_is_object(s->row) ? json_clone(s->row) : json_object();
    json_object_remove(body, "id");
    json_object_remove(body, "hasDbCredentials");
    json_object_remove(body, "dbUsername");
    json_object_remove(body, "dbPassword");
    if (!ssh_db_offered()) { json_object_remove(body, "dbHost"); json_object_remove(body, "dbPort"); }
    for (int f = 0; f < S_COUNT; f++) {
        if (!s->edits[f] || (ssh_field_tab(f) == ST_DATABASE && !ssh_db_offered())) continue;
        char *text = edit_text(s->edits[f]), *t = f == S_DBPASS ? xstrdup(text) : str_trim(text);
        free(text);
        if (f == S_DBUSER) {
            // Sent only when it changed: the server keeps the password with a new username, and empty clears the login.
            char *saved = ssh_text(s, f);
            if (!str_eq(t, saved) && (*t || s->db_user || json_str(json_get(s->row, "dbUsername")))) json_set_str(body, "dbUsername", t);
            free(saved);
        } else if (f == S_DBPASS) {
            // Left empty, the stored password stays.
            if (*t) json_set_str(body, "dbPassword", t);
        } else if (f == S_DBPORT && !*t) {
            json_object_remove(body, "dbPort");   // the server's own default, 3306
        } else if (SSH_FIELDS[f].kind == K_NUMBER) {
            char *end; long port = strtol(t, &end, 10);
            if (!*t || *end || port < 1 || port > 65535) {
                *why = xstrdup(f == S_DBPORT ? "Enter a database port from 1 to 65535." : "Enter a port from 1 to 65535.");
                s->error_tab = ssh_field_tab(f);
                free(t); json_free(body);
                return NULL;
            }
            json_set_num(body, SSH_FIELDS[f].key, (double)port);
        } else json_set_str(body, SSH_FIELDS[f].key, t);
        free(t);
    }
    for (int p = 0; p < SP_COUNT; p++) json_set_str(body, SSH_PICK_KEYS[p], s->picks[p] ? s->picks[p] : "");
    json_set_bool(body, "enabled", s->enabled);
    const char *missing = NULL;
    if (str_empty(json_str(json_get(body, "repo")))) missing = "Choose the project whose sessions may use this server.";
    else if (str_empty(json_str(json_get(body, "host")))) missing = "Enter the host: a hostname or an IP address.";
    else if (str_empty(json_str(json_get(body, "username")))) missing = "Enter the username to connect as.";
    else if (json_str_nonempty(json_get(body, "dbPassword"))) {
        // A password goes with a username: the one typed, else the one stored.
        const char *user = json_str(json_get(body, "dbUsername"));
        if (user ? !*user : !s->db_user ? !ssh_has_login(s) : !*s->db_user) { missing = "A database password needs a database username."; s->error_tab = ST_DATABASE; }
    }
    if (missing) { *why = xstrdup(missing); json_free(body); return NULL; }
    return body;
}

/// Whether a tab holds a change not saved yet, for the dot after its title.
static bool ssh_tab_changed(SshForm *s, int tab) {
    if (tab == ST_SERVER) {
        if (s->enabled != ssh_saved_enabled(s->row)) return true;
        for (int p = 0; p < SP_COUNT; p++)
            if (!str_eq(s->picks[p] ? s->picks[p] : "", ssh_saved_pick(s->row, p))) return true;
    }
    for (int f = 0; f < S_COUNT; f++) {
        if (!s->edits[f] || ssh_field_tab(f) != tab) continue;
        char *now = edit_text(s->edits[f]), *saved = ssh_text(s, f);
        bool differs = !str_eq(now, saved);
        free(now); free(saved);
        if (differs) return true;
    }
    return false;
}
static bool ssh_form_changed(SshForm *s) { return ssh_tab_changed(s, ST_SERVER) || ssh_tab_changed(s, ST_DATABASE); }
static void ssh_changed(SshForm *s) {
    if (s->filling) return;
    if (!s->dirty) { s->dirty = true; pane_header_changed(s->base.pane); }
    bool changed = ssh_form_changed(s);
    if (changed != s->tab_dot) { s->tab_dot = changed; pane_relayout(s->base.pane); }
}

// MARK: Layout

static void paint_ssh_box(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    (void)doc;
    SshForm *s = it->data;
    fill_round_rect(cv, rc, px(6), theme.raise, s->focused == (int)it->arg ? theme.accent_dim : theme.line);
}
/// A labelled box with its edit, the hint behind a help icon beside the label; advances.
static void ssh_field(SshForm *s, Doc *doc, int x, int w, int f) {
    const FieldDef *d = &SSH_FIELDS[f];
    doc_field_label(doc, x, w, d->label, theme.ink, d->hint);
    doc_space(doc, px(6));
    FontId fid = d->mono ? FONT_MONO : FONT_BODY;
    int fh = edit_line_height(fid), h = px(36);
    RECT box = { x, doc->y, x + w, doc->y + h };
    Item *it = doc_item(doc, doc_add(doc, &box, paint_ssh_box));
    it->data = s; it->arg = f; it->action = ACT_SSH_FOCUS;
    s->rects[f] = (RECT){ x + px(10), box.top + (h - fh) / 2, x + w - px(10), box.top + (h - fh) / 2 + fh };
    s->laid[f] = true;
    doc->y = box.bottom;
    doc_space(doc, px(14));
}
static void paint_ssh_check(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    SshForm *s = it->data;
    bool hovered = doc_item_hovered(doc, it);
    int size = px(15), top = rc->top + (rc->bottom - rc->top - size) / 2;
    RECT b = { rc->left, top, rc->left + size, top + size };
    fill_round_rect(cv, &b, px(3), s->enabled ? theme.accent : theme.field, s->enabled ? theme.accent : hovered ? theme.accent_dim : theme.line_strong);
    if (s->enabled) draw_glyph(cv, 0xE73E, &b, FONT_ICON_SMALL, theme.on_accent);
    RECT t = { b.right + px(8), rc->top, rc->right, rc->bottom };
    draw_text(cv, "Available to sessions on this project", &t, FONT_FOOTNOTE, theme.ink, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}
/// A labelled picker, as the web's `<select>`; advances.
static void ssh_select(SshForm *s, Doc *doc, int x, int w, int p, const char *label, const char *hint) {
    doc_field_label(doc, x, w, label, theme.ink, hint);
    doc_space(doc, px(6));
    RECT box = { x, doc->y, x + w, doc->y + px(36) };
    Item *it = doc_item(doc, doc_add(doc, &box, paint_select));
    SelectData *d = xcalloc(1, sizeof *d);
    const char *v = s->picks[p];
    d->text = xstrdup(p == SP_MODE ? mode_title(v) : !str_empty(v) ? v : "Choose a project");
    d->enabled = true;
    it->data = d; it->free_data = select_free; it->action = ACT_SSH_PICK; it->arg = p; it->hand = true;
    doc->y = box.bottom;
    doc_space(doc, px(14));
}
static void ssh_cell(SshForm *s, Doc *doc, int x, int w, int cell) {
    switch (cell) {
    case C_LABEL: ssh_field(s, doc, x, w, S_LABEL); break;
    case C_REPO: ssh_select(s, doc, x, w, SP_REPO, "Project", "Only this project's sessions see the server, through their SSH tool."); break;
    case C_HOST: ssh_field(s, doc, x, w, S_HOST); break;
    case C_PORT: ssh_field(s, doc, x, w, S_PORT); break;
    case C_USER: ssh_field(s, doc, x, w, S_USER); break;
    case C_KEY: ssh_field(s, doc, x, w, S_KEY); break;
    case C_ENABLED: {
        // Level with the boxes beside it: below where their label sits, centred on their height.
        doc_space(doc, font_height(doc->cv, FONT_FOOTNOTE) + px(6));
        int i = doc_custom(doc, x, w, px(36), paint_ssh_check, s, NULL, ACT_SSH_TOGGLE, 0);
        doc_item(doc, i)->hand = true;
        doc_space(doc, px(14));
        break;
    }
    case C_MODE: ssh_select(s, doc, x, w, SP_MODE, "Permission mode", "Ask shows the exact command in the dashboard for approval. Don't ask anything sends every command immediately."); break;
    case C_DBHOST: ssh_field(s, doc, x, w, S_DBHOST); break;
    case C_DBPORT: ssh_field(s, doc, x, w, S_DBPORT); break;
    case C_DBUSER: ssh_field(s, doc, x, w, S_DBUSER); break;
    case C_DBPASS: ssh_field(s, doc, x, w, S_DBPASS); break;
    }
}
/// Two cells side by side, each half the width, as the web's `.field-row`; the row is as tall as its taller cell.
static void ssh_row(SshForm *s, Doc *doc, int x, int col, int left, int right) {
    int gap = px(14), half = (col - gap) / 2, top = doc->y;
    ssh_cell(s, doc, x, half, left);
    int bottom = doc->y;
    doc->y = top;
    ssh_cell(s, doc, x + half + gap, col - half - gap, right);
    if (doc->y < bottom) doc->y = bottom;
}

static void ssh_layout(Screen *base, Doc *doc) {
    SshForm *s = (SshForm *)base;
    memset(s->laid, 0, sizeof s->laid);
    int col = doc->width, x = 0;
    doc_space(doc, px(8));
    char *why = ssh_unavailable();
    if (why) { doc_space(doc, px(10)); doc_text(doc, x, col, why, FONT_FOOTNOTE, theme.muted, DT_LEFT | DT_WORDBREAK); free(why); doc_space(doc, px(12)); return; }
    // The tabs, drawn as the other settings forms draw theirs, each with the dot when something on it is not saved yet.
    int h = px(42), tx = x, ty = doc->y;
    s->tab_dot = false;
    for (int t = 0; t < ST_COUNT; t++) {
        bool changed = s->dirty && ssh_tab_changed(s, t);
        s->tab_dot |= changed;
        char *title = changed ? xstrfmt("%s \xE2\x80\xA2", SSH_TABS[t].title) : xstrdup(SSH_TABS[t].title);
        doc_tab(doc, &tx, &ty, x, x + col, h, SSH_TABS[t].glyph, title, NULL, t == g_ssh_tab, ACT_SSH_TAB, t);
        free(title);
    }
    doc->y = ty + h;
    doc_rule(doc, x, col);
    doc_space(doc, px(18));
    if (s->error) { doc_notice_box(doc, x, col, s->error); doc_space(doc, px(16)); }
    if (g_ssh_tab == ST_SERVER) {
        ssh_row(s, doc, x, col, C_LABEL, C_REPO);
        ssh_row(s, doc, x, col, C_HOST, C_PORT);
        ssh_row(s, doc, x, col, C_USER, C_KEY);
        ssh_row(s, doc, x, col, C_MODE, C_ENABLED);
        note(doc, x, col, "First verify the server's host key and add it to the Briareus account's known_hosts file. Unknown or changed host keys are refused. SSH configuration aliases and interactive commands are not supported.");
    } else if (!ssh_db_offered()) {
        doc_text(doc, x, col, "This Briareus server does not store database logins yet: its API has no GET /settings/ssh/servers/{id}/db-credentials. Update the server, then reconnect.", FONT_FOOTNOTE, theme.muted, DT_LEFT | DT_WORDBREAK);
    } else {
        // The server's database, reached through a tunnel over SSH to it.
        note(doc, x, col, "A MySQL login on this server, stored encrypted. Clients reach the database through an SSH tunnel to this server, at the host and port below as the server itself sees them.");
        doc_space(doc, px(8));
        if (s->login_error) { char *text = xstrfmt("The stored login could not be read: %s", s->login_error); doc_notice_box(doc, x, col, text); free(text); doc_space(doc, px(16)); }
        ssh_row(s, doc, x, col, C_DBHOST, C_DBPORT);
        ssh_row(s, doc, x, col, C_DBUSER, C_DBPASS);
        if (s->req_login) note(doc, x, col, "Reading the stored login\xE2\x80\xA6");
        else if (ssh_has_login(s)) note(doc, x, col, "A login is stored. Type a password only to change it.");
    }
    doc_space(doc, px(40));
}

static void ssh_header(Screen *base, HeaderInfo *info) {
    SshForm *s = (SshForm *)base;
    const char *label = json_str_nonempty(json_get(s->row, "label")), *host = json_str_nonempty(json_get(s->row, "host"));
    const char *user = json_str_nonempty(json_get(s->row, "username")), *repo = json_str_nonempty(json_get(s->row, "repo"));
    if (s->id) {
        snprintf(info->title, sizeof info->title, "%s", label ? label : host ? host : "SSH server");
        char *port = ssh_field_text(s->row, S_PORT);
        snprintf(info->subtitle, sizeof info->subtitle, "%s%s%s:%s \xC2\xB7 %s", user ? user : "", user ? "@" : "", host ? host : "", port, repo ? repo : "");
        free(port);
    } else {
        snprintf(info->title, sizeof info->title, "New SSH server");
        snprintf(info->subtitle, sizeof info->subtitle, "A server a project's agents may run commands on, with approval.");
    }
    if (!store_supports("settings_ssh_servers")) return;
    bool busy = s->req_save || s->req_delete;
    HeaderButton *b = &info->buttons[info->button_count++];
    snprintf(b->label, sizeof b->label, "%s", s->req_save ? "Saving\xE2\x80\xA6" : "Save");
    b->glyph = 0xE74E; b->action = ACT_SSH_SAVE; b->prominent = true; b->tip = "Save this SSH server (Ctrl+S)";
    b->enabled = !busy && (s->dirty || !s->id) && store_supports(s->id ? "update_ssh_server" : "create_ssh_server");
    if (!s->id) return;
    HeaderButton *c = &info->buttons[info->button_count++];
    c->glyph = 0xE8C8; c->action = ACT_SSH_CLONE; c->enabled = !busy && store_supports("create_ssh_server"); c->tip = "Clone into a new SSH server";
    HeaderButton *d = &info->buttons[info->button_count++];
    d->glyph = 0xE74D; d->action = ACT_SSH_DELETE; d->destructive = true; d->enabled = !busy && store_supports("delete_ssh_server"); d->tip = "Delete this SSH server";
}

// MARK: The edits

static void ssh_place(Screen *base, const RECT *content, int scroll_y) {
    SshForm *s = (SshForm *)base;
    int m = margin_of(base->pane);
    for (int f = 0; f < S_COUNT; f++) {
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

static void ssh_save(SshForm *s);
static int ssh_next_field(SshForm *s, int from, int step) {
    for (int k = 1; k <= S_COUNT; k++) {
        int f = ((from + step * k) % S_COUNT + S_COUNT) % S_COUNT;
        if (s->edits[f] && s->laid[f]) return f;
    }
    return from;
}
static LRESULT CALLBACK ssh_field_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref) {
    SshForm *s = (SshForm *)ref;
    int f = (int)id;
    bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    switch (msg) {
    case WM_KEYDOWN:
        if (wp == VK_TAB) { SetFocus(s->edits[ssh_next_field(s, f, (GetKeyState(VK_SHIFT) & 0x8000) ? -1 : 1)]); return 0; }
        if (ctrl && wp == 'S') { ssh_save(s); return 0; }
        if (ctrl && wp == 'A') { SendMessageW(hwnd, EM_SETSEL, 0, -1); return 0; }
        if (wp == VK_ESCAPE) { SetFocus(GetParent(hwnd)); return 0; }
        if (wp == VK_RETURN) { SetFocus(s->edits[ssh_next_field(s, f, 1)]); return 0; }
        break;
    case WM_CHAR:
        if (wp == '\t' || wp == 0x13 || wp == 0x01 || wp == 0x1B || wp == '\r') return 0;
        break;
    case WM_MOUSEWHEEL: SendMessageW(GetParent(hwnd), msg, wp, lp); return 0;
    case WM_NCDESTROY: RemoveWindowSubclass(hwnd, ssh_field_proc, id); break;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}
static void ssh_ensure_controls(SshForm *s) {
    if (s->edits[0]) return;
    HWND owner = pane_hwnd(s->base.pane);
    for (int f = 0; f < S_COUNT; f++) {
        DWORD style = WS_CHILD | WS_TABSTOP | ES_AUTOHSCROLL | (SSH_FIELDS[f].kind == K_NUMBER ? ES_NUMBER : 0) | (f == S_DBPASS ? ES_PASSWORD : 0);
        HWND e = CreateWindowExW(0, L"EDIT", L"", style, 0, 0, 10, 10, owner, (HMENU)(INT_PTR)(ID_SSH_FIELD + f), GetModuleHandleW(NULL), NULL);
        SendMessageW(e, WM_SETFONT, (WPARAM)font(SSH_FIELDS[f].mono ? FONT_MONO : FONT_BODY), TRUE);
        SendMessageW(e, EM_SETLIMITTEXT, SSH_FIELDS[f].kind == K_NUMBER ? 5 : 1024, 0);
        wchar_t *w = utf8_to_wide(SSH_FIELDS[f].cue); SendMessageW(e, EM_SETCUEBANNER, TRUE, (LPARAM)w); free(w);
        SetWindowSubclass(e, ssh_field_proc, (UINT_PTR)f, (DWORD_PTR)s);
        theme_apply_control(e);
        s->edits[f] = e;
    }
    // The edits take the row's text; what was picked or ticked before they existed stays as it is.
    s->filling = true;
    for (int f = 0; f < S_COUNT; f++) { char *text = ssh_text(s, f); set_edit_text(s->edits[f], text); free(text); }
    s->filling = false;
    ssh_password_cue(s);
}

/// The stored database username, read back so the box shows it; the password that comes with it is not kept.
static void ssh_login_done(void *owner, Request *req) {
    SshForm *s = owner;
    const Json *login = req->ok ? json_get(req->result, "credentials") : NULL;
    if (!json_is_object(login)) { request_error_into(&s->login_error, req); pane_relayout(s->base.pane); return; }
    set_string(&s->login_error, NULL);
    const char *user = json_str(json_get(login, "username"));
    // The box takes it unless something was typed in it meanwhile.
    char *now = s->edits[S_DBUSER] ? edit_text(s->edits[S_DBUSER]) : xstrdup("");
    bool untouched = !*now;
    free(now);
    set_string(&s->db_user, user ? user : "");
    if (untouched && s->edits[S_DBUSER]) { s->filling = true; set_edit_text(s->edits[S_DBUSER], s->db_user); s->filling = false; }
    // Filling the box is no edit; one typed meanwhile is now measured against the username read back.
    if (s->dirty) ssh_changed(s);
    pane_relayout(s->base.pane);
}
static void ssh_load_login(SshForm *s) {
    if (s->db_user || s->req_login || s->login_error || !ssh_has_login(s) || !store_supports("ssh_db_credentials")) return;
    Json *args = json_object(); json_set_num(args, "id", s->id);
    store_call("ssh_db_credentials", args, 0, s, ssh_login_done, 0, &s->req_login);
}

/// The project or permission mode picker, as a menu under the pointer.
static void ssh_pick(SshForm *s, int p, POINT pt) {
    HMENU m = CreatePopupMenu();
    const char *now = s->picks[p] ? s->picks[p] : "";
    const Json *rows = g_settings ? settings_rows(g_settings) : NULL;
    size_t n = 0;
    if (p == SP_MODE) {
        append_item(m, 1, mode_title("ask"), str_eq(now, "ask"), true);
        append_item(m, 2, mode_title("allow"), str_eq(now, "allow"), true);
    } else {
        n = json_count(rows);
        bool listed = false;
        for (size_t i = 0; i < n; i++) {
            const char *repo = json_str(json_get(json_at(rows, i), "repo"));
            if (!repo) continue;
            listed |= str_eq(repo, now);
            append_item(m, (UINT)i + 1, repo, str_eq(repo, now), true);
        }
        // A project that is gone stays picked rather than being swapped for another; the server refuses it on save.
        if (*now && !listed) { char *label = xstrfmt("%s (not a project)", now); append_item(m, 9999, label, true, false); free(label); }
        if (!n && !*now) append_item(m, 9999, "No projects yet", false, false);
    }
    int chosen = TrackPopupMenu(m, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, pt.x, pt.y, 0, pane_hwnd(s->base.pane), NULL);
    DestroyMenu(m);
    const char *value = NULL;
    if (p == SP_MODE && (chosen == 1 || chosen == 2)) value = chosen == 1 ? "ask" : "allow";
    else if (p == SP_REPO && chosen >= 1 && (size_t)chosen - 1 < n) value = json_str(json_get(json_at(rows, (size_t)chosen - 1), "repo"));
    if (value && !str_eq(value, now)) { set_string(&s->picks[p], value); ssh_changed(s); pane_relayout(s->base.pane); }
}

// MARK: Saving, cloning, deleting

static void ssh_show_error(SshForm *s, char *text) {
    set_string(&s->error, text); free(text);
    g_ssh_tab = s->error_tab;
    pane_relayout(s->base.pane); pane_header_changed(s->base.pane); pane_scroll_to_top(s->base.pane);
}
static void ssh_save_done(void *owner, Request *req) {
    SshForm *s = owner;
    const Json *row = req->ok ? json_get(req->result, "server") : NULL;
    if (!json_is_object(row)) {
        char *text = request_error_or_unexpected(req);
        // The server's own words, on the tab in use; one about the database login or its key opens the Database tab.
        s->error_tab = str_icontains(text, "database") || str_icontains(text, "credential") ? ST_DATABASE : g_ssh_tab;
        ssh_show_error(s, text);
        return;
    }
    // The server's word on what was saved: an empty label is now user@host:port, and a new server has an id.
    // The username just sent is now the stored one; a server without a login has none.
    const Json *sent = json_get(req->args, "dbUsername");
    if (!json_bool_is(json_get(row, "hasDbCredentials"), true)) set_string(&s->db_user, "");
    else if (json_str(sent)) set_string(&s->db_user, json_str(sent));
    else if (!s->db_user && !s->id) set_string(&s->db_user, "");
    json_free(s->row); s->row = json_clone(row);
    set_string(&s->error, NULL);
    s->id = ssh_row_id(row);
    free(s->base.id); s->base.id = ssh_form_id(s->id);
    pane_set_selected_id(app_sidebar_pane(), s->base.id);
    ssh_fill(s);
    // A server that answers without the login's fields did not keep it: say so rather than show an empty form as saved.
    bool sent_login = json_str(json_get(req->args, "dbUsername")) || json_str(json_get(req->args, "dbPassword"));
    const Json *has = json_get(row, "hasDbCredentials");
    if (sent_login && !json_bool_is(has, true) && !json_bool_is(has, false)) {
        s->error_tab = ST_DATABASE;
        ssh_show_error(s, xstrdup("The server saved the SSH server but not its database login: it does not store database logins yet. Update the Briareus server."));
    }
    pane_relayout(s->base.pane); pane_header_changed(s->base.pane);
    settings_ssh_changed();
}
static void ssh_save(SshForm *s) {
    if (s->req_save || s->req_delete || !store_supports(s->id ? "update_ssh_server" : "create_ssh_server")) return;
    char *why = NULL;
    Json *body = ssh_body(s, &why);
    if (!body) { ssh_show_error(s, why); return; }
    if (s->id) json_set_num(body, "id", s->id);
    store_call(s->id ? "update_ssh_server" : "create_ssh_server", body, 0, s, ssh_save_done, 0, &s->req_save);
    pane_header_changed(s->base.pane);
}
static void ssh_delete_done(void *owner, Request *req) {
    SshForm *s = owner;
    if (!req->ok) { s->error_tab = g_ssh_tab; ssh_show_error(s, request_error_text(req)); return; }
    s->dirty = false;
    app_clear_detail();
    settings_ssh_changed();
}
static void ssh_delete(SshForm *s) {
    if (!s->id || s->req_save || s->req_delete) return;
    const char *name = json_str_nonempty(json_get(s->row, "label"));
    char *title = xstrfmt("Delete %s?", name ? name : "this SSH server");
    bool ok = app_confirm(title, "Sessions lose access through the SSH tool and pending approvals are cancelled.", "Delete", true);
    free(title);
    if (!ok) return;
    Json *args = json_object(); json_set_num(args, "id", s->id);
    store_call("delete_ssh_server", args, 0, s, ssh_delete_done, 0, &s->req_delete);
    pane_header_changed(s->base.pane);
}
static void ssh_clone(SshForm *s) {
    char *why = NULL;
    Json *copy = ssh_body(s, &why);
    if (!copy) { ssh_show_error(s, why); return; }
    // The copy carries what the form holds now, saved or not, without the label that named this one. The database
    // username goes with it; a stored password is never read back, so only one typed here does.
    json_set_str(copy, "label", "");
    if (s->edits[S_DBUSER]) { char *text = edit_text(s->edits[S_DBUSER]), *t = str_trim(text); json_set_str(copy, "dbUsername", t); free(text); free(t); }
    s->dirty = false;
    Screen *clone = ssh_settings_screen_new(copy, NULL);
    ((SshForm *)clone)->focus_first = S_LABEL;
    g_ssh_tab = ST_SERVER;
    json_free(copy);
    app_show_detail(clone);
}

// MARK: The screen

static void ssh_destroy(Screen *base) {
    SshForm *s = (SshForm *)base;
    request_cancel(&s->req_save); request_cancel(&s->req_delete); request_cancel(&s->req_login);
    for (int f = 0; f < S_COUNT; f++) if (s->edits[f]) DestroyWindow(s->edits[f]);
    for (int p = 0; p < SP_COUNT; p++) free(s->picks[p]);
    json_free(s->row); free(s->error); free(s->db_user); free(s->login_error);
    screen_release(base);
}
static void ssh_action(Screen *base, int action, intptr_t arg, POINT pt) {
    SshForm *s = (SshForm *)base;
    switch (action) {
    case ACT_SSH_SAVE: ssh_save(s); break;
    case ACT_SSH_CLONE: ssh_clone(s); break;
    case ACT_SSH_DELETE: ssh_delete(s); break;
    case ACT_SSH_TOGGLE: s->enabled = !s->enabled; ssh_changed(s); pane_relayout(base->pane); break;
    case ACT_SSH_PICK: if (arg >= 0 && arg < SP_COUNT) ssh_pick(s, (int)arg, pt); break;
    case ACT_SSH_FOCUS: if (arg >= 0 && arg < S_COUNT && s->edits[arg]) SetFocus(s->edits[arg]); break;
    case ACT_SSH_TAB:
        if (arg < 0 || arg >= ST_COUNT) break;
        if (s->focused >= 0) SetFocus(pane_hwnd(base->pane));
        g_ssh_tab = (int)arg;
        pane_relayout(base->pane); pane_scroll_to_top(base->pane);
        break;
    }
}
static void ssh_command(Screen *base, int id, int code, HWND control) {
    (void)control;
    SshForm *s = (SshForm *)base;
    int f = id - ID_SSH_FIELD;
    if (f < 0 || f >= S_COUNT || !s->edits[f]) return;
    switch (code) {
    case EN_CHANGE: ssh_changed(s); break;
    case EN_SETFOCUS: {
        s->focused = f;
        RECT content = pane_content_rect(base->pane);
        int top = s->rects[f].top - px(40), bottom = s->rects[f].bottom + px(16), y = pane_scroll_y(base->pane);
        if (top < y || bottom > y + (content.bottom - content.top)) pane_scroll_to(base->pane, top);
        pane_repaint(base->pane);
        break;
    }
    case EN_KILLFOCUS: if (s->focused == f) s->focused = -1; pane_repaint(base->pane); break;
    }
}
static bool ssh_key(Screen *base, WPARAM vk, bool ctrl, bool shift) {
    (void)shift;
    if (ctrl && vk == 'S') { ssh_save((SshForm *)base); return true; }
    return false;
}
static void ssh_visible(Screen *base, bool shown) {
    SshForm *s = (SshForm *)base;
    s->shown = shown;
    if (shown) {
        ssh_ensure_controls(s);
        ssh_load_login(s);
        if (s->focus_first >= 0) { int f = s->focus_first; s->focus_first = -1; SetFocus(s->edits[f]); }
    } else for (int f = 0; f < S_COUNT; f++) if (s->edits[f]) ShowWindow(s->edits[f], SW_HIDE);
}
static bool ssh_can_leave(Screen *base) {
    SshForm *s = (SshForm *)base;
    if (!s->dirty) return true;
    const char *name = json_str_nonempty(json_get(s->row, "label"));
    char *message = s->id ? xstrfmt("The changes to %s have not been saved.", name ? name : "this SSH server") : xstrdup("The new SSH server has not been saved.");
    bool leave = app_confirm("Discard unsaved changes?", message, "Discard", true);
    free(message);
    if (leave) s->dirty = false;
    return leave;
}

static const ScreenVTable ssh_vt = {
    .destroy = ssh_destroy, .layout = ssh_layout, .header = ssh_header, .action = ssh_action, .place = ssh_place,
    .visible = ssh_visible, .command = ssh_command, .key = ssh_key, .can_leave = ssh_can_leave,
};
static Screen *ssh_settings_screen_new(const Json *row, const Json *defaults) {
    SshForm *s = xcalloc(1, sizeof *s);
    s->base.vt = &ssh_vt;
    s->focused = -1; s->focus_first = -1;
    s->row = json_is_object(row) ? json_clone(row) : json_is_object(defaults) ? json_clone(defaults) : json_object();
    s->id = ssh_row_id(s->row);
    s->base.id = ssh_form_id(s->id);
    if (!s->id) {
        // A new server starts on the first project, as the web's form does, and where its label is typed.
        if (!json_str_nonempty(json_get(s->row, "repo")) && g_settings) {
            const char *first = json_str_nonempty(json_get(json_at(settings_rows(g_settings), 0), "repo"));
            if (first) json_set_str(s->row, "repo", first);
        }
        if (!json_get(s->row, "port")) json_set_num(s->row, "port", 22);
        if (!row) { s->focus_first = S_LABEL; g_ssh_tab = ST_SERVER; }
    }
    ssh_fill(s);
    return &s->base;
}

// MARK: - The Forge account form

/// A Forge account's boxes. The token is write-only: the server never sends it back, so its box starts empty and a save
/// with it empty keeps the stored one.
enum { G_LABEL, G_ORG, G_TOKEN, G_COUNT };
static const FieldDef FORGE_FIELDS[G_COUNT] = {
    [G_LABEL] = { "label", K_TEXT, "Label", "Acme production", "Leave empty to name it after the organization.", 0, false },
    [G_ORG] = { "organization", K_TEXT, "Organization", "acme", "The slug in your Forge URLs: forge.laravel.com/<organization>/\xE2\x80\xA6", 0, true },
    [G_TOKEN] = { "token", K_TEXT, "API token", "Paste a Forge API token",
        "Create one in Forge under your profile's API tokens. The server stores it encrypted and never sends it back.", 0, true },
};

enum { ACT_FORGE_SAVE = 1300, ACT_FORGE_DELETE, ACT_FORGE_REPO, ACT_FORGE_FOCUS };
enum { ID_FORGE_FIELD = 2200 };

typedef struct {
    Screen base;
    Json *row;             // what the form was filled from: the saved row or the defaults
    double id;             // 0 until the account is saved
    HWND edits[G_COUNT];
    RECT rects[G_COUNT];
    bool laid[G_COUNT], clipped[G_COUNT];
    Json *repos;           // the projects ticked, as `owner/name`
    Request *req_save, *req_delete;
    bool dirty, filling, shown, tab_dot;
    int focused, focus_first;
    char *error;
} ForgeForm;

static bool forge_has_token(const ForgeForm *s) { return json_bool_is(json_get(s->row, "hasToken"), true); }
static char *forge_field_text(const Json *row, int f) {
    if (f == G_TOKEN) return xstrdup("");
    const char *v = json_str(json_get(row, FORGE_FIELDS[f].key));
    return xstrdup(v ? v : "");
}
static bool repos_has(const Json *repos, const char *repo) {
    for (size_t i = 0; i < json_count(repos); i++) if (str_eq(json_str(json_at(repos, i)), repo)) return true;
    return false;
}
/// Whether two project lists hold the same projects, in any order.
static bool repos_same(const Json *a, const Json *b) {
    if (json_count(a) != json_count(b)) return false;
    for (size_t i = 0; i < json_count(a); i++) if (!repos_has(b, json_str(json_at(a, i)))) return false;
    return true;
}
/// What the Projects list offers: every project, then any ticked one that is no longer a project (the server refuses it
/// on save, so it stays visible to be unticked).
static Json *forge_choices(ForgeForm *s) {
    Json *out = json_array();
    const Json *rows = g_settings ? settings_rows(g_settings) : NULL;
    for (size_t i = 0; i < json_count(rows); i++) {
        const char *repo = json_str_nonempty(json_get(json_at(rows, i), "repo"));
        if (repo && !repos_has(out, repo)) json_array_push(out, json_string(repo));
    }
    for (size_t i = 0; i < json_count(s->repos); i++) {
        const char *repo = json_str(json_at(s->repos, i));
        if (repo && !repos_has(out, repo)) json_array_push(out, json_string(repo));
    }
    return out;
}

/// The edits take the row's text, and the token's box says whether one is stored.
static void forge_fill_edits(ForgeForm *s) {
    s->filling = true;
    for (int f = 0; f < G_COUNT; f++) {
        if (!s->edits[f]) continue;
        char *text = forge_field_text(s->row, f);
        set_edit_text(s->edits[f], text);
        free(text);
    }
    if (s->edits[G_TOKEN]) {
        wchar_t *w = utf8_to_wide(forge_has_token(s) ? "Stored \xC2\xB7 type a new token to replace it" : FORGE_FIELDS[G_TOKEN].cue);
        SendMessageW(s->edits[G_TOKEN], EM_SETCUEBANNER, TRUE, (LPARAM)w);
        free(w);
    }
    s->filling = false;
}
static void forge_fill(ForgeForm *s) {
    json_free(s->repos);
    s->repos = json_is_array(json_get(s->row, "repos")) ? json_clone(json_get(s->row, "repos")) : json_array();
    forge_fill_edits(s);
    s->dirty = false;
}

/// The body a save sends; NULL with `*why` when the form cannot be sent as it is.
static Json *forge_body(ForgeForm *s, char **why) {
    Json *body = json_object();
    for (int f = 0; f < G_COUNT; f++) {
        char *text = s->edits[f] ? edit_text(s->edits[f]) : forge_field_text(s->row, f), *t = str_trim(text);
        free(text);
        // An empty token is left out, so the stored one stays.
        if (f != G_TOKEN || *t) json_set_str(body, FORGE_FIELDS[f].key, t);
        free(t);
    }
    json_object_set(body, "repos", json_clone(s->repos));
    const char *missing = NULL;
    if (str_empty(json_str(json_get(body, "organization")))) missing = "Enter the organization slug from your Forge URLs.";
    else if (!forge_has_token(s) && str_empty(json_str(json_get(body, "token")))) missing = "Paste a Forge API token for this organization.";
    if (missing) { *why = xstrdup(missing); json_free(body); return NULL; }
    return body;
}

/// Whether the form holds a change not saved yet, for the dot after the tab's title.
static bool forge_form_changed(ForgeForm *s) {
    if (!repos_same(s->repos, json_get(s->row, "repos"))) return true;
    for (int f = 0; f < G_COUNT; f++) {
        if (!s->edits[f]) continue;
        char *now = edit_text(s->edits[f]), *saved = forge_field_text(s->row, f);
        bool differs = !str_eq(now, saved);
        free(now); free(saved);
        if (differs) return true;
    }
    return false;
}
static void forge_changed(ForgeForm *s) {
    if (s->filling) return;
    if (!s->dirty) { s->dirty = true; pane_header_changed(s->base.pane); }
    bool changed = forge_form_changed(s);
    if (changed != s->tab_dot) { s->tab_dot = changed; pane_relayout(s->base.pane); }
}

// MARK: Layout

static void paint_forge_box(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    (void)doc;
    ForgeForm *s = it->data;
    fill_round_rect(cv, rc, px(6), theme.raise, s->focused == (int)it->arg ? theme.accent_dim : theme.line);
}
/// A labelled box with its edit, the hint behind a help icon beside the label; advances.
static void forge_field(ForgeForm *s, Doc *doc, int x, int w, int f) {
    const FieldDef *d = &FORGE_FIELDS[f];
    doc_field_label(doc, x, w, d->label, theme.ink, d->hint);
    doc_space(doc, px(6));
    FontId fid = d->mono ? FONT_MONO : FONT_BODY;
    int fh = edit_line_height(fid), h = px(36);
    RECT box = { x, doc->y, x + w, doc->y + h };
    Item *it = doc_item(doc, doc_add(doc, &box, paint_forge_box));
    it->data = s; it->arg = f; it->action = ACT_FORGE_FOCUS;
    s->rects[f] = (RECT){ x + px(10), box.top + (h - fh) / 2, x + w - px(10), box.top + (h - fh) / 2 + fh };
    s->laid[f] = true;
    doc->y = box.bottom;
    doc_space(doc, px(14));
}
typedef struct { char *text; bool on, gone; } ForgeCheck;
static void forge_check_free(void *p) { ForgeCheck *d = p; free(d->text); free(d); }
static void paint_forge_check(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    ForgeCheck *d = it->data;
    bool hovered = doc_item_hovered(doc, it);
    int size = px(15), top = rc->top + (rc->bottom - rc->top - size) / 2;
    RECT b = { rc->left, top, rc->left + size, top + size };
    fill_round_rect(cv, &b, px(3), d->on ? theme.accent : theme.field, d->on ? theme.accent : hovered ? theme.accent_dim : theme.line_strong);
    if (d->on) draw_glyph(cv, 0xE73E, &b, FONT_ICON_SMALL, theme.on_accent);
    RECT t = { b.right + px(8), rc->top, rc->right, rc->bottom };
    draw_text(cv, d->text, &t, FONT_FOOTNOTE, d->gone ? theme.muted : theme.ink, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}
/// The projects the account is available to, one tick box each, as the web form's multiple select.
static void forge_projects(ForgeForm *s, Doc *doc, int x, int w) {
    doc_field_label(doc, x, w, "Projects", theme.ink, "Only these projects offer the account's Forge servers and sites to their clients.");
    doc_space(doc, px(6));
    Json *choices = forge_choices(s);
    const Json *rows = g_settings ? settings_rows(g_settings) : NULL;
    for (size_t i = 0; i < json_count(choices); i++) {
        const char *repo = json_str(json_at(choices, i));
        bool listed = false;
        for (size_t k = 0; k < json_count(rows); k++) listed |= str_eq(json_str(json_get(json_at(rows, k), "repo")), repo);
        ForgeCheck *d = xcalloc(1, sizeof *d);
        d->on = repos_has(s->repos, repo); d->gone = !listed;
        d->text = listed ? xstrdup(repo) : xstrfmt("%s (not a project)", repo);
        int it = doc_custom(doc, x, w, px(28), paint_forge_check, d, forge_check_free, ACT_FORGE_REPO, (intptr_t)i);
        doc_item(doc, it)->hand = true;
    }
    if (!json_count(choices)) doc_text(doc, x, w, "No projects yet. Add one under Projects first.", FONT_FOOTNOTE, theme.muted, DT_LEFT | DT_WORDBREAK);
    json_free(choices);
    doc_space(doc, px(14));
}

static void forge_layout(Screen *base, Doc *doc) {
    ForgeForm *s = (ForgeForm *)base;
    memset(s->laid, 0, sizeof s->laid);
    int col = doc->width, x = 0;
    doc_space(doc, px(8));
    int h = px(42), tx = x, ty = doc->y;
    s->tab_dot = s->dirty && forge_form_changed(s);
    doc_tab(doc, &tx, &ty, x, x + col, h, 0xE753, s->tab_dot ? "Forge account \xE2\x80\xA2" : "Forge account", NULL, true, 0, 0);
    doc->y = ty + h;
    doc_rule(doc, x, col);
    doc_space(doc, px(18));
    if (s->error) { doc_notice_box(doc, x, col, s->error); doc_space(doc, px(16)); }
    // Label and organization side by side, as the SSH form's `.field-row`; then the token and the projects across.
    int gap = px(14), half = (col - gap) / 2, top = doc->y;
    forge_field(s, doc, x, half, G_LABEL);
    int bottom = doc->y;
    doc->y = top;
    forge_field(s, doc, x + half + gap, col - half - gap, G_ORG);
    if (doc->y < bottom) doc->y = bottom;
    forge_field(s, doc, x, col, G_TOKEN);
    forge_projects(s, doc, x, col);
    doc_space(doc, px(40));
}

static void forge_header(Screen *base, HeaderInfo *info) {
    ForgeForm *s = (ForgeForm *)base;
    const char *label = json_str_nonempty(json_get(s->row, "label")), *org = json_str_nonempty(json_get(s->row, "organization"));
    if (s->id) {
        snprintf(info->title, sizeof info->title, "%s", label ? label : org ? org : "Forge account");
        size_t n = json_count(json_get(s->row, "repos"));
        snprintf(info->subtitle, sizeof info->subtitle, "forge.laravel.com/%s \xC2\xB7 %zu project%s%s", org ? org : "", n, n == 1 ? "" : "s",
                 forge_has_token(s) ? "" : " \xC2\xB7 no token");
    } else {
        snprintf(info->title, sizeof info->title, "New Forge account");
        snprintf(info->subtitle, sizeof info->subtitle, "A Laravel Forge organization, and the projects whose clients may use it.");
    }
    bool busy = s->req_save || s->req_delete;
    HeaderButton *b = &info->buttons[info->button_count++];
    snprintf(b->label, sizeof b->label, "%s", s->req_save ? "Saving\xE2\x80\xA6" : "Save");
    b->glyph = 0xE74E; b->action = ACT_FORGE_SAVE; b->prominent = true; b->tip = "Save this Forge account (Ctrl+S)";
    b->enabled = !busy && (s->dirty || !s->id) && store_supports(s->id ? "update_forge_account" : "create_forge_account");
    if (!s->id) return;
    HeaderButton *d = &info->buttons[info->button_count++];
    d->glyph = 0xE74D; d->action = ACT_FORGE_DELETE; d->destructive = true; d->enabled = !busy && store_supports("delete_forge_account"); d->tip = "Delete this Forge account";
}

// MARK: The edits

static void forge_place(Screen *base, const RECT *content, int scroll_y) {
    ForgeForm *s = (ForgeForm *)base;
    int m = margin_of(base->pane);
    for (int f = 0; f < G_COUNT; f++) {
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

static void forge_save(ForgeForm *s);
static LRESULT CALLBACK forge_field_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref) {
    ForgeForm *s = (ForgeForm *)ref;
    int f = (int)id;
    bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    switch (msg) {
    case WM_KEYDOWN:
        if (wp == VK_TAB) { SetFocus(s->edits[(f + ((GetKeyState(VK_SHIFT) & 0x8000) ? G_COUNT - 1 : 1)) % G_COUNT]); return 0; }
        if (ctrl && wp == 'S') { forge_save(s); return 0; }
        if (ctrl && wp == 'A') { SendMessageW(hwnd, EM_SETSEL, 0, -1); return 0; }
        if (wp == VK_ESCAPE) { SetFocus(GetParent(hwnd)); return 0; }
        if (wp == VK_RETURN) { SetFocus(s->edits[(f + 1) % G_COUNT]); return 0; }
        break;
    case WM_CHAR:
        if (wp == '\t' || wp == 0x13 || wp == 0x01 || wp == 0x1B || wp == '\r') return 0;
        break;
    case WM_MOUSEWHEEL: SendMessageW(GetParent(hwnd), msg, wp, lp); return 0;
    case WM_NCDESTROY: RemoveWindowSubclass(hwnd, forge_field_proc, id); break;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}
static void forge_ensure_controls(ForgeForm *s) {
    if (s->edits[0]) return;
    HWND owner = pane_hwnd(s->base.pane);
    for (int f = 0; f < G_COUNT; f++) {
        DWORD style = WS_CHILD | WS_TABSTOP | ES_AUTOHSCROLL | (f == G_TOKEN ? ES_PASSWORD : 0);
        HWND e = CreateWindowExW(0, L"EDIT", L"", style, 0, 0, 10, 10, owner, (HMENU)(INT_PTR)(ID_FORGE_FIELD + f), GetModuleHandleW(NULL), NULL);
        SendMessageW(e, WM_SETFONT, (WPARAM)font(FORGE_FIELDS[f].mono ? FONT_MONO : FONT_BODY), TRUE);
        SendMessageW(e, EM_SETLIMITTEXT, f == G_TOKEN ? 4096 : 200, 0);
        wchar_t *w = utf8_to_wide(FORGE_FIELDS[f].cue); SendMessageW(e, EM_SETCUEBANNER, TRUE, (LPARAM)w); free(w);
        SetWindowSubclass(e, forge_field_proc, (UINT_PTR)f, (DWORD_PTR)s);
        theme_apply_control(e);
        s->edits[f] = e;
    }
    // The edits take the row's text; the projects ticked before they existed stay as they are.
    forge_fill_edits(s);
}

static void forge_toggle_repo(ForgeForm *s, size_t index) {
    Json *choices = forge_choices(s);
    const char *repo = json_str(json_at(choices, index));
    if (repo) {
        Json *next = json_array();
        for (size_t i = 0; i < json_count(s->repos); i++) {
            const char *r = json_str(json_at(s->repos, i));
            if (r && !str_eq(r, repo)) json_array_push(next, json_string(r));
        }
        if (!repos_has(s->repos, repo)) json_array_push(next, json_string(repo));
        json_free(s->repos); s->repos = next;
        forge_changed(s);
        pane_relayout(s->base.pane);
    }
    json_free(choices);
}

// MARK: Saving, deleting

static void forge_show_error(ForgeForm *s, char *text) {
    set_string(&s->error, text); free(text);
    pane_relayout(s->base.pane); pane_header_changed(s->base.pane); pane_scroll_to_top(s->base.pane);
}
static void forge_save_done(void *owner, Request *req) {
    ForgeForm *s = owner;
    const Json *row = req->ok ? json_get(req->result, "account") : NULL;
    if (!json_is_object(row)) {
        char *text = request_error_or_unexpected(req);
        forge_show_error(s, text);
        return;
    }
    // The server's word on what was saved: an empty label is now the organization, and a new account has an id.
    json_free(s->row); s->row = json_clone(row);
    set_string(&s->error, NULL);
    s->id = ssh_row_id(row);
    free(s->base.id); s->base.id = forge_form_id(s->id);
    pane_set_selected_id(app_sidebar_pane(), s->base.id);
    forge_fill(s);
    pane_relayout(s->base.pane); pane_header_changed(s->base.pane);
    settings_forge_changed();
}
static void forge_save(ForgeForm *s) {
    if (s->req_save || s->req_delete || !store_supports(s->id ? "update_forge_account" : "create_forge_account")) return;
    char *why = NULL;
    Json *body = forge_body(s, &why);
    if (!body) { forge_show_error(s, why); return; }
    if (s->id) json_set_num(body, "id", s->id);
    store_call(s->id ? "update_forge_account" : "create_forge_account", body, 0, s, forge_save_done, 0, &s->req_save);
    pane_header_changed(s->base.pane);
}
static void forge_delete_done(void *owner, Request *req) {
    ForgeForm *s = owner;
    if (!req->ok) { forge_show_error(s, request_error_text(req)); return; }
    s->dirty = false;
    app_clear_detail();
    settings_forge_changed();
}
static void forge_delete(ForgeForm *s) {
    if (!s->id || s->req_save || s->req_delete) return;
    const char *name = json_str_nonempty(json_get(s->row, "label"));
    char *title = xstrfmt("Delete %s?", name ? name : "this Forge account");
    bool ok = app_confirm(title, "Its stored token is removed and clients can no longer reach the organization's Forge servers and sites through it.", "Delete", true);
    free(title);
    if (!ok) return;
    Json *args = json_object(); json_set_num(args, "id", s->id);
    store_call("delete_forge_account", args, 0, s, forge_delete_done, 0, &s->req_delete);
    pane_header_changed(s->base.pane);
}

// MARK: The screen

static void forge_destroy(Screen *base) {
    ForgeForm *s = (ForgeForm *)base;
    request_cancel(&s->req_save); request_cancel(&s->req_delete);
    for (int f = 0; f < G_COUNT; f++) if (s->edits[f]) DestroyWindow(s->edits[f]);
    json_free(s->repos); json_free(s->row); free(s->error);
    screen_release(base);
}
static void forge_action(Screen *base, int action, intptr_t arg, POINT pt) {
    (void)pt;
    ForgeForm *s = (ForgeForm *)base;
    switch (action) {
    case ACT_FORGE_SAVE: forge_save(s); break;
    case ACT_FORGE_DELETE: forge_delete(s); break;
    case ACT_FORGE_REPO: if (arg >= 0) forge_toggle_repo(s, (size_t)arg); break;
    case ACT_FORGE_FOCUS: if (arg >= 0 && arg < G_COUNT && s->edits[arg]) SetFocus(s->edits[arg]); break;
    }
}
static void forge_command(Screen *base, int id, int code, HWND control) {
    (void)control;
    ForgeForm *s = (ForgeForm *)base;
    int f = id - ID_FORGE_FIELD;
    if (f < 0 || f >= G_COUNT || !s->edits[f]) return;
    switch (code) {
    case EN_CHANGE: forge_changed(s); break;
    case EN_SETFOCUS: {
        s->focused = f;
        RECT content = pane_content_rect(base->pane);
        int top = s->rects[f].top - px(40), bottom = s->rects[f].bottom + px(16), y = pane_scroll_y(base->pane);
        if (top < y || bottom > y + (content.bottom - content.top)) pane_scroll_to(base->pane, top);
        pane_repaint(base->pane);
        break;
    }
    case EN_KILLFOCUS: if (s->focused == f) s->focused = -1; pane_repaint(base->pane); break;
    }
}
static bool forge_key(Screen *base, WPARAM vk, bool ctrl, bool shift) {
    (void)shift;
    if (ctrl && vk == 'S') { forge_save((ForgeForm *)base); return true; }
    return false;
}
static void forge_visible(Screen *base, bool shown) {
    ForgeForm *s = (ForgeForm *)base;
    s->shown = shown;
    if (shown) {
        forge_ensure_controls(s);
        if (s->focus_first >= 0) { int f = s->focus_first; s->focus_first = -1; SetFocus(s->edits[f]); }
    } else for (int f = 0; f < G_COUNT; f++) if (s->edits[f]) ShowWindow(s->edits[f], SW_HIDE);
}
static bool forge_can_leave(Screen *base) {
    ForgeForm *s = (ForgeForm *)base;
    if (!s->dirty) return true;
    const char *name = json_str_nonempty(json_get(s->row, "label"));
    char *message = s->id ? xstrfmt("The changes to %s have not been saved.", name ? name : "this Forge account") : xstrdup("The new Forge account has not been saved.");
    bool leave = app_confirm("Discard unsaved changes?", message, "Discard", true);
    free(message);
    if (leave) s->dirty = false;
    return leave;
}

static const ScreenVTable forge_vt = {
    .destroy = forge_destroy, .layout = forge_layout, .header = forge_header, .action = forge_action, .place = forge_place,
    .visible = forge_visible, .command = forge_command, .key = forge_key, .can_leave = forge_can_leave,
};
static Screen *forge_settings_screen_new(const Json *row, const Json *defaults) {
    ForgeForm *s = xcalloc(1, sizeof *s);
    s->base.vt = &forge_vt;
    s->focused = -1; s->focus_first = -1;
    s->row = json_is_object(row) ? json_clone(row) : json_is_object(defaults) ? json_clone(defaults) : json_object();
    s->id = ssh_row_id(s->row);
    s->base.id = forge_form_id(s->id);
    // A new account starts where its organization is typed, the one box it cannot do without.
    if (!s->id) s->focus_first = G_ORG;
    forge_fill(s);
    return &s->base;
}

// MARK: - The Slack workspace form

/// A Slack workspace's boxes. The token and the signing secret are write-only: the server never sends them back, so their
/// boxes start empty and a save with one empty keeps the stored one.
enum { W_LABEL, W_TOKEN, W_SECRET, W_COUNT };
static const FieldDef SLACK_FIELDS[W_COUNT] = {
    [W_LABEL] = { "label", K_TEXT, "Label", "Acme", "Leave empty to name it after the workspace.", 0, false },
    [W_TOKEN] = { "token", K_TEXT, "User OAuth token", "xoxp-\xE2\x80\xA6",
        "The Slack app's User OAuth Token, under OAuth & Permissions once the app is installed to the workspace. Messages go out as the user who installed it, not as a bot. The server checks it with Slack, stores it encrypted and never sends it back.", 0, true },
    [W_SECRET] = { "signingSecret", K_TEXT, "Signing secret", "From the app's Basic Information",
        "Lets the replies Slack sends to the Request URL below reach the sessions. Stored encrypted and never sent back.", 0, true },
};
/// The user token scopes the server's Slack tools call with.
static const char SLACK_SCOPES[] = "chat:write, users:read, channels:read, groups:read, im:read, mpim:read, im:write, mpim:write, channels:write, groups:write, im:history, mpim:history, channels:history and groups:history";

enum { ACT_SLACK_SAVE = 1400, ACT_SLACK_DELETE, ACT_SLACK_REPO, ACT_SLACK_FOCUS, ACT_SLACK_DM, ACT_SLACK_MODE, ACT_SLACK_COPY };
enum { ID_SLACK_FIELD = 2300, ID_SLACK_CHANNELS = 2400 };

/// A project the workspace serves: the channels it may post to (typed in a box of its own), whether it may write to
/// people, and whether each message waits for approval (`ask`) or goes at once (`allow`).
typedef struct {
    char *repo, *text;     // `text`: the channels as filled in, until the box exists
    HWND edit;
    RECT rect;
    bool laid, clipped, dm, allow;
} SlackProject;

typedef struct {
    Screen base;
    Json *row;             // what the form was filled from: the saved row or the defaults
    double id;             // 0 until the workspace is saved
    HWND edits[W_COUNT];
    RECT rects[W_COUNT];
    bool laid[W_COUNT], clipped[W_COUNT];
    SlackProject *projects;   // the projects ticked, in the order they were
    size_t project_count;
    Request *req_save, *req_delete;
    bool dirty, filling, shown, tab_dot;
    int focused, focus_first;   // a box: the fields', then each project's channels (W_COUNT + its index); -1 none
    char *error;
} SlackForm;

static bool slack_has(const SlackForm *s, const char *key) { return json_bool_is(json_get(s->row, key), true); }
static char *slack_field_text(const Json *row, int f) {
    if (f != W_LABEL) return xstrdup("");
    const char *v = json_str(json_get(row, SLACK_FIELDS[f].key));
    return xstrdup(v ? v : "");
}
/// The channels as the box shows them: `general, deploys`.
static char *channels_text(const Json *channels) {
    Str out; str_init(&out);
    for (size_t i = 0; i < json_count(channels); i++) {
        const char *c = json_str(json_at(channels, i));
        if (str_empty(c)) continue;
        if (out.len) str_appendz(&out, ", ");
        str_appendz(&out, c);
    }
    return str_detach(&out);
}
/// The channels a box holds, split on commas and spaces, without a leading `#`, each once, as the server keeps them.
static Json *channels_parse(const char *text) {
    Json *out = json_array();
    const char *p = text ? text : "";
    while (*p) {
        while (*p == ',' || *p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
        const char *start = p;
        while (*p && *p != ',' && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') p++;
        if (*start == '#') start++;
        if (p <= start) continue;
        char *c = xstrndup(start, (size_t)(p - start));
        if (!repos_has(out, c)) json_array_push(out, json_string(c));
        free(c);
    }
    return out;
}
static char *slack_project_channels(const SlackProject *p) { return p->edit ? edit_text(p->edit) : xstrdup(p->text ? p->text : ""); }
static Json *slack_project_json(const char *repo, Json *channels, bool dm, bool allow) {
    Json *o = json_object();
    json_set_str(o, "repo", repo);
    json_object_set(o, "channels", channels);
    json_set_bool(o, "directMessages", dm);
    json_set_str(o, "permissionMode", allow ? "allow" : "ask");
    return o;
}
/// The projects as the form holds them, in the shape the server takes.
static Json *slack_projects_json(const SlackForm *s) {
    Json *out = json_array();
    for (size_t i = 0; i < s->project_count; i++) {
        const SlackProject *p = &s->projects[i];
        char *text = slack_project_channels(p);
        json_array_push(out, slack_project_json(p->repo, channels_parse(text), p->dm, p->allow));
        free(text);
    }
    return out;
}
/// The saved projects in the same shape, read the way the form reads them back.
static Json *slack_saved_projects(const Json *row) {
    Json *out = json_array();
    const Json *list = json_get(row, "projects");
    for (size_t i = 0; i < json_count(list); i++) {
        const Json *p = json_at(list, i);
        const char *repo = json_str_nonempty(json_get(p, "repo"));
        if (!repo) continue;
        char *text = channels_text(json_get(p, "channels"));
        json_array_push(out, slack_project_json(repo, channels_parse(text), !json_bool_is(json_get(p, "directMessages"), false),
                                                str_eq(json_str(json_get(p, "permissionMode")), "allow")));
        free(text);
    }
    return out;
}
/// Whether two project lists say the same, in any order.
static bool slack_projects_same(const Json *a, const Json *b) {
    if (json_count(a) != json_count(b)) return false;
    for (size_t i = 0; i < json_count(a); i++) {
        const Json *x = json_at(a, i), *match = NULL;
        for (size_t k = 0; k < json_count(b) && !match; k++)
            if (str_eq(json_str(json_get(json_at(b, k), "repo")), json_str(json_get(x, "repo")))) match = json_at(b, k);
        if (!match || !json_equal(x, match)) return false;
    }
    return true;
}
static int slack_find(const SlackForm *s, const char *repo) {
    for (size_t i = 0; i < s->project_count; i++) if (str_eq(s->projects[i].repo, repo)) return (int)i;
    return -1;
}
/// The label of the other saved workspace that already serves `repo`, if one does: a project sends through one at most.
static const char *slack_taken_by(const SlackForm *s, const char *repo) {
    const Json *rows = g_settings ? slack_rows(g_settings) : NULL;
    for (size_t i = 0; i < json_count(rows); i++) {
        const Json *row = json_at(rows, i);
        if (s->id && ssh_row_id(row) == s->id) continue;
        const Json *list = json_get(row, "projects");
        for (size_t k = 0; k < json_count(list); k++)
            if (str_eq(json_str(json_get(json_at(list, k), "repo")), repo)) {
                const char *label = json_str_nonempty(json_get(row, "label"));
                return label ? label : "another";
            }
    }
    return NULL;
}
/// What the Projects list offers: every project, then any ticked one that is no longer a project.
static Json *slack_choices(const SlackForm *s) {
    Json *out = json_array();
    const Json *rows = g_settings ? settings_rows(g_settings) : NULL;
    for (size_t i = 0; i < json_count(rows); i++) {
        const char *repo = json_str_nonempty(json_get(json_at(rows, i), "repo"));
        if (repo && !repos_has(out, repo)) json_array_push(out, json_string(repo));
    }
    for (size_t i = 0; i < s->project_count; i++)
        if (!repos_has(out, s->projects[i].repo)) json_array_push(out, json_string(s->projects[i].repo));
    return out;
}

static size_t slack_edit_count(const SlackForm *s) { return W_COUNT + s->project_count; }
static HWND slack_edit_at(const SlackForm *s, size_t k) { return k < W_COUNT ? s->edits[k] : k < slack_edit_count(s) ? s->projects[k - W_COUNT].edit : NULL; }
static int slack_edit_index(const SlackForm *s, HWND e) {
    for (size_t k = 0; e && k < slack_edit_count(s); k++) if (slack_edit_at(s, k) == e) return (int)k;
    return -1;
}
static const RECT *slack_edit_rect(const SlackForm *s, size_t k) { return k < W_COUNT ? &s->rects[k] : &s->projects[k - W_COUNT].rect; }

static void slack_remove_project(SlackForm *s, size_t i) {
    SlackProject *p = &s->projects[i];
    if (p->edit) DestroyWindow(p->edit);
    free(p->repo); free(p->text);
    memmove(p, p + 1, (s->project_count - i - 1) * sizeof *p);
    s->project_count--;
    s->focused = -1;
}
static void slack_clear_projects(SlackForm *s) {
    while (s->project_count) slack_remove_project(s, s->project_count - 1);
    free(s->projects); s->projects = NULL;
}
static void slack_create_channels_edit(SlackForm *s, SlackProject *p);
static void slack_add_project(SlackForm *s, const char *repo, const char *channels, bool dm, bool allow) {
    s->projects = xrealloc(s->projects, (s->project_count + 1) * sizeof *s->projects);
    SlackProject *p = &s->projects[s->project_count++];
    memset(p, 0, sizeof *p);
    p->repo = xstrdup(repo); p->text = xstrdup(channels ? channels : ""); p->dm = dm; p->allow = allow;
    if (s->edits[0]) slack_create_channels_edit(s, p);
}

/// The edits take the row's text, and the token's and the secret's boxes say whether one is stored.
static void slack_fill_edits(SlackForm *s) {
    s->filling = true;
    for (int f = 0; f < W_COUNT; f++) {
        if (!s->edits[f]) continue;
        char *text = slack_field_text(s->row, f);
        set_edit_text(s->edits[f], text);
        free(text);
    }
    if (s->edits[W_TOKEN]) {
        wchar_t *w = utf8_to_wide(slack_has(s, "hasToken") ? "Stored \xC2\xB7 paste a new token to replace it" : SLACK_FIELDS[W_TOKEN].cue);
        SendMessageW(s->edits[W_TOKEN], EM_SETCUEBANNER, TRUE, (LPARAM)w);
        free(w);
    }
    if (s->edits[W_SECRET]) {
        wchar_t *w = utf8_to_wide(slack_has(s, "hasSigningSecret") ? "Stored \xC2\xB7 paste a new secret to replace it" : SLACK_FIELDS[W_SECRET].cue);
        SendMessageW(s->edits[W_SECRET], EM_SETCUEBANNER, TRUE, (LPARAM)w);
        free(w);
    }
    for (size_t i = 0; i < s->project_count; i++) if (s->projects[i].edit) set_edit_text(s->projects[i].edit, s->projects[i].text);
    s->filling = false;
}
static void slack_fill(SlackForm *s) {
    slack_clear_projects(s);
    s->filling = true;
    const Json *list = json_get(s->row, "projects");
    for (size_t i = 0; i < json_count(list); i++) {
        const Json *p = json_at(list, i);
        const char *repo = json_str_nonempty(json_get(p, "repo"));
        if (!repo || slack_find(s, repo) >= 0) continue;
        char *text = channels_text(json_get(p, "channels"));
        slack_add_project(s, repo, text, !json_bool_is(json_get(p, "directMessages"), false), str_eq(json_str(json_get(p, "permissionMode")), "allow"));
        free(text);
    }
    s->filling = false;
    slack_fill_edits(s);
    s->dirty = false;
}

/// The body a save sends; NULL with `*why` when the form cannot be sent as it is.
static Json *slack_body(SlackForm *s, char **why) {
    Json *body = json_object();
    for (int f = 0; f < W_COUNT; f++) {
        char *text = s->edits[f] ? edit_text(s->edits[f]) : slack_field_text(s->row, f), *t = str_trim(text);
        free(text);
        // An empty token or secret is left out, so the stored one stays.
        if (f == W_LABEL || *t) json_set_str(body, SLACK_FIELDS[f].key, t);
        free(t);
    }
    json_object_set(body, "projects", slack_projects_json(s));
    if (!slack_has(s, "hasToken") && str_empty(json_str(json_get(body, "token")))) {
        *why = xstrdup("Paste the Slack app's User OAuth Token (xoxp-\xE2\x80\xA6): messages go out as the user who installed the app.");
        json_free(body);
        return NULL;
    }
    return body;
}

/// Whether the form holds a change not saved yet, for the dot after the tab's title.
static bool slack_form_changed(SlackForm *s) {
    Json *now = slack_projects_json(s), *saved = slack_saved_projects(s->row);
    bool same = slack_projects_same(now, saved);
    json_free(now); json_free(saved);
    if (!same) return true;
    for (int f = 0; f < W_COUNT; f++) {
        if (!s->edits[f]) continue;
        char *text = edit_text(s->edits[f]), *saved_text = slack_field_text(s->row, f);
        bool differs = !str_eq(text, saved_text);
        free(text); free(saved_text);
        if (differs) return true;
    }
    return false;
}
static void slack_changed(SlackForm *s) {
    if (s->filling) return;
    if (!s->dirty) { s->dirty = true; pane_header_changed(s->base.pane); }
    bool changed = slack_form_changed(s);
    if (changed != s->tab_dot) { s->tab_dot = changed; pane_relayout(s->base.pane); }
}

// MARK: Layout

static void paint_slack_box(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    (void)doc;
    SlackForm *s = it->data;
    fill_round_rect(cv, rc, px(6), theme.raise, s->focused == (int)it->arg ? theme.accent_dim : theme.line);
}
/// A box for edit `k` at the cursor, `*rect` set to where its edit goes; advances past it.
static void slack_box(SlackForm *s, Doc *doc, int x, int w, size_t k, bool mono, RECT *rect) {
    FontId fid = mono ? FONT_MONO : FONT_BODY;
    int fh = edit_line_height(fid), h = px(36);
    RECT box = { x, doc->y, x + w, doc->y + h };
    Item *it = doc_item(doc, doc_add(doc, &box, paint_slack_box));
    it->data = s; it->arg = (intptr_t)k; it->action = ACT_SLACK_FOCUS;
    *rect = (RECT){ x + px(10), box.top + (h - fh) / 2, x + w - px(10), box.top + (h - fh) / 2 + fh };
    doc->y = box.bottom;
}
/// A labelled field, the hint behind a help icon beside the label and `note` under the box; advances.
static void slack_field(SlackForm *s, Doc *doc, int x, int w, int f, const char *note) {
    const FieldDef *d = &SLACK_FIELDS[f];
    doc_field_label(doc, x, w, d->label, theme.ink, d->hint);
    doc_space(doc, px(6));
    slack_box(s, doc, x, w, (size_t)f, d->mono, &s->rects[f]);
    s->laid[f] = true;
    if (note) { doc_space(doc, px(6)); doc_text(doc, x, w, note, FONT_FOOTNOTE, theme.muted, DT_LEFT | DT_WORDBREAK); }
    doc_space(doc, px(14));
}
/// Who the workspace's messages go out as, from Slack, once a token is stored.
static void slack_connection(SlackForm *s, Doc *doc, int x, int w) {
    const char *team = json_str_nonempty(json_get(s->row, "team")), *user = json_str_nonempty(json_get(s->row, "user"));
    const char *url = json_str_nonempty(json_get(s->row, "url"));
    int box = doc_box_begin(doc, x, w, px(12), theme.raise, theme.line, px(6));
    doc_labeled(doc, x + px(12), w - px(24), "Workspace", team ? team : "\xE2\x80\x94", theme.ink);
    if (url) { doc_space(doc, px(4)); doc_labeled(doc, x + px(12), w - px(24), "Address", url, theme.muted); }
    doc_space(doc, px(4));
    doc_labeled(doc, x + px(12), w - px(24), "Sends as", user ? user : "\xE2\x80\x94", theme.ink);
    doc_box_end(doc, box, px(12));
    doc_space(doc, px(18));
}
/// Where Slack sends the replies: the Request URL for the app's Event Subscriptions, with a Copy button.
static void slack_events(SlackForm *s, Doc *doc, int x, int w) {
    doc_field_label(doc, x, w, "Request URL", theme.ink,
        "Turn on the Slack app's Event Subscriptions with this as the Request URL, and subscribe on behalf of users to message.im, message.mpim, message.channels and message.groups. Like the server's other webhooks, the path must bypass Cloudflare Access.");
    doc_space(doc, px(6));
    const char *url = json_str_nonempty(json_get(s->row, "eventsUrl"));
    if (!s->id) {
        doc_text(doc, x, w, "Save the workspace to get the Request URL its replies come back through.", FONT_FOOTNOTE, theme.muted, DT_LEFT | DT_WORDBREAK);
    } else if (!url) {
        doc_text(doc, x, w, "The server has no public address (PUBLIC_BASE_URL), so it has no Request URL to give Slack: messages can be sent, but replies do not reach the sessions.", FONT_FOOTNOTE, theme.muted, DT_LEFT | DT_WORDBREAK);
    } else {
        int bw = text_width(doc->cv, "Copy", FONT_CAPTION) + px(28), top = doc->y;
        int cw = text_width(doc->cv, url, FONT_MONO);
        if (cw > w - bw - px(14)) cw = w - bw - px(14);
        doc_text(doc, x, cw, url, FONT_MONO, theme.ink, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
        int bottom = doc->y;
        doc->y = top;
        doc_button(doc, x + cw + px(14), 0, "Copy", BUTTON_PLAIN, ACT_SLACK_COPY, 0, true);
        if (doc->y < bottom) doc->y = bottom;
        doc_space(doc, px(6));
        doc_text(doc, x, w, "Subscribe on behalf of users to message.im, message.mpim, message.channels and message.groups.", FONT_FOOTNOTE, theme.muted, DT_LEFT | DT_WORDBREAK);
        if (!slack_has(s, "hasSigningSecret")) {
            doc_space(doc, px(4));
            doc_text(doc, x, w, "Replies reach the sessions once the signing secret is saved.", FONT_FOOTNOTE, theme.muted, DT_LEFT | DT_WORDBREAK);
        }
    }
    doc_space(doc, px(18));
}
/// The projects that may send through the workspace, one tick box each; a ticked one opens its channels, whether it may
/// write to people and whether each message waits for approval.
static void slack_projects(SlackForm *s, Doc *doc, int x, int w) {
    doc_field_label(doc, x, w, "Projects", theme.ink, "The projects whose sessions may send through this workspace. Leave every project unticked for an inbox-only workspace (projects: []). A project sends through one workspace at most.");
    doc_space(doc, px(6));
    Json *choices = slack_choices(s);
    const Json *rows = g_settings ? settings_rows(g_settings) : NULL;
    for (size_t i = 0; i < json_count(choices); i++) {
        const char *repo = json_str(json_at(choices, i));
        bool listed = false;
        for (size_t k = 0; k < json_count(rows); k++) listed |= str_eq(json_str(json_get(json_at(rows, k), "repo")), repo);
        int at = slack_find(s, repo);
        const char *taken = at < 0 ? slack_taken_by(s, repo) : NULL;
        ForgeCheck *d = xcalloc(1, sizeof *d);
        d->on = at >= 0; d->gone = !listed || taken;
        d->text = !listed ? xstrfmt("%s (not a project)", repo) : taken ? xstrfmt("%s \xC2\xB7 sends through %s", repo, taken) : xstrdup(repo);
        int it = doc_custom(doc, x, w, px(28), paint_forge_check, d, forge_check_free, ACT_SLACK_REPO, (intptr_t)i);
        doc_item(doc, it)->hand = true;
        if (at < 0) continue;
        SlackProject *p = &s->projects[at];
        int ix = x + px(23), iw = w - px(23);
        doc_space(doc, px(4));
        doc_field_label(doc, ix, iw, "Channels", theme.muted, "The channel names or ids its sessions may post to, separated by commas. Leave empty for none: it then writes only to people, if allowed below.");
        doc_space(doc, px(6));
        slack_box(s, doc, ix, iw, W_COUNT + (size_t)at, true, &p->rect);
        p->laid = true;
        doc_space(doc, px(8));
        ForgeCheck *dm = xcalloc(1, sizeof *dm);
        dm->on = p->dm; dm->text = xstrdup("May write direct messages to people");
        it = doc_custom(doc, ix, iw, px(28), paint_forge_check, dm, forge_check_free, ACT_SLACK_DM, (intptr_t)at);
        doc_item(doc, it)->hand = true;
        doc_space(doc, px(6));
        static const char *const modes[] = { "Ask before each message", "Send at once" };
        doc_segments(doc, ix, iw, modes, 2, p->allow ? 1 : 0, ACT_SLACK_MODE, (intptr_t)at * 2, true);
        doc_space(doc, px(6));
        doc_text(doc, ix, iw, p->allow
            ? "Messages go out as soon as a session sends them, except in a turn a webhook or a Slack reply started, which always asks."
            : "Each message waits in the attention inbox until you approve it, for a day at most.",
            FONT_FOOTNOTE, theme.muted, DT_LEFT | DT_WORDBREAK);
        doc_space(doc, px(14));
    }
    if (!json_count(choices)) doc_text(doc, x, w, "No projects assigned: this workspace can be used only in the operator inbox (projects: []).", FONT_FOOTNOTE, theme.muted, DT_LEFT | DT_WORDBREAK);
    json_free(choices);
    doc_space(doc, px(14));
}

static void slack_layout(Screen *base, Doc *doc) {
    SlackForm *s = (SlackForm *)base;
    memset(s->laid, 0, sizeof s->laid);
    for (size_t i = 0; i < s->project_count; i++) s->projects[i].laid = false;
    int col = doc->width, x = 0;
    doc_space(doc, px(8));
    int h = px(42), tx = x, ty = doc->y;
    s->tab_dot = s->dirty && slack_form_changed(s);
    doc_tab(doc, &tx, &ty, x, x + col, h, 0xE8BD, s->tab_dot ? "Slack workspace \xE2\x80\xA2" : "Slack workspace", NULL, true, 0, 0);
    doc->y = ty + h;
    doc_rule(doc, x, col);
    doc_space(doc, px(18));
    if (s->error) { doc_notice_box(doc, x, col, s->error); doc_space(doc, px(16)); }
    if (s->id && slack_has(s, "hasToken")) slack_connection(s, doc, x, col);
    // Label and token side by side, as the Forge form's label and organization; the scopes the token needs across under them.
    int gap = px(14), half = (col - gap) / 2, top = doc->y;
    slack_field(s, doc, x, half, W_LABEL, NULL);
    int bottom = doc->y;
    doc->y = top;
    slack_field(s, doc, x + half + gap, col - half - gap, W_TOKEN, NULL);
    if (doc->y < bottom) doc->y = bottom;
    char *scopes = xstrfmt("Create a Slack app at api.slack.com/apps and give it these user token scopes under OAuth & Permissions: %s. Install it to the workspace, then paste its User OAuth Token.", SLACK_SCOPES);
    doc->y -= px(8);
    doc_text(doc, x, col, scopes, FONT_FOOTNOTE, theme.muted, DT_LEFT | DT_WORDBREAK);
    doc_space(doc, px(14));
    free(scopes);
    slack_field(s, doc, x, col, W_SECRET, NULL);
    slack_events(s, doc, x, col);
    slack_projects(s, doc, x, col);
    doc_space(doc, px(40));
}

static void slack_header(Screen *base, HeaderInfo *info) {
    SlackForm *s = (SlackForm *)base;
    const char *label = json_str_nonempty(json_get(s->row, "label")), *team = json_str_nonempty(json_get(s->row, "team"));
    const char *user = json_str_nonempty(json_get(s->row, "user"));
    if (s->id) {
        snprintf(info->title, sizeof info->title, "%s", label ? label : team ? team : "Slack workspace");
        size_t n = json_count(json_get(s->row, "projects"));
        if (!slack_has(s, "hasToken")) snprintf(info->subtitle, sizeof info->subtitle, "%zu project%s \xC2\xB7 no token", n, n == 1 ? "" : "s");
        else snprintf(info->subtitle, sizeof info->subtitle, "%s%s%s \xC2\xB7 %zu project%s%s", team ? team : "Slack", user ? " \xC2\xB7 as " : "", user ? user : "",
                      n, n == 1 ? "" : "s", slack_has(s, "hasSigningSecret") ? "" : " \xC2\xB7 no replies");
    } else {
        snprintf(info->title, sizeof info->title, "New Slack workspace");
        snprintf(info->subtitle, sizeof info->subtitle, "Sessions send Slack messages as you, and hear the replies.");
    }
    bool busy = s->req_save || s->req_delete;
    HeaderButton *b = &info->buttons[info->button_count++];
    snprintf(b->label, sizeof b->label, "%s", s->req_save ? "Saving\xE2\x80\xA6" : "Save");
    b->glyph = 0xE74E; b->action = ACT_SLACK_SAVE; b->prominent = true; b->tip = "Save this Slack workspace (Ctrl+S)";
    b->enabled = !busy && (s->dirty || !s->id) && store_supports(s->id ? "update_slack_workspace" : "create_slack_workspace");
    if (!s->id) return;
    HeaderButton *d = &info->buttons[info->button_count++];
    d->glyph = 0xE74D; d->action = ACT_SLACK_DELETE; d->destructive = true; d->enabled = !busy && store_supports("delete_slack_workspace"); d->tip = "Delete this Slack workspace";
}

// MARK: The edits

static void slack_place_edit(Screen *base, HWND e, const RECT *rect, bool laid, bool *clipped, bool shown, const RECT *content, int scroll_y) {
    if (!e) return;
    if (!shown || !laid) { ShowWindow(e, SW_HIDE); return; }
    int m = margin_of(base->pane);
    RECT r = { content->left + m + rect->left, content->top + rect->top - scroll_y, content->left + m + rect->right, content->top + rect->bottom - scroll_y };
    RECT visible;
    if (!IntersectRect(&visible, &r, content)) { ShowWindow(e, SW_HIDE); return; }
    MoveWindow(e, r.left, r.top, r.right - r.left, r.bottom - r.top, TRUE);
    bool now = !EqualRect(&visible, &r);
    if (now) SetWindowRgn(e, CreateRectRgn(visible.left - r.left, visible.top - r.top, visible.right - r.left, visible.bottom - r.top), TRUE);
    else if (*clipped) SetWindowRgn(e, NULL, TRUE);
    *clipped = now;
    ShowWindow(e, SW_SHOWNA);
}
static void slack_place(Screen *base, const RECT *content, int scroll_y) {
    SlackForm *s = (SlackForm *)base;
    for (int f = 0; f < W_COUNT; f++) slack_place_edit(base, s->edits[f], &s->rects[f], s->laid[f], &s->clipped[f], s->shown, content, scroll_y);
    for (size_t i = 0; i < s->project_count; i++) {
        SlackProject *p = &s->projects[i];
        slack_place_edit(base, p->edit, &p->rect, p->laid, &p->clipped, s->shown, content, scroll_y);
    }
}

static void slack_save(SlackForm *s);
static LRESULT CALLBACK slack_field_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref) {
    SlackForm *s = (SlackForm *)ref;
    bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    switch (msg) {
    case WM_KEYDOWN: {
        int k = slack_edit_index(s, hwnd);
        size_t n = slack_edit_count(s);
        if ((wp == VK_TAB || wp == VK_RETURN) && k >= 0) {
            bool back = wp == VK_TAB && (GetKeyState(VK_SHIFT) & 0x8000);
            SetFocus(slack_edit_at(s, ((size_t)k + (back ? n - 1 : 1)) % n));
            return 0;
        }
        if (ctrl && wp == 'S') { slack_save(s); return 0; }
        if (ctrl && wp == 'A') { SendMessageW(hwnd, EM_SETSEL, 0, -1); return 0; }
        if (wp == VK_ESCAPE) { SetFocus(GetParent(hwnd)); return 0; }
        break;
    }
    case WM_CHAR:
        if (wp == '\t' || wp == 0x13 || wp == 0x01 || wp == 0x1B || wp == '\r') return 0;
        break;
    case WM_MOUSEWHEEL: SendMessageW(GetParent(hwnd), msg, wp, lp); return 0;
    case WM_NCDESTROY: RemoveWindowSubclass(hwnd, slack_field_proc, id); break;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}
static HWND slack_new_edit(SlackForm *s, int id, bool password, bool mono, int limit, const char *cue) {
    DWORD style = WS_CHILD | WS_TABSTOP | ES_AUTOHSCROLL | (password ? ES_PASSWORD : 0);
    HWND e = CreateWindowExW(0, L"EDIT", L"", style, 0, 0, 10, 10, pane_hwnd(s->base.pane), (HMENU)(INT_PTR)id, GetModuleHandleW(NULL), NULL);
    SendMessageW(e, WM_SETFONT, (WPARAM)font(mono ? FONT_MONO : FONT_BODY), TRUE);
    SendMessageW(e, EM_SETLIMITTEXT, limit, 0);
    wchar_t *w = utf8_to_wide(cue); SendMessageW(e, EM_SETCUEBANNER, TRUE, (LPARAM)w); free(w);
    SetWindowSubclass(e, slack_field_proc, (UINT_PTR)id, (DWORD_PTR)s);
    theme_apply_control(e);
    return e;
}
static void slack_create_channels_edit(SlackForm *s, SlackProject *p) {
    if (p->edit) return;
    p->edit = slack_new_edit(s, ID_SLACK_CHANNELS, false, true, 2000, "general, deploys");
    bool filling = s->filling;
    s->filling = true;
    set_edit_text(p->edit, p->text);
    s->filling = filling;
}
static void slack_ensure_controls(SlackForm *s) {
    if (s->edits[0]) return;
    for (int f = 0; f < W_COUNT; f++)
        s->edits[f] = slack_new_edit(s, ID_SLACK_FIELD + f, f != W_LABEL, SLACK_FIELDS[f].mono, f == W_LABEL ? 200 : 4096, SLACK_FIELDS[f].cue);
    for (size_t i = 0; i < s->project_count; i++) slack_create_channels_edit(s, &s->projects[i]);
    slack_fill_edits(s);
}

static void slack_toggle_repo(SlackForm *s, size_t index) {
    Json *choices = slack_choices(s);
    const char *repo = json_str(json_at(choices, index));
    if (repo) {
        int at = slack_find(s, repo);
        if (at >= 0) slack_remove_project(s, (size_t)at);
        // The server's defaults for a project: no channels, direct messages allowed, every message asked about.
        else slack_add_project(s, repo, "", true, false);
        slack_changed(s);
        pane_relayout(s->base.pane);
    }
    json_free(choices);
}

// MARK: Saving, deleting

static void slack_show_error(SlackForm *s, char *text) {
    set_string(&s->error, text); free(text);
    pane_relayout(s->base.pane); pane_header_changed(s->base.pane); pane_scroll_to_top(s->base.pane);
}
static void slack_save_done(void *owner, Request *req) {
    SlackForm *s = owner;
    const Json *row = req->ok ? json_get(req->result, "workspace") : NULL;
    if (!json_is_object(row)) {
        char *text = request_error_or_unexpected(req);
        slack_show_error(s, text);
        return;
    }
    // The server's word on what was saved: who the token is, an empty label now the workspace's name, and a new
    // workspace's id and Request URL.
    json_free(s->row); s->row = json_clone(row);
    set_string(&s->error, NULL);
    s->id = ssh_row_id(row);
    free(s->base.id); s->base.id = slack_form_id(s->id);
    pane_set_selected_id(app_sidebar_pane(), s->base.id);
    slack_fill(s);
    pane_relayout(s->base.pane); pane_header_changed(s->base.pane);
    settings_slack_changed();
}
static void slack_save(SlackForm *s) {
    if (s->req_save || s->req_delete || !store_supports(s->id ? "update_slack_workspace" : "create_slack_workspace")) return;
    char *why = NULL;
    Json *body = slack_body(s, &why);
    if (!body) { slack_show_error(s, why); return; }
    if (s->id) json_set_num(body, "id", s->id);
    store_call(s->id ? "update_slack_workspace" : "create_slack_workspace", body, 0, s, slack_save_done, 0, &s->req_save);
    pane_header_changed(s->base.pane);
}
static void slack_delete_done(void *owner, Request *req) {
    SlackForm *s = owner;
    if (!req->ok) { slack_show_error(s, request_error_text(req)); return; }
    s->dirty = false;
    app_clear_detail();
    settings_slack_changed();
}
static void slack_delete(SlackForm *s) {
    if (!s->id || s->req_save || s->req_delete) return;
    const char *name = json_str_nonempty(json_get(s->row, "label"));
    char *title = xstrfmt("Delete %s?", name ? name : "this Slack workspace");
    bool ok = app_confirm(title, "Its stored token and signing secret are removed: its projects' sessions can no longer send Slack messages, and replies stop reaching them.", "Delete", true);
    free(title);
    if (!ok) return;
    Json *args = json_object(); json_set_num(args, "id", s->id);
    store_call("delete_slack_workspace", args, 0, s, slack_delete_done, 0, &s->req_delete);
    pane_header_changed(s->base.pane);
}

// MARK: The screen

static void slack_destroy(Screen *base) {
    SlackForm *s = (SlackForm *)base;
    request_cancel(&s->req_save); request_cancel(&s->req_delete);
    for (int f = 0; f < W_COUNT; f++) if (s->edits[f]) DestroyWindow(s->edits[f]);
    slack_clear_projects(s);
    json_free(s->row); free(s->error);
    screen_release(base);
}
static void slack_action(Screen *base, int action, intptr_t arg, POINT pt) {
    (void)pt;
    SlackForm *s = (SlackForm *)base;
    switch (action) {
    case ACT_SLACK_SAVE: slack_save(s); break;
    case ACT_SLACK_DELETE: slack_delete(s); break;
    case ACT_SLACK_REPO: if (arg >= 0) slack_toggle_repo(s, (size_t)arg); break;
    case ACT_SLACK_DM:
        if (arg >= 0 && (size_t)arg < s->project_count) { s->projects[arg].dm = !s->projects[arg].dm; slack_changed(s); pane_relayout(base->pane); }
        break;
    case ACT_SLACK_MODE:
        if (arg >= 0 && (size_t)(arg / 2) < s->project_count) {
            SlackProject *p = &s->projects[arg / 2];
            bool allow = arg % 2 == 1;
            if (p->allow != allow) { p->allow = allow; slack_changed(s); pane_relayout(base->pane); }
        }
        break;
    case ACT_SLACK_FOCUS: { HWND e = arg >= 0 ? slack_edit_at(s, (size_t)arg) : NULL; if (e) SetFocus(e); break; }
    case ACT_SLACK_COPY: {
        const char *url = json_str_nonempty(json_get(s->row, "eventsUrl"));
        if (url) copy_to_clipboard(pane_hwnd(base->pane), url);
        break;
    }
    }
}
static void slack_command(Screen *base, int id, int code, HWND control) {
    SlackForm *s = (SlackForm *)base;
    if (id != ID_SLACK_CHANNELS && (id < ID_SLACK_FIELD || id >= ID_SLACK_FIELD + W_COUNT)) return;
    int k = slack_edit_index(s, control);
    if (k < 0) return;
    switch (code) {
    case EN_CHANGE: slack_changed(s); break;
    case EN_SETFOCUS: {
        s->focused = k;
        const RECT *rc = slack_edit_rect(s, (size_t)k);
        RECT content = pane_content_rect(base->pane);
        int top = rc->top - px(40), bottom = rc->bottom + px(16), y = pane_scroll_y(base->pane);
        if (top < y || bottom > y + (content.bottom - content.top)) pane_scroll_to(base->pane, top);
        pane_repaint(base->pane);
        break;
    }
    case EN_KILLFOCUS: if (s->focused == k) s->focused = -1; pane_repaint(base->pane); break;
    }
}
static bool slack_key(Screen *base, WPARAM vk, bool ctrl, bool shift) {
    (void)shift;
    if (ctrl && vk == 'S') { slack_save((SlackForm *)base); return true; }
    return false;
}
static void slack_visible(Screen *base, bool shown) {
    SlackForm *s = (SlackForm *)base;
    s->shown = shown;
    if (shown) {
        slack_ensure_controls(s);
        if (s->focus_first >= 0) { HWND e = slack_edit_at(s, (size_t)s->focus_first); s->focus_first = -1; if (e) SetFocus(e); }
    } else {
        for (int f = 0; f < W_COUNT; f++) if (s->edits[f]) ShowWindow(s->edits[f], SW_HIDE);
        for (size_t i = 0; i < s->project_count; i++) if (s->projects[i].edit) ShowWindow(s->projects[i].edit, SW_HIDE);
    }
}
static bool slack_can_leave(Screen *base) {
    SlackForm *s = (SlackForm *)base;
    if (!s->dirty) return true;
    const char *name = json_str_nonempty(json_get(s->row, "label"));
    char *message = s->id ? xstrfmt("The changes to %s have not been saved.", name ? name : "this Slack workspace") : xstrdup("The new Slack workspace has not been saved.");
    bool leave = app_confirm("Discard unsaved changes?", message, "Discard", true);
    free(message);
    if (leave) s->dirty = false;
    return leave;
}

static const ScreenVTable slack_vt = {
    .destroy = slack_destroy, .layout = slack_layout, .header = slack_header, .action = slack_action, .place = slack_place,
    .visible = slack_visible, .command = slack_command, .key = slack_key, .can_leave = slack_can_leave,
};
static Screen *slack_settings_screen_new(const Json *row, const Json *defaults) {
    SlackForm *s = xcalloc(1, sizeof *s);
    s->base.vt = &slack_vt;
    s->focused = -1; s->focus_first = -1;
    s->row = json_is_object(row) ? json_clone(row) : json_is_object(defaults) ? json_clone(defaults) : json_object();
    s->id = ssh_row_id(s->row);
    s->base.id = slack_form_id(s->id);
    // A new workspace starts where its token is pasted, the one box it cannot do without.
    if (!s->id) s->focus_first = W_TOKEN;
    slack_fill(s);
    return &s->base;
}
