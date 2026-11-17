#include "screen_fixture.h"
#include "screen_settings_internal.h"
#include "suites.h"

static Route ROUTES[] = {
    {"GET", "/github/account", "read"}, {"GET", "/settings/github/health", "admin"},
    {"GET", "/projects", "read"}, {"GET", "/settings/projects", "admin"},
    {"POST", "/settings/projects", "admin"},
};
static const char *IDENTITY = "{\"account\":{\"id\":7,\"label\":\"Production\",\"login\":\"fixture-bot\",\"globalFallback\":false,\"hasToken\":true}}";
static const char *FALLBACK = "{\"account\":{\"id\":null,\"label\":\"GITHUB_TOKEN\",\"login\":null,\"globalFallback\":true,\"hasToken\":true}}";
static const char *HEALTH = "{\"account\":{\"label\":\"Production\",\"login\":\"fixture-bot\",\"globalFallback\":false},\"healthy\":false,"
    "\"token\":{\"status\":\"valid\",\"httpStatus\":200},\"scopes\":{\"status\":\"unknown\",\"granted\":null,\"required\":[\"repo\"],\"missing\":null},"
    "\"repos\":[{\"repo\":\"o/r\",\"status\":\"ok\",\"accessible\":true,\"httpStatus\":200},{\"repo\":\"o/denied\",\"status\":\"no_access\",\"accessible\":false,\"httpStatus\":403}],"
    "\"rateLimit\":{\"core\":{\"remaining\":4997,\"limit\":5000,\"resetAt\":1700000000000},\"graphql\":{\"remaining\":4000,\"limit\":5000,\"resetAt\":1700000060000},\"cooldownUntil\":{\"core\":0,\"graphql\":0}}}";

typedef struct { ScreenFixture fixture; FormScreen *form; Doc doc; } Form;

static bool transport(void *ctx, const char *method, const char *url, const char *const *headers,
    const void *body, size_t bytes, int timeout, int *status, char **type, char **retry,
    char **response, size_t *len, char **error) {
    ScreenFixture *f = ctx;
    if (str_eq(method, "GET")) { set_string(&f->method, method); set_string(&f->url, url); }
    return screen_fixture_transport(ctx, method, url, headers, body, bytes, timeout, status, type, retry, response, len, error);
}

static void setup(Form *f, const char *permission, const char *identity) {
    memset(f, 0, sizeof *f);
    screen_fixture_init(&f->fixture, ROUTES, sizeof ROUTES / sizeof *ROUTES);
    api_client_set_transport(f->fixture.client, transport, &f->fixture);
    g_store.device.permission = (char *)permission;
    Json *row = json_parsez(str_eq(permission, "admin") ? "{\"id\":4,\"repo\":\"o/r\"}" : "{\"repo\":\"o/r\"}");
    f->form = (FormScreen *)project_settings_screen_new(row, NULL); json_free(row);
    screen_fixture_pane(&f->fixture, &f->form->base); doc_init(&f->doc);
    f->fixture.read_response = identity;
    f->form->base.vt->visible(&f->form->base, true);
    screen_settings_g_tab = T_PROJECT;
    if (store_supports("github_account")) screen_fixture_pump(&f->fixture, 1);
}
static void cleanup(Form *f) {
    f->form->base.vt->destroy(&f->form->base); doc_free(&f->doc); screen_fixture_clear(&f->fixture);
    screen_settings_g_tab = T_PROJECT;
}
static bool has_action(Form *f, int action) {
    screen_fixture_draw(&f->form->base, &f->doc);
    for (size_t i = 0; i < f->doc.count; i++) if (f->doc.items[i].action == action) return true;
    return false;
}
static bool contains(Form *f, const char *value) {
    screen_fixture_draw(&f->form->base, &f->doc);
    for (size_t i = 0; i < f->doc.count; i++) {
        const char *text = f->doc.items[i].text;
        if (text && strstr(text, value)) return true;
    }
    return false;
}
static void check_health(Form *f, const char *response) {
    f->fixture.read_response = response;
    f->form->base.vt->action(&f->form->base, ACT_PROJECT_GITHUB_CHECK, 0, (POINT){0});
    screen_fixture_pump(&f->fixture, 1);
    CHECK_STR(f->fixture.method, "GET"); CHECK_STR(f->fixture.url, "https://example.com/api/v1/settings/github/health?repo=o%2Fr");
    CHECK_INT(f->fixture.writes, 0);
}

