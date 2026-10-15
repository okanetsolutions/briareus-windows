#include "screen_settings_internal.h"

static void layout_mcp(SettingsScreen *s, Doc *doc, int w, const char *selected);
static void layout_ssh(SettingsScreen *s, Doc *doc, int w, const char *selected);
static void layout_forge(SettingsScreen *s, Doc *doc, int w, const char *selected);
static void layout_mail(SettingsScreen *s, Doc *doc, int w, const char *selected);
static void layout_servers(SettingsScreen *s, Doc *doc, int w, const char *selected);
static void layout_providers(SettingsScreen *s, Doc *doc, int w, const char *selected);
static void layout_projects(SettingsScreen *s, Doc *doc, int w, const char *selected);

static void layout_mcp(SettingsScreen *s, Doc *doc, int w, const char *selected) {
    doc_space(doc, px(18));
    screen_settings_section_title(doc, w, "MCP servers", NULL, mcp_settings_supported("create_mcp_server") && s->mcp_account == g_store.client && s->mcp_loaded && !s->mcp_error ? ACT_NEW_MCP : 0);
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
        doc_custom(doc, 0, w, px(52), screen_settings_paint_project_row, d, screen_settings_project_row_free, ACT_OPEN_MCP, (intptr_t)i);
        free(screen_id);
    }
    if (!json_count(rows) && !s->mcp_error) doc_text(doc, px(8), w - px(16), "No MCP servers yet.", FONT_FOOTNOTE, theme.muted, DT_WORDBREAK);
}

/// The SSH servers under the projects, as the web's settings sidebar lists them: each with its dot, label and project.
static void layout_ssh(SettingsScreen *s, Doc *doc, int w, const char *selected) {
    doc_space(doc, px(8));
    char *why = screen_settings_ssh_unavailable();
    screen_settings_section_title(doc, w, "SSH servers", NULL, why ? 0 : ACT_NEW_SSH);
    if (why) { doc_text(doc, px(8), w - px(16), why, FONT_FOOTNOTE, theme.muted, DT_LEFT | DT_WORDBREAK); free(why); return; }
    if (s->ssh_error) { doc_notice(doc, px(8), w - px(16), s->ssh_error); doc_space(doc, px(8)); }
    const Json *rows = screen_settings_ssh_rows(s);
    int row_h = px(6) + px(22) + px(18) + px(6);
    for (size_t i = 0; i < json_count(rows); i++) {
        const Json *row = json_at(rows, i);
        ProjectRowData *d = xcalloc(1, sizeof *d);
        const char *label = json_str_nonempty(json_get(row, "label")), *repo = json_str(json_get(row, "repo"));
        d->label = xstrdup(label ? label : json_str_nonempty(json_get(row, "host")) ? json_str(json_get(row, "host")) : "SSH server");
        d->repo = xstrdup(repo ? repo : "");
        d->enabled = !json_bool_is(json_get(row, "enabled"), false);
        char *id = screen_settings_ssh_form_id(screen_settings_ssh_row_id(row));
        d->selected = str_eq(selected, id);
        free(id);
        doc_custom(doc, 0, w, row_h, screen_settings_paint_project_row, d, screen_settings_project_row_free, ACT_OPEN_SSH, (intptr_t)i);
    }
    // A server being registered shows as its own row until it is saved.
    if (str_eq(selected, "settings-ssh:new")) {
        ProjectRowData *d = xcalloc(1, sizeof *d);
        d->label = xstrdup("New SSH server"); d->repo = xstrdup("not saved yet"); d->selected = true;
        doc_custom(doc, 0, w, row_h, screen_settings_paint_project_row, d, screen_settings_project_row_free, 0, 0);
    }
    if (s->ssh_loaded && !json_count(rows) && !s->ssh_error) doc_text(doc, px(8), w - px(16), "No SSH servers registered. \xEF\xBC\x8B New lets a project's sessions run commands on one, with approval.", FONT_FOOTNOTE, theme.muted, DT_WORDBREAK);
    if (!s->ssh_loaded) doc_loading(doc, 0, w, "Loading SSH servers\xE2\x80\xA6");
}

