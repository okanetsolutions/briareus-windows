#include "screen_fixture.h"
#include "modal_driver.h"
#include "suites.h"

static Route ROUTES[] = {
    {"GET", "/settings/mcp/servers", "admin"}, {"POST", "/settings/mcp/servers", "admin"},
    {"PUT", "/settings/mcp/servers/{id}", "admin"}, {"DELETE", "/settings/mcp/servers/{id}", "admin"},
    {"POST", "/settings/mcp/servers/{id}/connect", "admin"},
    {"POST", "/settings/mcp/servers/{id}/finish-sign-in", "admin"},
};
typedef struct { ScreenFixture fixture; Screen *screen; Doc doc; Json *row; char *registry; } Form;

static void registry(Form *f, bool listed) {
    Json *reply = json_object(), *rows = json_array();
    if (listed) json_array_push(rows, json_clone(f->row));
    json_object_set(reply, "servers", rows); free(f->registry);
    f->registry = json_serialize(reply, false); json_free(reply); f->fixture.read_response = f->registry;
}
static void setup(Form *f, bool existing) {
    memset(f, 0, sizeof *f); screen_fixture_init(&f->fixture, ROUTES, sizeof ROUTES / sizeof *ROUTES);
    f->row = json_parsez("{\"name\":\"tools\",\"label\":\"Original\",\"transport\":\"http\",\"url\":\"https://old.example\",\"command\":\"\",\"args\":[],\"repos\":[\"owner/deleted\"],\"enabled\":true,\"oauthClientId\":\"client\",\"oauthScope\":\"tools\",\"oauthClientName\":\"Briareus\",\"oauthRedirect\":\"callback\"}");
    if (existing) json_set_num(f->row, "id", 123);
    registry(f, existing); f->fixture.write_status = 429;
    f->screen = mcp_settings_screen_new(existing ? f->row : NULL, f->row);
    doc_init(&f->doc); screen_fixture_pane(&f->fixture, f->screen); f->screen->vt->visible(f->screen, true);
    if (existing) screen_fixture_pump(&f->fixture, 1);
}
static void cleanup(Form *f) {
    f->screen->vt->destroy(f->screen); doc_free(&f->doc); json_free(f->row); free(f->registry);
    screen_fixture_clear(&f->fixture);
}
static void click(Form *f, const char *text) { screen_fixture_click(f->screen, &f->doc, text); }
static void click_prefix(Form *f, const char *prefix) {
    screen_fixture_draw(f->screen, &f->doc);
    for (size_t i = 0; i < f->doc.count; i++) {
        Item *item = &f->doc.items[i];
        if (item->text && str_has_prefix(item->text, prefix) && item->action) {
            f->screen->vt->action(f->screen, item->action, item->arg, (POINT){0}); return;
        }
    }
    CHECK(false);
}
static BOOL CALLBACK show_edit(HWND window, LPARAM arg) {
    (void)arg; wchar_t name[16]; GetClassNameW(window, name, sizeof name / sizeof *name);
    if (!wcscmp(name, L"Edit")) ShowWindow(window, SW_SHOWNA);
    return TRUE;
}
static HWND field(Form *f, const char *label) {
    screen_fixture_draw(f->screen, &f->doc); int index = screen_fixture_find(&f->doc, label); CHECK(index >= 0);
    if (index < 0) return NULL;
    for (size_t i = (size_t)index + 1; i < f->doc.count; i++) {
        Item *item = &f->doc.items[i];
        if (!item->action) continue;
        EnumChildWindows(pane_hwnd(f->fixture.pane), show_edit, 0);
        f->screen->vt->action(f->screen, item->action, item->arg, (POINT){0});
        HWND edit = GetFocus(); wchar_t name[16] = {0}; GetClassNameW(edit, name, sizeof name / sizeof *name);
        CHECK(!wcscmp(name, L"Edit")); return edit;
    }
    CHECK(false); return NULL;
}
static void edit(Form *f, const char *label, const char *text) {
    HWND control = field(f, label); CHECK(control != NULL); if (!control) return;
    wchar_t *value = utf8_to_wide(text); SetWindowTextW(control, value); free(value);
    f->screen->vt->command(f->screen, GetDlgCtrlID(control), EN_CHANGE, control);
}
static char *text(Form *f, const char *label) {
    HWND control = field(f, label); int n = GetWindowTextLengthW(control);
    wchar_t *value = xcalloc((size_t)n + 1, sizeof *value); GetWindowTextW(control, value, n + 1);
    char *result = wide_to_utf8(value); free(value); return result;
}
static HeaderButton save_button(Form *f) {
    HeaderInfo info = {0}; f->screen->vt->header(f->screen, &info); CHECK(info.button_count > 0); return info.buttons[0];
}
static void save(Form *f) {
    HeaderButton button = save_button(f); f->screen->vt->action(f->screen, button.action, 0, (POINT){0});
}
static void refresh(Form *f) {
    registry(f, true); f->screen->vt->refresh(f->screen); screen_fixture_pump(&f->fixture, 1);
}
static void save_failure(Form *f) { save(f); screen_fixture_pump(&f->fixture, 1); }
static void test_assignments_and_disabled_empty_selection(void) {
    Form f; setup(&f, true); edit(&f, "Label", "Renamed"); save_failure(&f);
    CHECK_INT(f.fixture.writes, 1); CHECK(json_is_null(json_get(f.fixture.body, "repos")));
    click(&f, "Repositories"); click_prefix(&f, "Enabled:"); save_failure(&f);
    CHECK(json_is_null(json_get(f.fixture.body, "repos")));
    click_prefix(&f, "[x] owner/deleted"); click_prefix(&f, "Disabled:"); save(&f);
    CHECK_INT(f.fixture.writes, 2);
    click_prefix(&f, "Enabled:"); save_failure(&f); CHECK_INT(f.fixture.writes, 3);
    CHECK(json_is_array(json_get(f.fixture.body, "repos"))); CHECK_INT(json_count(json_get(f.fixture.body, "repos")), 0);
    cleanup(&f);
}
static void test_clean_refresh_and_dirty_conflict(void) {
    Form f; setup(&f, true); json_set_str(f.row, "url", "https://remote.example"); refresh(&f);
    CHECK_OWNED_STR(text(&f, "HTTP endpoint"), "https://remote.example");
    edit(&f, "Label", "Local label"); json_set_str(f.row, "label", "Remote label"); refresh(&f);
    CHECK_OWNED_STR(text(&f, "Label"), "Local label");
    HeaderButton button = save_button(&f);
    modal_driver_action(f.screen, button.action, 0, IDCANCEL); CHECK_INT(f.fixture.writes, 0);
    modal_driver_action(f.screen, button.action, 0, IDOK); screen_fixture_pump(&f.fixture, 1);
    CHECK_STR(json_str(json_get(f.fixture.body, "label")), "Local label");
    screen_fixture_draw(f.screen, &f.doc); int i = screen_fixture_find(&f.doc, "Reload current server fields"); CHECK(i >= 0);
    if (i >= 0) modal_driver_action(f.screen, f.doc.items[i].action, f.doc.items[i].arg, IDOK);
    CHECK_OWNED_STR(text(&f, "Label"), "Remote label"); CHECK(!save_button(&f).enabled);
    cleanup(&f);
}
static void test_clean_save_preserves_pending_sign_in(void) {
    Form f; setup(&f, true);
    json_set_str(f.row, "signInUrl", "https://auth.example/?state=pending"); json_set_bool(f.row, "signInNeedsPaste", true); refresh(&f);
    edit(&f, "Complete callback URL from the browser", "http://localhost/?state=pending&code=approved");
    CHECK(!save_button(&f).enabled); save(&f); CHECK_INT(f.fixture.writes, 0);
    CHECK_OWNED_STR(text(&f, "Complete callback URL from the browser"), "http://localhost/?state=pending&code=approved");
    json_set_str(f.row, "status", "needs-sign-in"); refresh(&f);
    CHECK_OWNED_STR(text(&f, "Complete callback URL from the browser"), "http://localhost/?state=pending&code=approved");
    edit(&f, "Label", "Renamed"); save_failure(&f); CHECK_INT(f.fixture.writes, 1);
    CHECK_OWNED_STR(text(&f, "Complete callback URL from the browser"), "http://localhost/?state=pending&code=approved");
    cleanup(&f);
    setup(&f, false); CHECK(save_button(&f).enabled); save_failure(&f); CHECK_INT(f.fixture.writes, 1); cleanup(&f);
}
static void test_sign_in_callback_contract(void) {
    Form f; setup(&f, true);
    json_set_str(f.row, "signInUrl", "https://auth.example/?state=pending"); json_set_bool(f.row, "signInNeedsPaste", true); refresh(&f);
    const char *callback = "http://localhost/?state=pending&code=private-code";
    edit(&f, "Complete callback URL from the browser", callback);
    HWND control = field(&f, "Complete callback URL from the browser");
    screen_fixture_draw(f.screen, &f.doc); int i = screen_fixture_find(&f.doc, "Finish sign-in"); CHECK(i >= 0);
    if (i >= 0) {
        Item button = f.doc.items[i]; CHECK(button.action != 0);
        f.screen->vt->action(f.screen, button.action, button.arg, (POINT){0}); screen_fixture_pump(&f.fixture, 1);
        CHECK_INT(f.fixture.writes, 1); CHECK_STR(f.fixture.method, "POST");
        CHECK_STR(f.fixture.url, "https://example.com/api/v1/settings/mcp/servers/123/finish-sign-in");
        CHECK_STR(json_str(json_get(f.fixture.body, "url")), callback); CHECK_INT(GetWindowTextLengthW(control), 0);
        f.screen->vt->action(f.screen, button.action, button.arg, (POINT){0}); CHECK_INT(f.fixture.writes, 1);
        screen_fixture_draw(f.screen, &f.doc);
        for (size_t k = 0; k < f.doc.count; k++) if (f.doc.items[k].text) CHECK(strstr(f.doc.items[k].text, "private-code") == NULL);
    }
    cleanup(&f);
}
static void test_normalized_endpoint_partial_updates(void) {
    Form f; setup(&f, true); Str endpoint; str_init(&endpoint); str_appendz(&endpoint, "https://mcp.example/");
    for (int i = 0; i < 300; i++) str_appendz(&endpoint, "%E4%B8%AD");
    json_set_str(f.row, "url", endpoint.data); refresh(&f); edit(&f, "Label", "Renamed"); save_failure(&f);
    CHECK(json_is_null(json_get(f.fixture.body, "url"))); CHECK_STR(json_str(json_get(f.fixture.body, "label")), "Renamed");
    char *changed_url = xstrfmt("%s/changed", endpoint.data); edit(&f, "HTTP endpoint", changed_url); free(changed_url);
    save(&f); CHECK_INT(f.fixture.writes, 1);
    edit(&f, "HTTP endpoint", "https://new.example/mcp"); save_failure(&f);
    CHECK_STR(json_str(json_get(f.fixture.body, "url")), "https://new.example/mcp");
    cleanup(&f);
    setup(&f, false); edit(&f, "HTTP endpoint", endpoint.data); save(&f); CHECK_INT(f.fixture.writes, 0); cleanup(&f); str_free(&endpoint);
}
static void test_metadata_updates_preserve_connection(void) {
    Form f; setup(&f, true); edit(&f, "Label", "Renamed"); save_failure(&f);
    static const char *const keys[] = {"transport", "url", "command", "args", "headers", "env", "oauthClientId", "oauthClientSecret", "oauthScope", "oauthClientName", "oauthRedirect"};
    for (size_t i = 0; i < sizeof keys / sizeof *keys; i++) CHECK(json_is_null(json_get(f.fixture.body, keys[i])));
    edit(&f, "HTTP endpoint", "https://new.example"); click_prefix(&f, "Headers:");
    edit(&f, "Replacement headers (JSON object)", "{\"Authorization\":\"private replacement\"}");
    click(&f, "Advanced OAuth"); click_prefix(&f, "Client secret:"); click_prefix(&f, "Client secret:");
    edit(&f, "OAuth client ID (empty = register automatically)", "new-client"); save_failure(&f);
    CHECK_STR(json_str(json_get(f.fixture.body, "url")), "https://new.example");
    CHECK_STR(json_str(json_get(json_get(f.fixture.body, "headers"), "Authorization")), "private replacement");
    CHECK_STR(json_str(json_get(f.fixture.body, "oauthClientSecret")), "");
    CHECK_STR(json_str(json_get(f.fixture.body, "oauthClientId")), "new-client");
    screen_fixture_draw(f.screen, &f.doc);
    for (size_t i = 0; i < f.doc.count; i++) if (f.doc.items[i].text)
        CHECK(strstr(f.doc.items[i].text, "private replacement") == NULL);
    cleanup(&f);
}
static void test_uncertain_create_reconciliation(void) {
    const int statuses[] = {400, 502};
    for (size_t k = 0; k < sizeof statuses / sizeof *statuses; k++) for (int committed = 0; committed < 2; committed++) {
        Form f; setup(&f, false); click(&f, "Advanced OAuth"); click_prefix(&f, "Client secret:");
        edit(&f, "Replacement OAuth client secret", "private draft"); click(&f, "Connection");
        f.fixture.write_status = statuses[k]; f.fixture.block_write = true;
        save(&f); CHECK_INT(WaitForSingleObject(f.fixture.entered, 10000), WAIT_OBJECT_0);
        CHECK_INT(f.fixture.writes, 1); CHECK_STR(json_str(json_get(f.fixture.body, "name")), "tools");
        if (committed) { json_set_num(f.row, "id", 456); json_set_str(f.row, "url", "https://created.example"); }
        registry(&f, committed); SetEvent(f.fixture.resume); screen_fixture_pump(&f.fixture, 2);
        CHECK_STR(f.screen->id, committed ? "settings-mcp:456" : "settings-mcp:new");
        CHECK_INT(f.fixture.writes, 1); click(&f, "Advanced OAuth");
        CHECK_OWNED_STR(text(&f, "Replacement OAuth client secret"), "private draft"); click(&f, "Connection");
        edit(&f, "Tool name", "edited-name"); f.fixture.write_status = 429; f.fixture.block_write = false;
        if (committed) {
            HeaderButton button = save_button(&f); modal_driver_action(f.screen, button.action, 0, IDOK);
            screen_fixture_pump(&f.fixture, 1); CHECK_STR(f.fixture.method, "PUT");
            CHECK_STR(f.fixture.url, "https://example.com/api/v1/settings/mcp/servers/456");
            CHECK_STR(json_str(json_get(f.fixture.body, "name")), "edited-name");
        } else {
            save_failure(&f); CHECK_STR(f.fixture.method, "POST");
            CHECK_STR(json_str(json_get(f.fixture.body, "name")), "tools");
            CHECK_OWNED_STR(text(&f, "Tool name"), "edited-name");
        }
        CHECK_INT(f.fixture.writes, 2); cleanup(&f);
    }
}
static void test_nul_authoring(void) {
    Form f; setup(&f, true); click_prefix(&f, "Transport:");
    edit(&f, "Command on the core machine", "mcp");
    edit(&f, "Arguments (JSON array; spaces and empty strings are preserved)", "[\"a\\u0000b\"]"); save(&f); CHECK_INT(f.fixture.writes, 0);
    edit(&f, "Arguments (JSON array; spaces and empty strings are preserved)", "[\"a\\\\u0000b\",\"\"]"); save_failure(&f);
    CHECK_STR(json_str(json_at(json_get(f.fixture.body, "args"), 0)), "a\\u0000b");
    CHECK_STR(json_str(json_at(json_get(f.fixture.body, "args"), 1)), "");
    click_prefix(&f, "Environment:"); edit(&f, "Replacement environment (JSON object)", "{\"TO\\u0000KEN\":\"value\"}"); save(&f); CHECK_INT(f.fixture.writes, 1);
    edit(&f, "Replacement environment (JSON object)", "{\"TOKEN\":\"replacement\"}"); save_failure(&f);
    CHECK_STR(json_str(json_get(json_get(f.fixture.body, "env"), "TOKEN")), "replacement"); cleanup(&f);
}
static Screen *settings_sidebar(Form *f, Pane **sidebar) {
    *sidebar = pane_create(f->fixture.parent, true); CHECK(*sidebar != NULL);
    Screen *settings = settings_screen_new(); settings->pane = *sidebar;
    return settings;
}