static void identity_read_permissions_and_fallback(void) {
    Form f; setup(&f, "read", FALLBACK);
    CHECK(f.form->read_only); CHECK_STR(f.form->base.id, "settings-project:o/r");
    CHECK_STR(f.fixture.url, "https://example.com/api/v1/github/account?repo=o%2Fr");
    CHECK(json_bool_is(json_get(f.form->github.account, "globalFallback"), true));
    CHECK(contains(&f, "GITHUB_TOKEN")); CHECK(f.form->edits[F_REPO] == NULL);
    CHECK(!has_action(&f, ACT_PROJECT_GITHUB_CHECK));
    f.form->base.vt->action(&f.form->base, ACT_PROJECT_GITHUB_CHECK, 0, (POINT){0});
    CHECK_INT(f.fixture.reads, 1); CHECK_INT(f.fixture.writes, 0);
    HeaderInfo header = {0}; f.form->base.vt->header(&f.form->base, &header); CHECK_INT(header.button_count, 0);
    CHECK(screen_settings_project_accounts_offered());
    cleanup(&f);
    setup(&f, "admin", IDENTITY);
    CHECK(contains(&f, "fixture-bot")); CHECK(contains(&f, "Production"));
    CHECK(has_action(&f, ACT_PROJECT_GITHUB_CHECK)); cleanup(&f);
    // An older server hides account identity and diagnostics, and issues neither read.
    screen_fixture_init(&f.fixture, ROUTES + 3, 1);
    Json *row = json_parsez("{\"id\":4,\"repo\":\"o/r\"}");
    f.form = (FormScreen *)project_settings_screen_new(row, NULL); json_free(row);
    screen_fixture_pane(&f.fixture, &f.form->base); doc_init(&f.doc);
    CHECK(!has_action(&f, ACT_PROJECT_GITHUB_CHECK)); CHECK(!project_github_offered()); CHECK_INT(f.fixture.reads, 0);
    cleanup(&f);
}

static void read_only_form_cannot_save_after_permission_upgrade(void) {
    Form f; setup(&f, "read", FALLBACK);
    CHECK(f.form->read_only);
    g_store.device.permission = "admin";
    CHECK(store_supports("settings_projects")); CHECK(store_supports("create_project"));
    HeaderInfo header = {0};
    f.form->base.vt->header(&f.form->base, &header);
    CHECK_INT(header.button_count, 0);

    f.form->base.vt->action(&f.form->base, ACT_SAVE, 0, (POINT){0});
    CHECK(f.form->req_save == NULL);
    if (f.form->req_save) screen_fixture_pump(&f.fixture, 1);
    CHECK(f.form->base.vt->key(&f.form->base, 'S', true, false));
    CHECK(f.form->req_save == NULL);
    if (f.form->req_save) screen_fixture_pump(&f.fixture, 1);
    CHECK_INT(f.fixture.writes, 0);
    cleanup(&f);
}

static void health_unknown_scopes_denied_repo_and_rates(void) {
    Form f; setup(&f, "admin", IDENTITY); check_health(&f, HEALTH);
    CHECK(json_bool_is(json_get(f.form->github.health, "healthy"), false));
    CHECK_STR(json_str(json_get(json_get(f.form->github.health, "scopes"), "status")), "unknown");
    CHECK(contains(&f, "Scopes: unknown")); CHECK(contains(&f, "o/denied: no_access"));
    CHECK(contains(&f, "4997 / 5000")); CHECK(contains(&f, "4000 / 5000"));
    CHECK(contains(&f, "2023-11-14 22:13:20 UTC")); CHECK(contains(&f, "2023-11-14 22:14:20 UTC"));
    CHECK_INT(f.fixture.reads, 2); CHECK(f.form->github.health_error == NULL);
    // Rechecking replaces the old report, including known missing scopes.
    check_health(&f, "{\"healthy\":false,\"token\":{\"status\":\"valid\"},\"scopes\":{\"status\":\"missing\",\"missing\":[\"repo\",\"admin:repo_hook\"]},\"repos\":[],\"rateLimit\":null}");
    CHECK(contains(&f, "repo, admin:repo_hook")); CHECK(!contains(&f, "o/denied"));
    cleanup(&f);
}