/// The Forge accounts under the SSH servers: each with its dot (a token is stored), label, organization and projects.
static void layout_forge(SettingsScreen *s, Doc *doc, int w, const char *selected) {
    doc_space(doc, px(8));
    screen_settings_section_title(doc, w, "Forge accounts", NULL, store_supports("create_forge_account") ? ACT_NEW_FORGE : 0);
    if (s->forge_error) { doc_notice(doc, px(8), w - px(16), s->forge_error); doc_space(doc, px(8)); }
    const Json *rows = screen_settings_forge_rows(s);
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
        char *id = screen_settings_forge_form_id(screen_settings_ssh_row_id(row));
        d->selected = str_eq(selected, id);
        free(id);
        doc_custom(doc, 0, w, row_h, screen_settings_paint_project_row, d, screen_settings_project_row_free, ACT_OPEN_FORGE, (intptr_t)i);
    }
    // An account being added shows as its own row until it is saved.
    if (str_eq(selected, "settings-forge:new")) {
        ProjectRowData *d = xcalloc(1, sizeof *d);
        d->label = xstrdup("New Forge account"); d->repo = xstrdup("not saved yet"); d->selected = true;
        doc_custom(doc, 0, w, row_h, screen_settings_paint_project_row, d, screen_settings_project_row_free, 0, 0);
    }
    if (s->forge_loaded && !json_count(rows) && !s->forge_error) doc_text(doc, px(8), w - px(16), "No Forge accounts yet. \xEF\xBC\x8B New adds a Laravel Forge organization and the projects that may use it.", FONT_FOOTNOTE, theme.muted, DT_WORDBREAK);
    if (!s->forge_loaded) doc_loading(doc, 0, w, "Loading Forge accounts\xE2\x80\xA6");
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
    screen_settings_section_title(doc, w, "Mail", NULL, can_new ? ACT_NEW_MAIL : 0);
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
        doc_custom(doc, 0, w, row_h, screen_settings_paint_project_row, d, screen_settings_project_row_free, ACT_OPEN_MAIL, (intptr_t)i);
    }
    if (str_eq(selected, "mail-settings")) {
        ProjectRowData *d = xcalloc(1, sizeof *d);
        d->label = xstrdup("New mailbox"); d->repo = xstrdup("not connected yet"); d->selected = true;
        doc_custom(doc, 0, w, row_h, screen_settings_paint_project_row, d, screen_settings_project_row_free, 0, 0);
    }
    if (!json_count(rows) && !s->mail_error) doc_text(doc, px(8), w - px(16), "No connected mailboxes yet. \xEF\xBC\x8B New signs in to Gmail or Outlook.", FONT_FOOTNOTE, theme.muted, DT_WORDBREAK);
}

