// A project's Forge tab, beside its pull requests and issues: the servers of the Laravel Forge accounts the project may
// use (Settings → Forge accounts) down the left, and the server picked there on the right with its Forge sites, each with
// its domain, state, repository and branch, deployment and PHP. The server reads Forge with the account's token
// (/forge/accounts/{account}/servers and …/sites), so the token never reaches this PC; both need an Admin token.
#include "screens.h"
#include "str.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The actions, from the host's `action_base` up.
enum { A_SERVER, A_OPEN_SITE };
enum { LIST_W = 280, MAX_PAGES = 20 };

struct ProjectForge {
    char *repo;
    Screen *host;
    int base;
    Json *accounts;        // the ForgeAccount rows available to the project
    Json *servers;         // every account's ForgeServer rows, each with `_account` and `_accountLabel` added
    size_t walking;        // the account whose servers are being read
    int pages;             // pages of it read so far
    bool loaded;
    char *error;           // the accounts' or a server list's
    Request *req;
    char *selected;        // the server on show, as "account:server"
    Json *sites;           // "account:server" → its ForgeSite rows, once read in full
    Json *incoming;        // the pages of the server being read
    int site_pages;
    char *sites_error;
    Request *req_sites;
};

bool project_forge_offered(void) { return store_supports("settings_forge_accounts") && store_supports("forge_servers") && store_supports("forge_sites"); }

static double row_id(const Json *row) {
    double id;
    return json_num(json_get(row, "id"), &id) && isfinite(id) ? id : 0;
}
static char *server_key(const Json *server) { return xstrfmt("%.0f:%.0f", json_num_or(json_get(server, "_account"), 0), row_id(server)); }
static const Json *selected_server(ProjectForge *p) {
    if (!p->selected) return NULL;
    for (size_t i = 0; i < json_count(p->servers); i++) {
        char *key = server_key(json_at(p->servers, i));
        bool hit = str_eq(key, p->selected);
        free(key);
        if (hit) return json_at(p->servers, i);
    }
    return NULL;
}

static void relayout(ProjectForge *p) {
    if (!p->host->pane) return;
    pane_relayout(p->host->pane);
    pane_header_changed(p->host->pane);
}

// MARK: - Loading

static void sites_load(ProjectForge *p);
static void servers_next(ProjectForge *p, const char *cursor);
static void servers_done(void *owner, Request *req) {
    ProjectForge *p = owner;
    const Json *account = json_at(p->accounts, p->walking);
    if (!req->ok) {
        // One account's refusal (a revoked token, Forge rate limiting) leaves the others' servers listed.
        char *why = request_error_text(req);
        const char *label = json_str_nonempty(json_get(account, "label"));
        char *line = xstrfmt("%s%s%s: %s", p->error ? p->error : "", p->error ? "\n" : "", label ? label : json_str(json_get(account, "organization")), why);
        set_string(&p->error, line);
        free(line); free(why);
    } else {
        const Json *rows = json_get(req->result, "servers");
        const char *label = json_str_nonempty(json_get(account, "label"));
        for (size_t i = 0; i < json_count(rows); i++) {
            Json *row = json_clone(json_at(rows, i));
            if (!json_is_object(row)) { json_free(row); continue; }
            json_set_num(row, "_account", row_id(account));
            json_set_str(row, "_accountLabel", label ? label : json_str(json_get(account, "organization")));
            json_array_push(p->servers, row);
        }
        const char *next = json_str_nonempty(json_get(req->result, "nextCursor"));
        if (next && ++p->pages < MAX_PAGES) { servers_next(p, next); relayout(p); return; }
    }
    p->walking++; p->pages = 0;
    servers_next(p, NULL);
    relayout(p);
}
/// Reads the next page of the account being walked, or moves on to the next account; once every account is read, opens
/// the first server unless one is picked.
static void servers_next(ProjectForge *p, const char *cursor) {
    if (p->walking < json_count(p->accounts)) {
        Json *args = json_object();
        json_set_num(args, "account", row_id(json_at(p->accounts, p->walking)));
        if (cursor) json_set_str(args, "cursor", cursor);
        store_call("forge_servers", args, 0, p, servers_done, 0, &p->req);
        return;
    }
    p->loaded = true;
    if (!selected_server(p) && json_count(p->servers)) {
        free(p->selected);
        p->selected = server_key(json_at(p->servers, 0));
    }
    sites_load(p);
}
static void accounts_done(void *owner, Request *req) {
    ProjectForge *p = owner;
    if (!req->ok) { request_error_into(&p->error, req); p->loaded = true; relayout(p); return; }
    json_free(p->accounts); p->accounts = json_array();
    const Json *rows = json_get(req->result, "accounts");
    // Only an account with a token can be read; the server answers the others with a refusal.
    for (size_t i = 0; i < json_count(rows); i++)
        if (!json_bool_is(json_get(json_at(rows, i), "hasToken"), false)) json_array_push(p->accounts, json_clone(json_at(rows, i)));
    p->walking = 0; p->pages = 0;
    servers_next(p, NULL);
    relayout(p);
}
void project_forge_load(ProjectForge *p) {
    if (p->loaded || p->req) return;
    if (!project_forge_offered()) { p->loaded = true; return; }
    set_string(&p->error, NULL);
    json_free(p->servers); p->servers = json_array();
    Json *args = json_object();
    json_set_str(args, "repo", p->repo);
    store_call("settings_forge_accounts", args, 0, p, accounts_done, 0, &p->req);
}

