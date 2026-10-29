#include "screen_settings_internal.h"
#include "sleep.h"

static void sign_out(void);
static bool in_rect(const RECT *r, POINT pt);
static char *unavailable(const char *call, const char *path, const char *what, const char *manage);
static void settings_destroy(Screen *base);
static void settings_header(Screen *base, HeaderInfo *info);
static int settings_footer_height(Screen *base, int width);
static void settings_footer_paint(Screen *base, Canvas *cv, const RECT *rc);
static void settings_footer_click(Screen *base, POINT pt);
static void settings_back(SettingsScreen *s);
static void settings_action(Screen *base, int action, intptr_t arg, POINT pt);
static void settings_context(Screen *base, int action, intptr_t arg, POINT pt);
static void settings_visible(Screen *base, bool shown);
static void settings_refresh(Screen *base);
static bool settings_key(Screen *base, WPARAM vk, bool ctrl, bool shift);

// Settings, as the dashboard's settings page: a sidebar of its own (Back to sessions, the mail accounts, the projects, the
// providers, the database pool, the SSH servers, the Forge accounts, each with ＋ New) and a project's form, its
// sections as tabs, or an SSH server's or a Forge account's on one tab, saved through
// /settings/projects, /settings/ssh/servers, /settings/forge/accounts. The provider form is screen_provider_settings.c and a
// database server's screen_db_servers.c. Those routes need an Admin token; any other token gets a sentence saying so.

static void sign_out(void) {
    if (!app_confirm("Sign out of this dashboard?", "The device token and the saved conversations are removed from this computer. Revoke the token itself in web Settings.", "Sign out", true)) return;
    store_forget();
}

static bool in_rect(const RECT *r, POINT pt) { return pt.x >= r->left && pt.x < r->right && pt.y >= r->top && pt.y < r->bottom; }

int screen_settings_row_id(const Json *row) { return json_int_or(json_get(row, "id"), 0); }

char *screen_settings_form_id(int id) { return id > 0 ? xstrfmt("settings-project:%d", id) : xstrdup("settings-project:new"); }

/// An SSH server's id is the time it was registered in milliseconds, past what an int holds.
char *screen_settings_ssh_form_id(double id) { return id > 0 ? xstrfmt("settings-ssh:%.0f", id) : xstrdup("settings-ssh:new"); }

double screen_settings_ssh_row_id(const Json *row) { double id; return json_num(json_get(row, "id"), &id) && isfinite(id) ? id : 0; }

/// A Forge account's id is, like an SSH server's, the time it was added in milliseconds.
char *screen_settings_forge_form_id(double id) { return id > 0 ? xstrfmt("settings-forge:%.0f", id) : xstrdup("settings-forge:new"); }