void screen_settings_settings_layout(Screen *base, Doc *doc) {
    SettingsScreen *s = (SettingsScreen *)base;
    int w = doc->width;
    const char *selected = pane_selected_id(base->pane);
    doc_space(doc, px(8));
    doc_custom(doc, 0, w, px(24), screen_settings_paint_back, NULL, NULL, ACT_BACK, 0);
    doc_space(doc, px(16));
    // This computer's own settings come first: they need no Admin token.
    screen_settings_section_title(doc, w, "This computer", NULL, 0);
    ProjectRowData *meeting = xcalloc(1, sizeof *meeting);
    meeting->label = xstrdup("\xF0\x9F\x8E\x99 Meeting assistant");
    meeting->repo = xstrdup(meeting_has_key(MEETING_KEY_ELEVENLABS) ? "ElevenLabs API key saved" : "add an ElevenLabs API key");
    meeting->enabled = meeting_has_key(MEETING_KEY_ELEVENLABS);
    meeting->selected = str_eq(selected, "settings-meeting");
    doc_custom(doc, 0, w, px(6) + px(22) + px(18) + px(6), screen_settings_paint_project_row, meeting, screen_settings_project_row_free, ACT_OPEN_MEETING, 0);
    doc_space(doc, px(16));
    if (home_assistant_offered()) {
        doc_button(doc, 0, w, "Smart home", BUTTON_BORDERED, ACT_OPEN_HOME, 0, true);
        doc_space(doc, px(16));
    }
    if (mail_settings_offered()) { layout_mail(s, doc, w, selected); doc_space(doc, px(16)); }
    char *why = screen_settings_settings_unavailable();
    screen_settings_section_title(doc, w, "Projects", NULL, why ? 0 : ACT_NEW_PROJECT);
    if (why) { doc_text(doc, px(8), w - px(16), why, FONT_FOOTNOTE, theme.muted, DT_LEFT | DT_WORDBREAK); free(why); layout_ssh(s, doc, w, selected); if (mcp_settings_supported("settings_mcp_servers")) layout_mcp(s, doc, w, selected); doc_space(doc, px(8)); return; }
    layout_projects(s, doc, w, selected);
    // The providers sessions start on, then the database pool, below the projects as on the dashboard; a server without
    // the routes shows neither.
    if (store_supports("settings_providers")) {
        doc_space(doc, px(8));
        screen_settings_section_title(doc, w, "Providers", NULL, store_supports("create_provider") ? ACT_NEW_PROVIDER : 0);
        layout_providers(s, doc, w, selected);
    }
    if (store_supports("settings_db_servers")) {
        doc_space(doc, px(8));
        // One open session with a database per server in the pool, so the pool's size heads the section, as the sessions
        // it lets run at once (the dashboard's "· 2 parallel sessions with a database", cut to the sidebar's width).
        size_t n = settings_pool_capacity();
        char *screen_settings_note = n ? xstrfmt("\xC2\xB7 %zu session%s", n, n == 1 ? "" : "s") : NULL;
        screen_settings_section_title(doc, w, "Database pool", screen_settings_note, store_supports("create_db_server") ? ACT_NEW_SERVER : 0);
        free(screen_settings_note);
        layout_servers(s, doc, w, selected);
    }
    // The SSH servers agents may run commands on, as on the dashboard, then the Forge accounts on a server that has them.
    layout_ssh(s, doc, w, selected);
    if (store_supports("settings_forge_accounts")) layout_forge(s, doc, w, selected);
    if (mcp_settings_supported("settings_mcp_servers")) layout_mcp(s, doc, w, selected);
}

/// The database pool: each server with its dot and host:port.
static void layout_servers(SettingsScreen *s, Doc *doc, int w, const char *selected) {
    if (s->servers_error) { doc_notice(doc, px(8), w - px(16), s->servers_error); doc_space(doc, px(8)); }
    const Json *rows = screen_settings_server_rows(s);
    int h = px(6) + px(22) + px(18) + px(6);
    for (size_t i = 0; i < json_count(rows); i++) {
        const Json *row = json_at(rows, i);
        ProjectRowData *d = xcalloc(1, sizeof *d);
        const char *host = json_str(json_get(row, "host"));
        d->repo = xstrfmt("%s:%d", host ? host : "", json_int_or(json_get(row, "port"), 0));
        d->label = xstrdup(json_str_nonempty(json_get(row, "label")) ? json_str(json_get(row, "label")) : d->repo);
        d->enabled = json_bool_is(json_get(row, "enabled"), true);
        char *id = xstrfmt("settings-db:%d", screen_settings_row_id(row));
        d->selected = str_eq(selected, id);
        free(id);
        doc_custom(doc, 0, w, h, screen_settings_paint_project_row, d, screen_settings_project_row_free, ACT_OPEN_SERVER, (intptr_t)i);
    }
    if (str_eq(selected, "settings-db:new")) {
        ProjectRowData *d = xcalloc(1, sizeof *d);
        d->label = xstrdup("New database server"); d->repo = xstrdup("not saved yet"); d->selected = true;
        doc_custom(doc, 0, w, h, screen_settings_paint_project_row, d, screen_settings_project_row_free, 0, 0);
    }
    if (s->servers_loaded && !json_count(rows) && !s->servers_error) doc_text(doc, px(8), w - px(16), "No servers yet. Add one so sessions can claim a database of their own.", FONT_FOOTNOTE, theme.muted, DT_WORDBREAK);
    if (!s->servers_loaded) doc_loading(doc, 0, w, "Loading the database pool\xE2\x80\xA6");
    doc_space(doc, px(8));
}