static void rate_limited_and_registry_error_stay_until_manual_retry(void) {
    Form f; setup(&f, "admin", IDENTITY);
    check_health(&f, "{\"account\":{\"globalFallback\":true},\"healthy\":false,\"token\":{\"status\":\"rate_limited\",\"httpStatus\":null},\"scopes\":{\"status\":\"unknown\",\"missing\":null},\"repos\":[{\"repo\":\"o/r\",\"status\":\"not_checked\",\"accessible\":null,\"httpStatus\":null}],\"rateLimit\":{\"core\":{\"remaining\":0,\"limit\":5000,\"resetAt\":1700000000000},\"cooldownUntil\":{\"core\":1700000060000}}}");
    CHECK(contains(&f, "rate_limited")); CHECK(contains(&f, "not_checked")); CHECK(!contains(&f, "HTTP 0"));
    CHECK_INT(f.fixture.reads, 2);
    f.fixture.read_status = 503;
    check_health(&f, "{\"error\":\"GitHub account registry is unavailable\"}");
    CHECK(f.form->github.health == NULL); CHECK(f.form->github.health_error != NULL);
    CHECK(contains(&f, "GitHub account registry is unavailable"));
    for (int i = 0; i < 3; i++) { screen_fixture_draw(&f.form->base, &f.doc); f.form->base.vt->visible(&f.form->base, true); }
    CHECK_INT(f.fixture.reads, 3); CHECK(f.form->github.health_request == NULL);
    // The identity endpoint also keeps a 503 visible without repeated reads from layout.
    f.form->base.vt->action(&f.form->base, ACT_PROJECT_GITHUB_RELOAD, 0, (POINT){0}); screen_fixture_pump(&f.fixture, 1);
    CHECK(f.form->github.identity_error != NULL); screen_fixture_draw(&f.form->base, &f.doc); CHECK_INT(f.fixture.reads, 4);
    f.fixture.read_status = 200;
    check_health(&f, HEALTH); CHECK(f.form->github.health_error == NULL); cleanup(&f);
}

static void health_scope_change_and_cancel(void) {
    Form f; setup(&f, "admin", IDENTITY); check_health(&f, HEALTH);
    g_store.device.permission = "read";
    f.fixture.read_response = FALLBACK;
    screen_fixture_draw(&f.form->base, &f.doc); screen_fixture_pump(&f.fixture, 1);
    CHECK(f.form->github.health == NULL); CHECK(!has_action(&f, ACT_PROJECT_GITHUB_CHECK));
    CHECK(f.form->github.health_error == NULL);
    cleanup(&f);
    setup(&f, "admin", IDENTITY);
    f.fixture.block_read = true; f.fixture.read_response = HEALTH;
    f.form->base.vt->action(&f.form->base, ACT_PROJECT_GITHUB_CHECK, 0, (POINT){0});
    CHECK_INT(WaitForSingleObject(f.fixture.entered, 10000), WAIT_OBJECT_0);
    f.form->base.vt->destroy(&f.form->base); f.form = NULL;
    SetEvent(f.fixture.resume); screen_fixture_pump(&f.fixture, 1);
    doc_free(&f.doc); screen_fixture_clear(&f.fixture);
}

static void read_settings_lists_only_allowed_projects(void) {
    ScreenFixture f; screen_fixture_init(&f, ROUTES, sizeof ROUTES / sizeof *ROUTES);
    api_client_set_transport(f.client, transport, &f); g_store.device.permission = "read";
    Screen *base = settings_screen_new(); SettingsScreen *s = (SettingsScreen *)base;
    screen_fixture_pane(&f, base); Doc doc; doc_init(&doc);
    f.read_response = "{\"projects\":[{\"repo\":\"o/r\",\"label\":\"Allowed\"}]}";
    base->vt->visible(base, true); screen_fixture_pump(&f, 1);
    CHECK_STR(f.url, "https://example.com/api/v1/projects"); CHECK_INT(f.reads, 1);
    CHECK_INT(json_count(screen_settings_settings_rows(s)), 1);
    screen_fixture_draw(base, &doc); bool project = false, create = false;
    for (size_t i = 0; i < doc.count; i++) {
        if (doc.items[i].action == ACT_OPEN_PROJECT) project = true;
        if (doc.items[i].action == ACT_NEW_PROJECT) create = true;
    }
    CHECK(project); CHECK(!create);
    CHECK_OWNED_STR(screen_settings_project_row_form_id(json_at(screen_settings_settings_rows(s), 0)), "settings-project:o/r");
    base->vt->destroy(base); doc_free(&doc); screen_fixture_clear(&f);
}