static void sites_next(ProjectForge *p, const Json *server, const char *cursor);
static void sites_done(void *owner, Request *req) {
    ProjectForge *p = owner;
    const Json *server = selected_server(p);
    if (!server) return;
    if (!req->ok) { request_error_into(&p->sites_error, req); json_free(p->incoming); p->incoming = NULL; relayout(p); return; }
    const Json *rows = json_get(req->result, "sites");
    for (size_t i = 0; i < json_count(rows); i++) json_array_push(p->incoming, json_clone(json_at(rows, i)));
    const char *next = json_str_nonempty(json_get(req->result, "nextCursor"));
    if (next && ++p->site_pages < MAX_PAGES) { sites_next(p, server, next); return; }
    json_object_set(p->sites, p->selected, p->incoming);
    p->incoming = NULL;
    relayout(p);
}
static void sites_next(ProjectForge *p, const Json *server, const char *cursor) {
    Json *args = json_object();
    json_set_num(args, "account", json_num_or(json_get(server, "_account"), 0));
    json_set_num(args, "server", row_id(server));
    if (cursor) json_set_str(args, "cursor", cursor);
    store_call("forge_sites", args, 0, p, sites_done, 0, &p->req_sites);
}
/// Reads the sites of the server on show, unless they are read already.
static void sites_load(ProjectForge *p) {
    const Json *server = selected_server(p);
    if (!server || p->req_sites || json_is_array(json_get(p->sites, p->selected))) return;
    set_string(&p->sites_error, NULL);
    json_free(p->incoming); p->incoming = json_array(); p->site_pages = 0;
    sites_next(p, server, NULL);
}
void project_forge_refresh(ProjectForge *p) {
    request_cancel(&p->req); request_cancel(&p->req_sites);
    json_free(p->incoming); p->incoming = NULL;
    json_free(p->sites); p->sites = json_object();
    set_string(&p->sites_error, NULL);
    p->loaded = false;
    project_forge_load(p);
}

// MARK: - Layout

