#include "project_forge_internal.h"

static double row_id(const Json *row);
static void relayout(ProjectForge *p);
static void servers_done(void *owner, Request *req);
static void servers_next(ProjectForge *p, const char *cursor);
static void accounts_done(void *owner, Request *req);
static void sites_done(void *owner, Request *req);
static void sites_next(ProjectForge *p, const Json *server, const char *cursor);
static void sites_load(ProjectForge *p);
static bool close_site(ProjectForge *p);

bool project_forge_offered(void) { return store_supports("settings_forge_accounts") && store_supports("forge_servers") && store_supports("forge_sites"); }

static double row_id(const Json *row) {
    double id;
    return json_num(json_get(row, "id"), &id) && isfinite(id) ? id : 0;
}

char *project_forge_server_key(const Json *server) { return xstrfmt("%.0f:%.0f", json_num_or(json_get(server, "_account"), 0), row_id(server)); }

const Json *project_forge_selected_server(ProjectForge *p) {
    if (!p->selected) return NULL;
    for (size_t i = 0; i < json_count(p->servers); i++) {
        char *key = project_forge_server_key(json_at(p->servers, i));
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
    if (!project_forge_selected_server(p) && json_count(p->servers)) {
        free(p->selected);
        p->selected = project_forge_server_key(json_at(p->servers, 0));
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

static void sites_done(void *owner, Request *req) {
    ProjectForge *p = owner;
    const Json *server = project_forge_selected_server(p);
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
    const Json *server = project_forge_selected_server(p);
    if (!server || p->req_sites || json_is_array(json_get(p->sites, p->selected))) return;
    set_string(&p->sites_error, NULL);
    json_free(p->incoming); p->incoming = json_array(); p->site_pages = 0;
    sites_next(p, server, NULL);
}

void project_forge_refresh(ProjectForge *p) {
    // With a site open, ⟳ reads the site's open tab again.
    if (p->site) { forge_site_refresh(p->site); return; }
    request_cancel(&p->req); request_cancel(&p->req_sites);
    json_free(p->incoming); p->incoming = NULL;
    json_free(p->sites); p->sites = json_object();
    set_string(&p->sites_error, NULL);
    p->loaded = false;
    project_forge_load(p);
}

void project_forge_header(ProjectForge *p, HeaderInfo *info) {
    const Json *server = project_forge_selected_server(p);
    if (!server) return;
    if (p->site) {
        char *detail = project_forge_server_detail(server);
        const char *name = json_str_nonempty(json_get(server, "name"));
        snprintf(info->subtitle, sizeof info->subtitle, "Forge %s \xC2\xB7 %s", name ? name : "server", detail);
        free(detail);
        forge_site_header(p->site, info);
        return;
    }
    const char *name = json_str_nonempty(json_get(server, "name"));
    const Json *sites = json_get(p->sites, p->selected);
    char *detail = project_forge_server_detail(server);
    if (json_is_array(sites)) snprintf(info->subtitle, sizeof info->subtitle, "Forge %s \xC2\xB7 %s \xC2\xB7 %zu site%s", name ? name : "server", detail, json_count(sites), json_count(sites) == 1 ? "" : "s");
    else snprintf(info->subtitle, sizeof info->subtitle, "Forge %s \xC2\xB7 %s", name ? name : "server", detail);
    free(detail);
}

// MARK: - Actions

/// Closes the open site, unless its unsaved changes are kept; true when none is open any more.
static bool close_site(ProjectForge *p) {
    if (!p->site) return true;
    if (!forge_site_can_leave(p->site)) return false;
    forge_site_free(p->site);
    p->site = NULL;
    return true;
}

bool project_forge_action(ProjectForge *p, int action, intptr_t arg, POINT pt) {
    (void)pt;
    if (action < p->base || action >= p->base + PROJECT_FORGE_ACTIONS) return false;
    if (forge_site_action(p->site, action, arg)) return true;
    switch (action - p->base) {
    case A_BACK: if (close_site(p)) { relayout(p); if (p->host->pane) pane_scroll_to_top(p->host->pane); } break;
    case A_SERVER: {
        const Json *row = json_at(p->servers, (size_t)arg);
        if (!json_is_object(row)) break;
        if (!close_site(p)) break;
        char *key = project_forge_server_key(row);
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
    case A_SITE_PAGE: {
        const Json *server = project_forge_selected_server(p), *site = json_at(json_get(p->sites, p->selected), (size_t)arg);
        if (!json_is_object(server) || !json_is_object(site) || !close_site(p)) break;
        p->site = forge_site_new(p->host, p->base + A_SITE_VIEW, json_num_or(json_get(server, "_account"), 0), server, site);
        relayout(p);
        if (p->host->pane) pane_scroll_to_top(p->host->pane);
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

void project_forge_place(ProjectForge *p, const RECT *content, int scroll_y, bool shown) {
    if (p->site) forge_site_place(p->site, content, scroll_y, shown);
}

bool project_forge_command(ProjectForge *p, int id, int code) { return forge_site_command(p->site, id, code); }

bool project_forge_key(ProjectForge *p, WPARAM vk, bool ctrl) { return forge_site_key(p->site, vk, ctrl); }

bool project_forge_can_leave(ProjectForge *p) { return forge_site_can_leave(p->site); }

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
    forge_site_free(p->site);
    json_free(p->accounts); json_free(p->servers); json_free(p->sites); json_free(p->incoming);
    free(p->repo); free(p->error); free(p->selected); free(p->sites_error);
    free(p);
}