/// A settings form in the detail pane: a project's, a provider's, a database server's, an SSH server's, a Forge account's,
/// or the meeting assistant's.
bool screen_settings_is_form_id(const char *id) {
    return id && (str_has_prefix(id, "settings-project:") || str_has_prefix(id, "settings-provider:") || str_has_prefix(id, "settings-db:")
                  || str_has_prefix(id, "settings-ssh:") || str_has_prefix(id, "settings-forge:")
                  || str_has_prefix(id, "settings-mcp:") || str_eq(id, "settings-home") || str_eq(id, "settings-meeting") || str_eq(id, "mail-settings") || str_has_prefix(id, "mail-settings:"));
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

char *screen_settings_settings_unavailable(void) { return unavailable("settings_projects", "settings/projects", "Project settings", "projects"); }

char *screen_settings_ssh_unavailable(void) { return unavailable("settings_ssh_servers", "settings/ssh/servers", "SSH servers", "them"); }

/// The settings sidebar on screen, for a form to tell when the list changed.
SettingsScreen *screen_settings_g_settings;

static void settings_destroy(Screen *base) {
    SettingsScreen *s = (SettingsScreen *)base;
    if (screen_settings_g_settings == s) screen_settings_g_settings = NULL;
    request_cancel(&s->req); request_cancel(&s->req_order); request_cancel(&s->req_providers); request_cancel(&s->req_servers);
    request_cancel(&s->req_ssh); request_cancel(&s->req_forge);
    json_free(s->projects); free(s->error);
    json_free(s->providers); free(s->providers_error);
    json_free(s->servers); free(s->servers_error);
    json_free(s->ssh); free(s->ssh_error); api_client_release(s->ssh_account); free(s->ssh_device);
    json_free(s->forge); free(s->forge_error);
    request_cancel(&s->req_mcp); json_free(s->mcp); free(s->mcp_error); api_client_release(s->mcp_account);
    request_cancel(&s->req_mail); json_free(s->mail); free(s->mail_error); api_client_release(s->mail_client);
    screen_release(base);
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
    if (root && screen_settings_is_form_id(root->id)) {
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
    case ACT_OPEN_PROJECT: screen_settings_settings_open_row(s, (size_t)arg); break;
    case ACT_NEW_PROVIDER: app_show_detail(provider_settings_screen_new(NULL, json_get(s->providers, "defaults"))); break;
    case ACT_OPEN_PROVIDER: screen_settings_settings_open_provider(s, (size_t)arg); break;
    case ACT_NEW_SERVER: app_show_detail(db_server_settings_screen_new(NULL, json_get(s->servers, "defaults"))); break;
    case ACT_OPEN_SERVER: screen_settings_servers_open_row(s, (size_t)arg); break;
    case ACT_NEW_SSH:
        if (!s->ssh_loaded || s->ssh_account != g_store.client || !str_eq(s->ssh_device, g_store.device.id)) break;
        app_show_detail(screen_settings_ssh_settings_screen_new(NULL, json_get(s->ssh, "defaults"))); break;
    case ACT_OPEN_SSH: screen_settings_ssh_open_row(s, (size_t)arg); break;
    case ACT_NEW_FORGE: app_show_detail(screen_settings_forge_settings_screen_new(NULL, json_get(s->forge, "defaults"))); break;
    case ACT_OPEN_FORGE: screen_settings_forge_open_row(s, (size_t)arg); break;
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
    case ACT_OPEN_HOME: if (home_assistant_offered()) app_show_detail(home_assistant_screen_new()); break;
    case ACT_SLEEP: sleep_settings_action(); pane_relayout(base->pane); break;
    case ACT_OPEN_MEETING: app_show_detail(meeting_settings_screen_new()); break;
    case ACT_MOVE_UP: screen_settings_settings_move(s, (size_t)arg, -1); break;
    case ACT_MOVE_DOWN: screen_settings_settings_move(s, (size_t)arg, 1); break;
    }
}

static void settings_context(Screen *base, int action, intptr_t arg, POINT pt) {
    SettingsScreen *s = (SettingsScreen *)base;
    if (action != ACT_OPEN_PROJECT || !store_supports("order_projects")) return;
    size_t n = json_count(screen_settings_settings_rows(s)), i = (size_t)arg;
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING | (i > 0 && !s->req_order ? 0 : MF_GRAYED), MENU_UP, L"Move up");
    AppendMenuW(menu, MF_STRING | (i + 1 < n && !s->req_order ? 0 : MF_GRAYED), MENU_DOWN, L"Move down");
    int chosen = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, pt.x, pt.y, 0, pane_hwnd(base->pane), NULL);
    DestroyMenu(menu);
    if (chosen == MENU_UP) screen_settings_settings_move(s, i, -1);
    else if (chosen == MENU_DOWN) screen_settings_settings_move(s, i, 1);
}

static void settings_visible(Screen *base, bool shown) {
    SettingsScreen *s = (SettingsScreen *)base;
    if (!shown) return;
    if (!s->loaded && !s->req) screen_settings_projects_load(s);
    if (!s->providers_loaded && !s->req_providers) screen_settings_providers_load(s);
    if (!s->servers_loaded && !s->req_servers) screen_settings_servers_load(s);
    if (!s->ssh_loaded || s->ssh_account != g_store.client || !str_eq(s->ssh_device, g_store.device.id)) screen_settings_ssh_load(s);
    if (!s->forge_loaded && !s->req_forge) screen_settings_forge_load(s);
    if (!s->mcp_loaded || s->mcp_account != g_store.client) screen_settings_mcp_load(s);
    if (mail_settings_offered() && (!s->mail_loaded || s->mail_client != g_store.client)) screen_settings_mail_load(s);
}

static void settings_refresh(Screen *base) {
    SettingsScreen *s = (SettingsScreen *)base;
    request_cancel(&s->req); request_cancel(&s->req_ssh); request_cancel(&s->req_forge); request_cancel(&s->req_mcp);
    request_cancel(&s->req_mail); s->mail_loaded = false; screen_settings_settings_load(s);
    request_cancel(&s->req_providers); screen_settings_providers_load(s);
    request_cancel(&s->req_servers); screen_settings_servers_load(s);
}