static void layout_providers(SettingsScreen *s, Doc *doc, int w, const char *selected) {
    if (s->providers_error) { doc_notice(doc, px(8), w - px(16), s->providers_error); doc_space(doc, px(8)); }
    const Json *rows = screen_settings_provider_rows(s);
    int h = px(6) + px(22) + px(18) + px(6);
    for (size_t i = 0; i < json_count(rows); i++) {
        const Json *row = json_at(rows, i);
        ProviderRowData *d = xcalloc(1, sizeof *d);
        const char *label = json_str_nonempty(json_get(row, "label")), *binary = json_str(json_get(row, "binary"));
        d->label = label ? xstrdup(label) : xstrfmt("Provider #%d", screen_settings_row_id(row));
        d->binary = xstrdup(binary ? binary : "");
        d->active = !json_bool_is(json_get(row, "active"), false);
        d->login = json_bool_is(json_get(row, "hasLogin"), true);
        d->endpoint = json_str_nonempty(json_get(row, "baseUrl")) != NULL;
        char *id = xstrfmt("settings-provider:%d", screen_settings_row_id(row));
        d->selected = str_eq(selected, id);
        free(id);
        doc_custom(doc, 0, w, h, screen_settings_paint_provider_row, d, screen_settings_provider_row_free, ACT_OPEN_PROVIDER, (intptr_t)i);
    }
    if (str_eq(selected, "settings-provider:new")) {
        ProviderRowData *d = xcalloc(1, sizeof *d);
        d->label = xstrdup("New provider"); d->binary = xstrdup("not saved yet"); d->unsaved = true; d->selected = true;
        doc_custom(doc, 0, w, h, screen_settings_paint_provider_row, d, screen_settings_provider_row_free, 0, 0);
    }
    if (s->providers_loaded && !json_count(rows) && !s->providers_error) doc_text(doc, px(8), w - px(16), "No providers yet. Add one so sessions can be started.", FONT_FOOTNOTE, theme.muted, DT_WORDBREAK);
    if (!s->providers_loaded) doc_loading(doc, 0, w, "Loading providers\xE2\x80\xA6");
    doc_space(doc, px(8));
}

static void layout_projects(SettingsScreen *s, Doc *doc, int w, const char *selected) {
    if (s->error) { doc_notice(doc, px(8), w - px(16), s->error); doc_space(doc, px(8)); }
    const Json *rows = screen_settings_settings_rows(s);
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
        char *id = screen_settings_form_id(screen_settings_row_id(row));
        d->selected = str_eq(selected, id);
        free(id);
        d->movable = movable;
        bool opened = d->selected;
        int item = doc_custom(doc, 0, w, px(6) + px(22) + px(18) + px(6), screen_settings_paint_project_row, d, screen_settings_project_row_free, ACT_OPEN_PROJECT, (intptr_t)i);
        if (movable) screen_settings_layout_move_arrows(s, doc, item, i, json_count(rows), opened);
    }
    // A project being added shows as its own row until it is saved.
    if (str_eq(selected, "settings-project:new")) {
        ProjectRowData *d = xcalloc(1, sizeof *d);
        d->label = xstrdup("New project"); d->repo = xstrdup("not saved yet"); d->selected = true;
        doc_custom(doc, 0, w, px(6) + px(22) + px(18) + px(6), screen_settings_paint_project_row, d, screen_settings_project_row_free, 0, 0);
    }
    if (s->loaded && !json_count(rows) && !s->error) doc_text(doc, px(8), w - px(16), "No projects yet. \xEF\xBC\x8B New adds a repository sessions can be started against.", FONT_FOOTNOTE, theme.muted, DT_WORDBREAK);
    if (!s->loaded) doc_loading(doc, 0, w, "Loading projects\xE2\x80\xA6");
    doc_space(doc, px(8));
}