static void settings_reloads_projects_on_scope_change(void) {
    ScreenFixture f; screen_fixture_init(&f, ROUTES, sizeof ROUTES / sizeof *ROUTES);
    api_client_set_transport(f.client, transport, &f);
    g_store.device.id = "original-device"; g_store.device.permission = "read";
    Screen *base = settings_screen_new(); SettingsScreen *s = (SettingsScreen *)base;
    screen_fixture_pane(&f, base); Doc doc; doc_init(&doc);
    f.read_response = "{\"projects\":[{\"repo\":\"o/read\"}]}";
    base->vt->visible(base, true); screen_fixture_pump(&f, 1);
    CHECK_STR(f.url, "https://example.com/api/v1/projects");

    // Launch verification may change permissions while Settings is already visible.
    g_store.device.permission = "admin";
    f.read_response = "{\"projects\":[{\"id\":4,\"repo\":\"o/admin\"}]}";
    screen_fixture_draw(base, &doc);
    CHECK_INT(json_count(screen_settings_settings_rows(s)), 0);
    screen_fixture_pump(&f, 1);
    CHECK_STR(f.url, "https://example.com/api/v1/settings/projects");
    CHECK_INT(screen_settings_row_id(json_at(screen_settings_settings_rows(s), 0)), 4);

    g_store.device.id = "replacement-device";
    f.read_response = "{\"projects\":[{\"id\":5,\"repo\":\"o/other\"}]}";
    base->vt->visible(base, true); screen_fixture_pump(&f, 1);
    CHECK_INT(screen_settings_row_id(json_at(screen_settings_settings_rows(s), 0)), 5);

    ServerAddress address; ApiError error; api_error_init(&error);
    CHECK(server_address_parse("https://replacement.example", &address));
    ApiClient *replacement = api_client_new(&address, "brm_aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", &error);
    CHECK(replacement != NULL); server_address_free(&address); api_error_clear(&error);
    if (replacement) {
        g_store.client = replacement; api_client_set_transport(replacement, transport, &f);
        f.read_response = "{\"projects\":[{\"id\":6,\"repo\":\"o/new-server\"}]}";
        screen_fixture_draw(base, &doc); screen_fixture_pump(&f, 1);
        CHECK_STR(f.url, "https://replacement.example/api/v1/settings/projects");
        CHECK_INT(screen_settings_row_id(json_at(screen_settings_settings_rows(s), 0)), 6);
        base->vt->visible(base, true); screen_fixture_draw(base, &doc);
        CHECK(s->req == NULL);
        CHECK_INT(f.reads, 4);
    }
    base->vt->destroy(base); g_store.client = f.client; api_client_release(replacement);
    doc_free(&doc); screen_fixture_clear(&f);
}

static void settings_discards_reply_after_permission_change(void) {
    ScreenFixture f; screen_fixture_init(&f, ROUTES, sizeof ROUTES / sizeof *ROUTES);
    api_client_set_transport(f.client, transport, &f);
    Screen *base = settings_screen_new(); SettingsScreen *s = (SettingsScreen *)base;
    screen_fixture_pane(&f, base);
    f.read_response = "{\"projects\":[{\"id\":4,\"repo\":\"o/admin\"}]}";
    f.block_read = true;
    base->vt->visible(base, true);
    CHECK_INT(WaitForSingleObject(f.entered, 10000), WAIT_OBJECT_0);
    g_store.device.permission = "read";
    SetEvent(f.resume); screen_fixture_pump(&f, 1);
    CHECK_INT(json_count(screen_settings_settings_rows(s)), 0);
    CHECK(!s->loaded);

    f.block_read = false; f.read_response = "{\"projects\":[{\"repo\":\"o/read\"}]}";
    base->vt->visible(base, true); screen_fixture_pump(&f, 1);
    CHECK_STR(f.url, "https://example.com/api/v1/projects");
    CHECK_STR(json_str(json_get(json_at(screen_settings_settings_rows(s), 0), "repo")), "o/read");
    base->vt->destroy(base); screen_fixture_clear(&f);
}

void app_project_github_tests(void) {
    screen_fixture_prepare();
    test_run("Read settings lists permitted projects without admin endpoints", read_settings_lists_only_allowed_projects);
    test_run("Settings reloads projects after changes to permission, device or client", settings_reloads_projects_on_scope_change);
    test_run("Settings discards the project reply when permission changes before completion", settings_discards_reply_after_permission_change);
    test_run("Project GitHub identity read tokens fallback and older servers", identity_read_permissions_and_fallback);
    test_run("Read-only project forms cannot save after a permission upgrade", read_only_form_cannot_save_after_permission_upgrade);
    test_run("Project GitHub health unknown scopes denied repositories and rate resets", health_unknown_scopes_denied_repo_and_rates);
    test_run("Project GitHub throttling and 503 wait for explicit retry", rate_limited_and_registry_error_stay_until_manual_retry);
    test_run("Project GitHub health clears on permission change and cancels on destroy", health_scope_change_and_cancel);
}