typedef struct { char *name, *detail; bool ready, selected; } ServerRowData;
static void server_row_free(void *v) { ServerRowData *d = v; free(d->name); free(d->detail); free(d); }
static void paint_server(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    ServerRowData *d = it->data;
    if (d->selected || doc_item_hovered(doc, it)) fill_round_rect(cv, rc, px(6), theme.raise, d->selected ? theme.line : theme.raise);
    if (d->selected) { RECT bar = { rc->left, rc->top + px(8), rc->left + px(3), rc->bottom - px(8) }; fill_rect(cv, &bar, theme.accent); }
    int x = rc->left + px(8), top = rc->top + px(5);
    RECT icon = { x, rc->top, x + px(20), rc->bottom };
    draw_glyph(cv, 0xE753, &icon, FONT_ICON_SMALL, d->selected ? theme.accent : theme.secondary);
    int right = rc->right - px(10);
    draw_status_dot(cv, right - px(4), (rc->top + rc->bottom) / 2, d->ready ? "idle" : "waiting");
    right -= px(16);
    int tx = icon.right + px(6);
    RECT t = { tx, top, right, top + px(20) };
    draw_text(cv, d->name, &t, d->selected ? FONT_SUBHEADLINE_SEMIBOLD : FONT_SUBHEADLINE, theme.ink, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    RECT sub = { tx, top + px(20), right, top + px(36) };
    draw_text(cv, d->detail, &sub, FONT_CAPTION2, theme.muted, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}
static void paint_rule(Doc *doc, Item *it, Canvas *cv, const RECT *rc) { (void)doc; (void)it; fill_rect(cv, rc, theme.line); }

/// "a · b · c" from the parts that are set.
static char *joined(const char *const *parts, size_t count) {
    Str s; str_init(&s);
    for (size_t i = 0; i < count; i++) {
        if (str_empty(parts[i])) continue;
        if (s.len) str_appendz(&s, " \xC2\xB7 ");
        str_appendz(&s, parts[i]);
    }
    char *out = xstrdup(s.data ? s.data : "");
    str_free(&s);
    return out;
}
static char *server_detail(const Json *server) {
    const char *parts[] = { json_str(json_get(server, "ip_address")), json_str(json_get(server, "provider")), json_str(json_get(server, "region")) };
    return joined(parts, 3);
}

/// The servers down the left, grouped by account when the project has more than one.
static void layout_list(ProjectForge *p, Doc *doc, int x, int w) {
    if (p->error) { doc_notice(doc, x, w, p->error); doc_space(doc, px(8)); }
    bool grouped = json_count(p->accounts) > 1;
    double account = -1;
    for (size_t i = 0; i < json_count(p->servers); i++) {
        const Json *row = json_at(p->servers, i);
        double a = json_num_or(json_get(row, "_account"), 0);
        if (grouped && a != account) { doc_section(doc, x + px(6), w - px(12), json_str(json_get(row, "_accountLabel"))); account = a; }
        ServerRowData *d = xcalloc(1, sizeof *d);
        const char *name = json_str_nonempty(json_get(row, "name"));
        d->name = xstrdup(name ? name : "Forge server");
        d->detail = server_detail(row);
        d->ready = !json_bool_is(json_get(row, "is_ready"), false);
        char *key = server_key(row);
        d->selected = str_eq(key, p->selected);
        free(key);
        doc_custom(doc, x, w, px(46), paint_server, d, server_row_free, p->base + A_SERVER, (intptr_t)i);
        doc_space(doc, px(2));
    }
    if (!p->loaded) { doc_loading(doc, x, w, "Loading Forge servers\xE2\x80\xA6"); return; }
    if (!json_count(p->accounts) && !p->error)
        doc_text(doc, x + px(6), w - px(12), "No Forge account is available to this project. Add one, or this project to one, under \xE2\x9A\x99 Settings \xE2\x86\x92 Forge accounts.", FONT_FOOTNOTE, theme.muted, DT_WORDBREAK);
    else if (!json_count(p->servers) && !p->error)
        doc_text(doc, x + px(6), w - px(12), "This project's Forge accounts have no servers.", FONT_FOOTNOTE, theme.muted, DT_WORDBREAK);
}

/// Whether a site deploys this project: its repository names `owner/name`.
static bool site_is_project(const Json *site, const char *repo) {
    const Json *r = json_get(site, "repository");
    const char *names[] = { json_str(json_get(r, "url")), json_str(json_get(r, "name")), json_str(json_get(r, "repository")), json_str(r) };
    for (size_t i = 0; i < sizeof names / sizeof *names; i++) {
        if (str_empty(names[i])) continue;
        // owner/name, as the end of a URL or of git@host:owner/name(.git).
        size_t n = strlen(names[i]), rn = strlen(repo);
        char *s = xstrdup(names[i]);
        if (n > 4 && str_ieq(s + n - 4, ".git")) s[n -= 4] = 0;
        bool hit = n >= rn && str_ieq(s + n - rn, repo) && (n == rn || s[n - rn - 1] == '/' || s[n - rn - 1] == ':');
        free(s);
        if (hit) return true;
    }
    return false;
}
/// A site's state as a badge colour: installed green, failing red, under way blue.
static COLORREF state_color(const char *state) {
    if (str_empty(state)) return theme.muted;
    if (str_icontains(state, "fail") || str_icontains(state, "error")) return theme.danger;
    if (str_ieq(state, "installed") || str_ieq(state, "deployed") || str_ieq(state, "finished") || str_ieq(state, "success")) return theme.ok;
    if (str_icontains(state, "ing") || str_ieq(state, "queued") || str_ieq(state, "pending")) return theme.accent;
    return theme.muted;
}
/// A field read as text whatever its type: a string, a number or a boolean.
static char *field_text(const Json *v) {
    const char *s = json_str_nonempty(v);
    if (s) return xstrdup(s);
    double n;
    if (json_num(v, &n) && isfinite(n)) return n == floor(n) ? xstrfmt("%.0f", n) : xstrfmt("%g", n);
    int b = json_bool_tristate(v);
    return b >= 0 ? xstrdup(b ? "Yes" : "No") : NULL;
}
static void labeled_field(Doc *doc, int x, int w, const char *label, const Json *v) {
    char *text = field_text(v);
    if (!text) return;
    doc_space(doc, px(4));
    doc_labeled(doc, x, w, label, text, theme.text);
    free(text);
}

/// One site: its domain and state, then what Forge says about it, and Open site.
static void layout_site(ProjectForge *p, Doc *doc, int x, int w, const Json *site, size_t index) {
    bool mine = site_is_project(site, p->repo);
    int box = doc_box_begin(doc, x, w, px(12), theme.elevated, mine ? theme.accent : theme.line, px(10));
    doc_item(doc, box)->hover_fill = false;
    int ix = x + px(14), iw = w - px(28);
    const char *name = json_str_nonempty(json_get(site, "name"));
    doc_text(doc, ix, iw, name ? name : "Forge site", FONT_HEADLINE, theme.ink, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
    doc_space(doc, px(6));
    BadgeSpec badges[4]; size_t bn = 0;
    const char *state = json_str_nonempty(json_get(site, "status"));
    const char *deploy = json_str_nonempty(json_get(site, "deployment_status"));
    if (state) { BadgeSpec b = { 0, state, state_color(state), true }; badges[bn++] = b; }
    char *deploy_text = deploy ? xstrfmt("Deployment %s", deploy) : NULL;
    if (deploy) { BadgeSpec b = { 0xE895, deploy_text, state_color(deploy), false }; badges[bn++] = b; }
    if (json_bool_is(json_get(site, "quick_deploy"), true)) { BadgeSpec b = { 0xE945, "Quick deploy", theme.ok, false }; badges[bn++] = b; }
    if (mine) { BadgeSpec b = { 0xE8F1, "This project", theme.accent, false }; badges[bn++] = b; }
    if (bn) { doc_badges(doc, ix, iw, badges, bn, theme.elevated); doc_space(doc, px(4)); }
    free(deploy_text);
    const Json *repo = json_get(site, "repository");
    labeled_field(doc, ix, iw, "URL", json_get(site, "url"));
    if (json_is_object(repo)) {
        const Json *name_v = json_get(repo, "url");
        if (!json_str_nonempty(name_v)) name_v = json_get(repo, "name");
        labeled_field(doc, ix, iw, "Repository", name_v);
        labeled_field(doc, ix, iw, "Branch", json_get(repo, "branch"));
        labeled_field(doc, ix, iw, "Provider", json_get(repo, "provider"));
    } else labeled_field(doc, ix, iw, "Repository", repo);
    labeled_field(doc, ix, iw, "PHP", json_get(site, "php_version"));
    labeled_field(doc, ix, iw, "App type", json_get(site, "app_type"));
    labeled_field(doc, ix, iw, "Web directory", json_get(site, "web_directory"));
    labeled_field(doc, ix, iw, "Forge id", json_get(site, "id"));
    const char *url = json_str(json_get(site, "url"));
    char *https = NULL;
    // Forge's `url` may come without a scheme; a bare domain opens over HTTPS.
    if (url && !safe_web_url(url) && !strstr(url, "://")) https = xstrfmt("https://%s", url);
    if (safe_web_url(https ? https : url)) {
        doc_space(doc, px(10));
        ButtonSpec b = { 0xE8A7, "Open site", BUTTON_BORDERED, p->base + A_OPEN_SITE, (intptr_t)index, true };
        doc_button_row(doc, ix, iw, &b, 1);
    }
    free(https);
    doc_box_end(doc, box, px(12));
}

/// The server on show and its sites, the project's first.
static void layout_server(ProjectForge *p, Doc *doc, int x, int w) {
    // A wide pane would spread each label and its value apart; the column stops at a reading width.
    if (w > px(820)) w = px(820);
    const Json *server = selected_server(p);
    if (!server) {
        if (p->loaded && json_count(p->servers)) doc_empty_state(doc, x, w, 0xE753, "No server picked", "Click a server on the left to see its Forge sites.");
        return;
    }
    const char *name = json_str_nonempty(json_get(server, "name"));
    doc_text(doc, x, w, name ? name : "Forge server", FONT_TITLE3, theme.ink, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
    doc_space(doc, px(4));
    char *php = field_text(json_get(server, "php_version")), *ubuntu = field_text(json_get(server, "ubuntu_version"));
    char *php_text = php ? xstrfmt("PHP %s", php) : NULL, *ubuntu_text = ubuntu ? xstrfmt("Ubuntu %s", ubuntu) : NULL;
    const char *parts[] = { json_str(json_get(server, "ip_address")), json_str(json_get(server, "provider")), json_str(json_get(server, "region")),
                            php_text, ubuntu_text, json_str(json_get(server, "database_type")), json_str(json_get(server, "_accountLabel")) };
    char *line = joined(parts, sizeof parts / sizeof *parts);
    doc_text(doc, x, w, line, FONT_FOOTNOTE, theme.muted, DT_WORDBREAK);
    free(line); free(php); free(ubuntu); free(php_text); free(ubuntu_text);
    if (json_bool_is(json_get(server, "is_ready"), false)) { doc_space(doc, px(4)); doc_text(doc, x, w, "Forge is still provisioning this server.", FONT_FOOTNOTE, theme.warn, DT_WORDBREAK); }
    doc_space(doc, px(14));
    const Json *sites = json_get(p->sites, p->selected);
    if (p->sites_error) { doc_notice(doc, x, w, p->sites_error); doc_space(doc, px(8)); }
    if (!json_is_array(sites)) {
        if (!p->sites_error) doc_loading(doc, x, w, "Loading sites\xE2\x80\xA6");
        return;
    }
    char *title = xstrfmt("%zu site%s", json_count(sites), json_count(sites) == 1 ? "" : "s");
    doc_text(doc, x, w, title, FONT_CAPTION_SEMIBOLD, theme.muted, DT_LEFT | DT_SINGLELINE);
    free(title);
    doc_space(doc, px(8));
    if (!json_count(sites)) { doc_text(doc, x, w, "No sites on this server.", FONT_FOOTNOTE, theme.muted, DT_LEFT); return; }
    // The project's own sites first, then the rest in Forge's order.
    for (int pass = 0; pass < 2; pass++)
        for (size_t i = 0; i < json_count(sites); i++) {
            const Json *site = json_at(sites, i);
            if (site_is_project(site, p->repo) != (pass == 0)) continue;
            layout_site(p, doc, x, w, site, i);
            doc_space(doc, px(10));
        }
}

void project_forge_layout(ProjectForge *p, Doc *doc, int w) {
    if (!project_forge_offered()) {
        doc_text(doc, 0, w, "The Forge servers are read with the Forge accounts in Settings, which needs an Admin token. Create one on the web dashboard under Settings \xE2\x86\x92 Devices and clients and connect with it.", FONT_FOOTNOTE, theme.muted, DT_WORDBREAK);
        return;
    }
    project_forge_load(p);
    RECT view = pane_content_rect(p->host->pane);
    int top = doc->y, bottom = top + (view.bottom - view.top) - top - px(14);
    if (bottom < top + px(240)) bottom = top + px(240);
    int lw = px(LIST_W);
    if (lw > w / 3) lw = w / 3;
    layout_list(p, doc, 0, lw);
    int list_bottom = doc->y;
    RECT rule = { lw + px(8), top, lw + px(9), bottom };
    doc_add(doc, &rule, paint_rule);
    int rx = lw + px(18), rw = w - rx;
    doc->y = top;
    layout_server(p, doc, rx, rw);
    int right_bottom = doc->y;
    doc->y = list_bottom > right_bottom ? list_bottom : right_bottom;
    if (doc->y < bottom) doc->y = bottom;
}

void project_forge_header(ProjectForge *p, HeaderInfo *info) {
    const Json *server = selected_server(p);
    if (!server) return;
    const char *name = json_str_nonempty(json_get(server, "name"));
    const Json *sites = json_get(p->sites, p->selected);
    char *detail = server_detail(server);
    if (json_is_array(sites)) snprintf(info->subtitle, sizeof info->subtitle, "Forge %s \xC2\xB7 %s \xC2\xB7 %zu site%s", name ? name : "server", detail, json_count(sites), json_count(sites) == 1 ? "" : "s");
    else snprintf(info->subtitle, sizeof info->subtitle, "Forge %s \xC2\xB7 %s", name ? name : "server", detail);
    free(detail);
}

// MARK: - Actions

bool project_forge_action(ProjectForge *p, int action, intptr_t arg, POINT pt) {
    (void)pt;
    if (action < p->base || action >= p->base + PROJECT_FORGE_ACTIONS) return false;
    switch (action - p->base) {
    case A_SERVER: {
        const Json *row = json_at(p->servers, (size_t)arg);
        if (!json_is_object(row)) break;
        char *key = server_key(row);
        if (!str_eq(key, p->selected)) {
            // The sites of the server left behind stop being read.
            request_cancel(&p->req_sites);
            json_free(p->incoming); p->incoming = NULL;
            set_string(&p->sites_error, NULL);
            free(p->selected); p->selected = key; key = NULL;
            sites_load(p);
        }
        free(key);
        relayout(p);
        break;
    }
    case A_OPEN_SITE: {
        const Json *site = json_at(json_get(p->sites, p->selected), (size_t)arg);
        const char *url = json_str(json_get(site, "url"));
        if (!url) break;
        if (!safe_web_url(url) && !strstr(url, "://")) { char *https = xstrfmt("https://%s", url); open_web_url(https); free(https); }
        else open_web_url(url);
        break;
    }
    }
    return true;
}

// MARK: - Lifetime

ProjectForge *project_forge_new(const char *repo, Screen *host, int action_base) {
    ProjectForge *p = xcalloc(1, sizeof *p);
    p->repo = xstrdup(repo ? repo : "");
    p->host = host; p->base = action_base;
    p->accounts = json_array(); p->servers = json_array(); p->sites = json_object();
    return p;
}
void project_forge_free(ProjectForge *p) {
    if (!p) return;
    request_cancel(&p->req); request_cancel(&p->req_sites);
    json_free(p->accounts); json_free(p->servers); json_free(p->sites); json_free(p->incoming);
    free(p->repo); free(p->error); free(p->selected); free(p->sites_error);
    free(p);
}