static void test_departed_mutation_completion(void) {
    for (int deleting = 0; deleting < 2; deleting++) for (int allowed = 0; allowed < 2; allowed++) {
        Form f; setup(&f, true); Pane *sidebar = NULL; Screen *settings = settings_sidebar(&f, &sidebar);
        f.fixture.block_write = true;
        if (deleting) {
            HeaderInfo info = {0}; f.screen->vt->header(f.screen, &info); CHECK_INT(info.button_count, 2);
            modal_driver_action(f.screen, info.buttons[1].action, 0, IDOK);
        } else { edit(&f, "Label", "Departed draft"); save(&f); }
        CHECK_INT(WaitForSingleObject(f.fixture.entered, 10000), WAIT_OBJECT_0);
        f.screen->vt->visible(f.screen, false); f.screen->vt->destroy(f.screen); f.screen = NULL;
        if (!allowed) g_store.device.permission = "read";
        LONG reads = f.fixture.reads;
        SetEvent(f.fixture.resume); screen_fixture_pump(&f.fixture, allowed ? 2 : 1);
        CHECK_INT(f.fixture.writes, 1); CHECK_INT(f.fixture.reads, reads + allowed);
        settings->vt->destroy(settings); pane_destroy(sidebar);
        doc_free(&f.doc); json_free(f.row); free(f.registry); screen_fixture_clear(&f.fixture);
    }
}
static void test_polled_status_and_deletion_refresh_settings(void) {
    Form f; setup(&f, true); Pane *sidebar = NULL; Screen *settings = settings_sidebar(&f, &sidebar);
    json_set_str(f.row, "status", "ready"); json_set_bool(f.row, "signedIn", true); registry(&f, true);
    LONG reads = f.fixture.reads;
    f.screen->vt->refresh(f.screen); screen_fixture_pump(&f.fixture, 2); CHECK_INT(f.fixture.reads, reads + 2);
    f.screen->vt->refresh(f.screen); screen_fixture_pump(&f.fixture, 1); CHECK_INT(f.fixture.reads, reads + 3);
    edit(&f, "Label", "Unsaved draft"); registry(&f, false);
    f.screen->vt->refresh(f.screen); screen_fixture_pump(&f.fixture, 2);
    CHECK_INT(f.fixture.reads, reads + 5); CHECK(!save_button(&f).enabled);
    save(&f); CHECK_INT(f.fixture.writes, 0);
    settings->vt->destroy(settings); pane_destroy(sidebar); cleanup(&f);
}
static bool has_text(Form *f, const char *fragment) {
    screen_fixture_draw(f->screen, &f->doc);
    for (size_t i = 0; i < f->doc.count; i++)
        if (f->doc.items[i].text && strstr(f->doc.items[i].text, fragment)) return true;
    return false;
}
static void test_read_notices_and_malformed_reconciliation(void) {
    Form f; setup(&f, true); f.fixture.read_status = 502;
    f.screen->vt->refresh(f.screen); screen_fixture_pump(&f.fixture, 1); CHECK(has_text(&f, "HTTP 502"));
    f.fixture.read_status = 200; refresh(&f); CHECK(!has_text(&f, "HTTP 502"));
    edit(&f, "Label", "Renamed"); f.fixture.write_status = 400;
    save(&f); screen_fixture_pump(&f.fixture, 2); CHECK(has_text(&f, "HTTP 400"));
    refresh(&f); CHECK(has_text(&f, "HTTP 400")); cleanup(&f);
    setup(&f, false); f.fixture.write_status = 502; f.fixture.read_response = "{}";
    save(&f); screen_fixture_pump(&f.fixture, 2); CHECK_INT(f.fixture.writes, 1);
    CHECK(!save_button(&f).enabled); save(&f); CHECK_INT(f.fixture.writes, 1);
    registry(&f, false); f.screen->vt->refresh(f.screen); screen_fixture_pump(&f.fixture, 1);
    CHECK(save_button(&f).enabled); CHECK_INT(f.fixture.writes, 1); cleanup(&f);
}
static void test_capability_and_stale_results(void) {
    Form f; setup(&f, true); edit(&f, "Label", "Draft");
    HeaderButton button = save_button(&f);
    g_store.device.permission = "read"; f.screen->vt->action(f.screen, button.action, 0, (POINT){0}); CHECK_INT(f.fixture.writes, 0);
    g_store.device.permission = "admin"; g_store.route_count = 1; save(&f); CHECK_INT(f.fixture.writes, 0);
    g_store.route_count = sizeof ROUTES / sizeof *ROUTES;
    f.fixture.block_read = true; json_set_str(f.row, "label", "Stale remote"); registry(&f, true);
    f.screen->vt->refresh(f.screen); CHECK_INT(WaitForSingleObject(f.fixture.entered, 10000), WAIT_OBJECT_0);
    f.screen->vt->visible(f.screen, false); SetEvent(f.fixture.resume); screen_fixture_pump(&f.fixture, 1);
    f.fixture.block_read = false; json_set_str(f.row, "label", "Original"); registry(&f, true);
    f.screen->vt->visible(f.screen, true); screen_fixture_pump(&f.fixture, 1);
    CHECK_OWNED_STR(text(&f, "Label"), "Draft"); cleanup(&f);
}
void app_mcp_settings_tests(void) {
    screen_fixture_prepare();
    test_run("MCP linked form retains assignments and validates disabled empty selections", test_assignments_and_disabled_empty_selection);
    test_run("MCP refresh preserves drafts until actual conflict confirmation or reload", test_clean_refresh_and_dirty_conflict);
    test_run("MCP clean saves and status polls preserve pending OAuth callbacks", test_clean_save_preserves_pending_sign_in);
    test_run("MCP sign-in callbacks send the deployed contract once and erase private input on failure", test_sign_in_callback_contract);
    test_run("MCP partial updates omit stored normalized endpoints and validate replacements", test_normalized_endpoint_partial_updates);
    test_run("MCP metadata updates preserve connections and send explicit secret changes", test_metadata_updates_preserve_connection);
    test_run("MCP uncertain and postcommit failed creates reconcile names without automatic retries", test_uncertain_create_reconciliation);
    test_run("MCP authored JSON rejects NUL and preserves literal escapes and empty arguments", test_nul_authoring);
    test_run("MCP detached writes complete safely after controls and their form are destroyed", test_departed_mutation_completion);
    test_run("MCP polled status changes and authoritative deletion refresh the linked Settings screen", test_polled_status_and_deletion_refresh_settings);
    test_run("MCP healthy reads clear read notices, preserve mutation notices and reject malformed create reconciliation", test_read_notices_and_malformed_reconciliation);
    test_run("MCP forms enforce live capabilities and ignore hidden reads", test_capability_and_stale_results);
}
