#include "screen_settings_internal.h"

static void mcp_done(void *owner, Request *req);
static void mail_prepare(SettingsScreen *s);
static void mail_done(void *owner, Request *req);
static void settings_done(void *owner, Request *req);
static void ssh_done(void *owner, Request *req);
static void forge_done(void *owner, Request *req);
static void providers_done(void *owner, Request *req);
static void servers_done(void *owner, Request *req);
static void order_done(void *owner, Request *req);

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

void screen_settings_mcp_load(SettingsScreen *s) {
    if (s->mcp_account != g_store.client) {
        request_cancel(&s->req_mcp); api_client_release(s->mcp_account); s->mcp_account = api_client_retain(g_store.client);
        json_free(s->mcp); s->mcp = json_object(); s->mcp_loaded = false; set_string(&s->mcp_error, NULL);
    }
    if (s->req_mcp || !mcp_settings_supported("settings_mcp_servers")) return;
    store_call("settings_mcp_servers", json_object(), 0, s, mcp_done, 0, &s->req_mcp);
}

void settings_mcp_changed(void) {
    if (!screen_settings_g_settings) return;
    request_cancel(&screen_settings_g_settings->req_mcp); screen_settings_mcp_load(screen_settings_g_settings);
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

void screen_settings_mail_load(SettingsScreen *s) {
    if (!mail_settings_offered()) return;
    mail_prepare(s);
    if (s->req_mail || s->mail_loaded) return;
    store_call("settings_mail_accounts", json_object(), 0, s, mail_done, 0, &s->req_mail);
}

void settings_mail_changed(const Json *list) {
    if (!screen_settings_g_settings || !mail_settings_offered()) return;
    if (list && json_is_array(json_get(list, "accounts")) && screen_settings_g_settings->mail_client == g_store.client) {
        request_cancel(&screen_settings_g_settings->req_mail);
        json_free(screen_settings_g_settings->mail);
        screen_settings_g_settings->mail = json_clone(list);
        screen_settings_g_settings->mail_loaded = true;
        set_string(&screen_settings_g_settings->mail_error, NULL);
        pane_relayout(screen_settings_g_settings->base.pane);
        return;
    }
    request_cancel(&screen_settings_g_settings->req_mail);
    screen_settings_g_settings->mail_loaded = false;
    screen_settings_mail_load(screen_settings_g_settings);
}

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
    if (root && screen_settings_is_form_id(root->id)) return;
    if (json_count(screen_settings_settings_rows(s))) screen_settings_settings_open_row(s, 0);
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

void screen_settings_ssh_load(SettingsScreen *s) {
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

void screen_settings_forge_load(SettingsScreen *s) {
    if (s->req_forge || !store_supports("settings_forge_accounts")) { s->forge_loaded = true; return; }
    store_call("settings_forge_accounts", json_object(), 0, s, forge_done, 0, &s->req_forge);
}

void screen_settings_projects_load(SettingsScreen *s) {
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
    if (root && screen_settings_is_form_id(root->id)) return;
    if (json_count(screen_settings_provider_rows(s))) screen_settings_settings_open_provider(s, 0);
    else app_show_detail(provider_settings_screen_new(NULL, json_get(s->providers, "defaults")));
}

void screen_settings_providers_load(SettingsScreen *s) {
    if (s->req_providers || !store_supports("settings_providers")) { s->providers_loaded = true; return; }
    store_call("settings_providers", json_object(), 0, s, providers_done, 0, &s->req_providers);
}

void settings_providers_changed(bool open_first) {
    if (!screen_settings_g_settings) return;
    screen_settings_g_settings->open_first_provider = open_first;
    request_cancel(&screen_settings_g_settings->req_providers);
    screen_settings_providers_load(screen_settings_g_settings);
}

const Json *screen_settings_server_rows(SettingsScreen *s) { return json_get(s->servers, "list"); }

void screen_settings_servers_open_row(SettingsScreen *s, size_t index) {
    const Json *row = json_at(screen_settings_server_rows(s), index);
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
    if (root && screen_settings_is_form_id(root->id)) return;
    if (json_count(screen_settings_server_rows(s))) screen_settings_servers_open_row(s, 0);
    else app_show_detail(db_server_settings_screen_new(NULL, json_get(s->servers, "defaults")));
}

void screen_settings_servers_load(SettingsScreen *s) {
    if (s->req_servers || !store_supports("settings_db_servers")) { s->servers_loaded = true; return; }
    store_call("settings_db_servers", json_object(), 0, s, servers_done, 0, &s->req_servers);
}

void settings_db_servers_changed(bool open_first) {
    if (!screen_settings_g_settings) return;
    screen_settings_g_settings->open_first_server = open_first;
    request_cancel(&screen_settings_g_settings->req_servers);
    screen_settings_servers_load(screen_settings_g_settings);
}

const Json *settings_project_rows(void) { return screen_settings_g_settings ? screen_settings_settings_rows(screen_settings_g_settings) : NULL; }

const Json *settings_db_server_rows(void) { return screen_settings_g_settings ? screen_settings_server_rows(screen_settings_g_settings) : NULL; }

size_t settings_pool_capacity(void) {
    const Json *rows = settings_db_server_rows();
    size_t n = 0;
    for (size_t i = 0; i < json_count(rows); i++) if (json_bool_is(json_get(json_at(rows, i), "enabled"), true)) n++;
    return n;
}

void screen_settings_settings_load(SettingsScreen *s) { screen_settings_projects_load(s); screen_settings_ssh_load(s); screen_settings_forge_load(s); screen_settings_mcp_load(s); screen_settings_mail_load(s); }

void settings_projects_changed(int select_id) {
    (void)select_id;   // the form's own id is what the sidebar highlights
    if (!screen_settings_g_settings) return;
    request_cancel(&screen_settings_g_settings->req);
    screen_settings_projects_load(screen_settings_g_settings);
}

void screen_settings_settings_ssh_changed(void) {
    servers_ssh_changed();
    if (!screen_settings_g_settings) return;
    request_cancel(&screen_settings_g_settings->req_ssh);
    screen_settings_ssh_load(screen_settings_g_settings);
}

const Json *screen_settings_ssh_rows(SettingsScreen *s) { return json_get(s->ssh, "list"); }

void screen_settings_ssh_open_row(SettingsScreen *s, size_t index) {
    const Json *row = json_at(screen_settings_ssh_rows(s), index);
    if (json_is_object(row)) app_show_detail(screen_settings_ssh_settings_screen_new(row, json_get(s->ssh, "defaults")));
}

void screen_settings_settings_forge_changed(void) {
    if (!screen_settings_g_settings) return;
    request_cancel(&screen_settings_g_settings->req_forge);
    screen_settings_forge_load(screen_settings_g_settings);
}

const Json *screen_settings_forge_rows(SettingsScreen *s) { return json_get(s->forge, "list"); }

void screen_settings_forge_open_row(SettingsScreen *s, size_t index) {
    const Json *row = json_at(screen_settings_forge_rows(s), index);
    if (json_is_object(row)) app_show_detail(screen_settings_forge_settings_screen_new(row, json_get(s->forge, "defaults")));
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
void screen_settings_settings_move(SettingsScreen *s, size_t index, int delta) {
    const Json *rows = screen_settings_settings_rows(s);
    size_t n = json_count(rows);
    if (s->req_order || index >= n) return;
    if ((delta < 0 && index == 0) || (delta > 0 && index + 1 >= n)) return;
    size_t other = delta < 0 ? index - 1 : index + 1;
    Json *ids = json_array();
    for (size_t i = 0; i < n; i++) {
        size_t from = i == index ? other : i == other ? index : i;
        json_array_push(ids, json_number(screen_settings_row_id(json_at(rows, from))));
    }
    Json *args = json_object(); json_object_set(args, "ids", ids);
    store_call("order_projects", args, 0, s, order_done, 0, &s->req_order);
    pane_relayout(s->base.pane);   // the arrows are off until the server answers
}