static bool settings_key(Screen *base, WPARAM vk, bool ctrl, bool shift) {
    (void)ctrl; (void)shift;
    // Backspace and Escape leave Settings as ← Back to sessions does, so a form with changes is asked first.
    if (vk == VK_BACK || vk == VK_ESCAPE) { settings_back((SettingsScreen *)base); return true; }
    return false;
}

static const ScreenVTable settings_vt = {
    .destroy = settings_destroy, .layout = screen_settings_settings_layout, .header = settings_header, .action = settings_action,
    .context = settings_context, .visible = settings_visible, .refresh = settings_refresh, .key = settings_key,
    .footer_height = settings_footer_height, .footer_paint = settings_footer_paint, .footer_click = settings_footer_click,
};

void settings_select_id(const char *id) {
    if (screen_settings_g_settings && screen_settings_g_settings->base.pane) pane_set_selected_id(screen_settings_g_settings->base.pane, id);
}

Screen *settings_screen_new(void) {
    SettingsScreen *s = xcalloc(1, sizeof *s);
    s->base.vt = &settings_vt; s->base.id = xstrdup("settings");
    s->projects = json_object(); s->ssh = json_object(); s->forge = json_object();
    s->providers = json_object();
    s->servers = json_object();
    s->mcp = json_object();
    s->mail = json_object();
    screen_settings_g_settings = s;
    return &s->base;
}

const FieldDef screen_settings_FIELDS[F_COUNT] = {
    [F_REPO] = { "repo", K_TEXT, "Repository", "owner/name", "Cloned over HTTPS with the machine's own git credentials.", 0, false },
    [F_LABEL] = { "label", K_TEXT, "Label", "shown in the project dropdown", NULL, 0, false },
    [F_ENABLED] = { "enabled", K_BOOL, "Active: sessions can be started on this project" },
    [F_MAIL_TOOLS] = { "mailToolsEnabled", K_BOOL, "Allow sessions to access connected email accounts" },
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
    [F_BABYSIT] = { "babysitPrs", K_BOOL, "Babysit pull requests" },
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

const RuntimeDef screen_settings_RUNTIMES[R_COUNT] = {
    [R_REVIEW] = { "reviewProviderId", "reviewModel", "reviewEffort", NULL, "Pick a provider", "Provider" },
    [R_TEST_SHEET] = { NULL, NULL, NULL, "testSheet", "Same as the code review", "Provider" },
    [R_TEST_RUN] = { NULL, NULL, NULL, "testRun", "Same as the code review", "Provider" },
    [R_WORKER] = { "workerProviderId", "workerModel", "workerEffort", NULL, "Same as the orchestrator", "Worker provider" },
};

/// The open tab stays open from one project to the next, so comparing a setting across projects is one click each.
int screen_settings_g_tab;

const FieldDef screen_settings_SSH_FIELDS[S_COUNT] = {
    [S_LABEL] = { "label", K_TEXT, "Label", "Production web server", "Leave empty to name it user@host:port.", 0, false },
    [S_HOST] = { "host", K_TEXT, "Host", "server.example.com", NULL, 0, true },
    [S_PORT] = { "port", K_NUMBER, "Port", "22", NULL, 0, true },
    [S_USER] = { "username", K_TEXT, "Username", "deploy", NULL, 0, true },
    [S_KEY] = { "identityFile", K_TEXT, "Private key path", "/home/you/.ssh/id_ed25519",
        "Absolute path on the machine running Briareus; leave empty for its default SSH keys or agent. Not used while an SSH password is stored.", 0, true },
    [S_PASSWORD] = { "password", K_TEXT, "Password", "empty = key authentication", "Stored encrypted and never shown again. Leave empty to keep the stored password.", 0, true },
    [S_SUDO_PASSWORD] = { "sudoPassword", K_TEXT, "Sudo password", "empty = no sudo", "Separate from SSH login. Root commands follow the permission mode, including running without approval in Don't ask anything mode.", 0, true },
    [S_DBHOST] = { "dbHost", K_TEXT, "Database host", "127.0.0.1", "Where the database listens, as seen from this server itself.", 0, true },
    [S_DBPORT] = { "dbPort", K_NUMBER, "Database port", "3306", NULL, 0, true },
    [S_DBUSER] = { "dbUsername", K_TEXT, "Database username", "app", "Empty removes the stored login.", 0, true },
    [S_DBPASS] = { "dbPassword", K_TEXT, "Database password", "", NULL, 0, true },
};

int screen_settings_g_ssh_tab;
