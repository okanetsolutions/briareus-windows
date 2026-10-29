#include "screen_fixture.h"
#include "screen_provider_settings_internal.h"
#include "suites.h"

static Route ROUTES[] = {
    {"GET", "/settings/providers", "admin"},
    {"GET", "/openapi.json", "read"},
    {"GET", "/settings/providers/{id}/status", "admin"},
    {"POST", "/settings/providers/{id}/login", "admin"},
    {"POST", "/settings/providers/test", "admin"},
    {"POST", "/settings/providers", "admin"},
    {"PUT", "/settings/providers/{id}", "admin"},
};
static const char *CONNECTED = "{\"status\":{\"auth\":{\"loggedIn\":true,\"email\":\"one@example.com\",\"checkedAt\":\"earlier\"}}}";
static const char *REREAD = "{\"status\":{\"auth\":{\"loggedIn\":true,\"email\":\"one@example.com\",\"checkedAt\":\"later\"}}}";
static const char *OTHER = "{\"status\":{\"auth\":{\"loggedIn\":true,\"email\":\"two@example.com\"}}}";
static const char *LOGGED_OUT = "{\"status\":{\"auth\":{\"loggedIn\":false}}}";
typedef struct { ScreenFixture fixture; FormScreen *form; } Form;

static void setup(Form *f, const char *status) {
    memset(f, 0, sizeof *f); screen_fixture_init(&f->fixture, ROUTES, sizeof ROUTES / sizeof *ROUTES);
    f->fixture.read_response = status;
    // An unsupported scheme avoids launching a real browser in the linked harness.
    f->fixture.write_response = "{\"url\":\"fixture://device-login\",\"deviceCode\":\"FIXTURE-CODE\"}";
    Json *row = json_parsez("{\"id\":7,\"binary\":\"codex\",\"label\":\"Fixture\"}");
    f->form = (FormScreen *)provider_settings_screen_new(row, NULL); json_free(row);
    screen_fixture_pane(&f->fixture, &f->form->base);
    screen_provider_settings_load_status(f->form, false);
}
static void cleanup(Form *f) {
    f->form->base.vt->destroy(&f->form->base); screen_fixture_clear(&f->fixture);
}
static void start(Form *f) {
    screen_provider_settings_login_start(f->form); screen_fixture_pump(&f->fixture, 1);
    CHECK_STR(f->form->device_code, "FIXTURE-CODE");
}
static void poll(Form *f, const char *status) {
    f->fixture.read_response = status;
    screen_provider_settings_load_status(f->form, false); screen_fixture_pump(&f->fixture, 1);
}
static void retained_and_changed(void) {
    Form f; setup(&f, CONNECTED); screen_fixture_pump(&f.fixture, 1); start(&f);
    poll(&f, REREAD); CHECK_STR(f.form->device_code, "FIXTURE-CODE");
    poll(&f, OTHER); CHECK(f.form->device_code == NULL);
    cleanup(&f);
}
static void first_login(void) {
    Form f; setup(&f, LOGGED_OUT); screen_fixture_pump(&f.fixture, 1); start(&f);
    poll(&f, LOGGED_OUT); CHECK(f.form->device_code != NULL);
    poll(&f, CONNECTED); CHECK(f.form->device_code == NULL);
    cleanup(&f);
}
static void initial_and_previous_reads(void) {
    Form f; setup(&f, CONNECTED);
    screen_provider_settings_login_start(f.form); CHECK(f.form->req_login == NULL);
    screen_fixture_pump(&f.fixture, 1); CHECK_INT(f.fixture.writes, 0); start(&f);
    // A previous attempt's in-flight poll gates another login until its response is consumed.
    f.fixture.block_read = true; ResetEvent(f.fixture.resume);
    screen_provider_settings_load_status(f.form, false);
    CHECK_INT(WaitForSingleObject(f.fixture.entered, 10000), WAIT_OBJECT_0);
    screen_provider_settings_login_start(f.form); CHECK(f.form->req_login == NULL);
    SetEvent(f.fixture.resume); screen_fixture_pump(&f.fixture, 1); f.fixture.block_read = false;
    start(&f); poll(&f, REREAD); CHECK(f.form->device_code != NULL);
    cleanup(&f);
}
static void pending_login_and_errors(void) {
    Form f; setup(&f, CONNECTED); screen_fixture_pump(&f.fixture, 1);
    f.fixture.block_write = true; ResetEvent(f.fixture.resume);
    screen_provider_settings_login_start(f.form);
    CHECK_INT(WaitForSingleObject(f.fixture.entered, 10000), WAIT_OBJECT_0);
    screen_provider_settings_load_status(f.form, false); CHECK(f.form->req_status == NULL);
    screen_provider_settings_login_start(f.form); CHECK_INT(f.fixture.writes, 1);
    SetEvent(f.fixture.resume); screen_fixture_pump(&f.fixture, 1); f.fixture.block_write = false;
    f.fixture.read_status = 503; poll(&f, "{}"); CHECK(f.form->device_code != NULL);
    CHECK(f.form->status_error != NULL);
    f.fixture.read_status = 200; poll(&f, REREAD); CHECK(f.form->device_code != NULL);
    f.fixture.write_status = 503; screen_provider_settings_login_start(f.form); screen_fixture_pump(&f.fixture, 1);
    CHECK(f.form->error != NULL); CHECK(f.form->req_login == NULL);
    cleanup(&f);
}
static void picker_cancellation_preserves_initial_read(void) {
    Form f; setup(&f, CONNECTED);
    Request *initial = f.form->req_status; CHECK(initial != NULL);
    // Both pickers cancel login activity on the way away and back.
    screen_provider_settings_cancel_login(f.form);
    screen_provider_settings_cancel_login(f.form);
    CHECK(f.form->req_status == initial);
    screen_provider_settings_login_start(f.form); CHECK(f.form->req_login == NULL);
    screen_fixture_pump(&f.fixture, 1);
    CHECK_STR(json_str(json_get(json_get(f.form->status, "auth"), "email")), "one@example.com");
    CHECK_INT(f.fixture.reads, 1);
    start(&f);
    cleanup(&f);
}
static void cancelled_attempt(void) {
    Form f; setup(&f, CONNECTED); screen_fixture_pump(&f.fixture, 1); start(&f);
    f.fixture.block_read = true; ResetEvent(f.fixture.resume);
    screen_provider_settings_load_status(f.form, false);
    CHECK_INT(WaitForSingleObject(f.fixture.entered, 10000), WAIT_OBJECT_0);
    screen_provider_settings_cancel_login(f.form);
    CHECK(f.form->req_status == NULL); CHECK(f.form->device_code == NULL);
    // The cancelled old read will report another account after the new code has arrived.
    start(&f); f.fixture.read_response = OTHER;
    SetEvent(f.fixture.resume); screen_fixture_pump(&f.fixture, 1); f.fixture.block_read = false;
    CHECK_STR(f.form->device_code, "FIXTURE-CODE");
    CHECK_STR(json_str(json_get(json_get(f.form->status, "auth"), "email")), "one@example.com");
    poll(&f, REREAD); CHECK(f.form->device_code != NULL);
    cleanup(&f);
}
static void teardown(void) {
    for (int login = 0; login < 2; login++) {
        Form f; setup(&f, CONNECTED); screen_fixture_pump(&f.fixture, 1);
        if (login) f.fixture.block_write = true;
        else f.fixture.block_read = true;
        if (login) screen_provider_settings_login_start(f.form);
        else screen_provider_settings_load_status(f.form, false);
        CHECK_INT(WaitForSingleObject(f.fixture.entered, 10000), WAIT_OBJECT_0);
        f.form->base.vt->destroy(&f.form->base);
        SetEvent(f.fixture.resume); screen_fixture_pump(&f.fixture, 1);
        screen_fixture_clear(&f.fixture);
    }
}
static const char *const IDS[] = { "claude", "codex", "grok", "opencode", "dsh", "qwen" };

static void binary_catalog(void) {
    Form f; setup(&f, CONNECTED); screen_fixture_pump(&f.fixture, 1);
    HMENU menu = screen_provider_settings_binary_menu(f.form);
    CHECK_INT(GetMenuItemCount(menu), 6); CHECK_INT(BINARY_COUNT, 6);
    for (int i = 0; i < 6; i++) {
        CHECK_STR(screen_provider_settings_BINARIES[i].id, IDS[i]);
        CHECK_INT(GetMenuItemID(menu, i), i + 1);
        wchar_t title[128] = {0};
        CHECK(GetMenuStringW(menu, (UINT)i, title, 128, MF_BYPOSITION) > 0);
        char *text = wide_to_utf8(title);
        CHECK(strstr(text, IDS[i]) != NULL); free(text);
    }
    CHECK(GetMenuState(menu, 1, MF_BYPOSITION) & MF_CHECKED);
    DestroyMenu(menu);
    CHECK_STR(screen_provider_settings_binary_title("future-cli"), "future-cli");
    CHECK_STR(screen_provider_settings_binary_title(NULL), "Pick a binary");
    cleanup(&f);
}

static void auth_modes(void) {
    Form f; setup(&f, CONNECTED); screen_fixture_pump(&f.fixture, 1);
    for (int i = 0; i < 6; i++) {
        screen_provider_settings_select_binary(f.form, IDS[i]);
        bool choice = i < 2, key = i >= 3;
        CHECK_INT(screen_provider_settings_mode_offered(IDS[i]), choice);
        f.form->token = false;
        CHECK_INT(screen_provider_settings_uses_token(f.form), key);
        CHECK_INT(screen_provider_settings_field_shown(f.form, F_API_KEY), key);
        CHECK_INT(screen_provider_settings_can_log_in(f.form), i == 1);
        f.form->token = true;
        CHECK_INT(screen_provider_settings_uses_token(f.form), i != 2);
        CHECK(!screen_provider_settings_can_log_in(f.form));
        if (key) {
            screen_provider_settings_login_start(f.form); CHECK(f.form->req_login == NULL);
            screen_provider_settings_select_mode(f.form, false); CHECK(f.form->token);
        }
    }
    // Login is safe again only for the saved binary, with its mode set to Login.
    screen_provider_settings_select_binary(f.form, "codex");
    screen_provider_settings_select_mode(f.form, false);
    CHECK(screen_provider_settings_can_log_in(f.form));
    cleanup(&f);
}

static void fill_unsaved(FormScreen *s, const char *key) {
    screen_provider_settings_form_ensure_controls(s);
    screen_provider_settings_set_edit_text(s->edits[F_LABEL], "Unsaved label");
    screen_provider_settings_set_edit_text(s->edits[F_BASE_URL], "https://gateway.example/v1");
    screen_provider_settings_set_edit_text(s->edits[F_API_KEY], key);
    screen_provider_settings_set_edit_text(s->edits[F_MODELS], "proxy-a\nproxy-b");
    screen_provider_settings_set_edit_text(s->edits[F_EFFORTS], "low\nhigh");
    screen_provider_settings_set_edit_text(s->edits[F_DEFAULT_MODEL], "proxy-b");
    screen_provider_settings_set_edit_text(s->edits[F_DEFAULT_EFFORT], "high");
}

static void check_fields(const Json *body, const char *binary, const char *key) {
    CHECK_STR(json_str(json_get(body, "binary")), binary);
    CHECK_STR(json_str(json_get(body, "baseUrl")), "https://gateway.example/v1");
    CHECK_STR(json_str(json_get(body, "apiKey")), key);
    CHECK_STR(json_str(json_get(body, "defaultModel")), "proxy-b");
    CHECK_STR(json_str(json_at(json_get(body, "models"), 0)), "proxy-a");
    CHECK_STR(json_str(json_at(json_get(body, "models"), 1)), "proxy-b");
}

static void serialized_contracts(void) {
    for (int i = 0; i < 6; i++) {
        Form f; setup(&f, CONNECTED); screen_fixture_pump(&f.fixture, 1);
        const char *key = i == 5 ? "" : "fixture-key";
        fill_unsaved(f.form, key);
        screen_provider_settings_select_binary(f.form, IDS[i]);
        if (i < 2) screen_provider_settings_select_mode(f.form, true);
        if (i != 2) {
            f.fixture.write_response = "{\"models\":[],\"probedModel\":\"proxy-b\"}";
            screen_provider_settings_test_endpoint(f.form); screen_fixture_pump(&f.fixture, 1);
            CHECK_STR(f.fixture.method, "POST");
            CHECK_STR(f.fixture.url, "https://example.com/api/v1/settings/providers/test");
            check_fields(f.fixture.body, IDS[i], key);
            CHECK_INT(json_int_or(json_get(f.fixture.body, "id"), 0), 7);
        }
        f.fixture.write_status = 400; f.fixture.write_response = "{\"error\":\"fixture refuses save\"}";
        screen_provider_settings_form_save(f.form); screen_fixture_pump(&f.fixture, 1);
        CHECK_STR(f.fixture.method, "PUT");
        CHECK_STR(f.fixture.url, "https://example.com/api/v1/settings/providers/7");
        if (i != 2) check_fields(f.fixture.body, IDS[i], key);
        else {
            CHECK_STR(json_str(json_get(f.fixture.body, "binary")), "grok");
            CHECK_STR(json_str(json_get(f.fixture.body, "apiKey")), "");
            CHECK_STR(json_str(json_get(f.fixture.body, "baseUrl")), "");
        }
        CHECK_STR(json_str(json_get(f.fixture.body, "label")), "Unsaved label");
        CHECK_STR(json_str(json_get(f.fixture.body, "defaultEffort")), "high");
        CHECK_INT(json_count(json_get(f.fixture.body, "efforts")), 2);
        CHECK(f.form->dirty); cleanup(&f);
    }
}

static void unknown_and_clone(void) {
    Form f; setup(&f, CONNECTED); screen_fixture_pump(&f.fixture, 1);
    json_set_str(f.form->row, "binary", "future-cli");
    json_set_str(f.form->row, "apiKey", "fixture-kept");
    json_set_str(f.form->row, "baseUrl", "https://future.example");
    screen_provider_settings_form_fill(f.form);
    CHECK(!f.form->dirty); CHECK(!screen_provider_settings_tab_changed(f.form, T_PROVIDER));
    CHECK(!screen_provider_settings_can_log_in(f.form));
    // No controls yet: saved unknown IDs and their authentication still round-trip.
    int tab = T_PROVIDER; Json *body = screen_provider_settings_form_body(f.form, NULL, &tab);
    CHECK_STR(json_str(json_get(body, "binary")), "future-cli");
    CHECK_STR(json_str(json_get(body, "apiKey")), "fixture-kept");
    CHECK_STR(json_str(json_get(body, "baseUrl")), "https://future.example");
    json_free(body);
    fill_unsaved(f.form, "fixture-unsaved");
    screen_provider_settings_select_binary(f.form, "dsh");
    screen_provider_settings_select_binary(f.form, "qwen");
    screen_provider_settings_select_binary(f.form, "future-cli");
    body = screen_provider_settings_form_body(f.form, NULL, &tab);
    check_fields(body, "future-cli", "fixture-unsaved");
    // Clone path uses this same body, stripped of server identity, as a new form.
    FormScreen *clone = (FormScreen *)provider_settings_screen_new(body, NULL);
    CHECK_INT(clone->id, 0); CHECK_STR(clone->binary, "future-cli");
    Json *copy = screen_provider_settings_form_body(clone, NULL, &tab);
    check_fields(copy, "future-cli", "fixture-unsaved");
    json_free(copy); json_free(body); clone->base.vt->destroy(&clone->base);
    cleanup(&f);
}

static void new_defaults(void) {
    for (int i = 4; i < 6; i++) {
        Form f; setup(&f, CONNECTED); screen_fixture_pump(&f.fixture, 1);
        f.form->id = 0; screen_provider_settings_form_ensure_controls(f.form);
        screen_provider_settings_select_binary(f.form, IDS[i]);
        screen_provider_settings_set_edit_text(f.form->edits[F_LABEL], "New fixture");
        f.fixture.write_response = "{\"probedModel\":\"fixture-default\"}";
        screen_provider_settings_test_endpoint(f.form); screen_fixture_pump(&f.fixture, 1);
        CHECK_STR(json_str(json_get(f.fixture.body, "binary")), IDS[i]);
        CHECK_STR(json_str(json_get(f.fixture.body, "apiKey")), "");
        CHECK_STR(json_str(json_get(f.fixture.body, "baseUrl")), "");
        CHECK_STR(json_str(json_get(f.fixture.body, "defaultModel")), "");
        CHECK_INT(json_count(json_get(f.fixture.body, "models")), 0);
        CHECK(json_is_null(json_get(f.fixture.body, "id")));
        f.fixture.write_status = 400; f.fixture.write_response = "{}";
        screen_provider_settings_form_save(f.form); screen_fixture_pump(&f.fixture, 1);
        CHECK_STR(f.fixture.method, "POST"); CHECK_STR(f.fixture.url, "https://example.com/api/v1/settings/providers");
        CHECK_STR(json_str(json_get(f.fixture.body, "binary")), IDS[i]);
        CHECK_INT(json_count(json_get(f.fixture.body, "models")), 0);
        CHECK_INT(json_count(json_get(f.fixture.body, "efforts")), 0);
        CHECK_STR(json_str(json_get(f.fixture.body, "defaultModel")), "");
        CHECK_STR(json_str(json_get(f.fixture.body, "defaultEffort")), "");
        cleanup(&f);
    }
}

static void stale_probe(void) {
    Form f; setup(&f, CONNECTED); screen_fixture_pump(&f.fixture, 1);
    fill_unsaved(f.form, "fixture-key");
    screen_provider_settings_select_binary(f.form, "dsh");
    f.fixture.block_write = true; f.fixture.write_response = "{\"models\":[\"stale-model\"]}";
    screen_provider_settings_test_endpoint(f.form);
    CHECK_INT(WaitForSingleObject(f.fixture.entered, 10000), WAIT_OBJECT_0);
    screen_provider_settings_select_binary(f.form, "qwen");
    CHECK(f.form->req_test == NULL); CHECK(f.form->test_result == NULL);
    SetEvent(f.fixture.resume); screen_fixture_pump(&f.fixture, 1);
    CHECK_STR(f.form->binary, "qwen");
    char *models = screen_provider_settings_field_now(f.form, F_MODELS);
    CHECK_STR(models, "proxy-a\nproxy-b"); free(models);
    CHECK(f.form->test_result == NULL); CHECK(f.form->dirty);
    // Returning to the original binary/mode restores all edits and does not use the old verdict.
    screen_provider_settings_select_binary(f.form, "codex");
    screen_provider_settings_select_mode(f.form, true);
    screen_provider_settings_select_mode(f.form, false);
    screen_provider_settings_select_mode(f.form, true);
    int tab = 0; Json *body = screen_provider_settings_form_body(f.form, NULL, &tab);
    check_fields(body, "codex", "fixture-key"); json_free(body);
    cleanup(&f);
}

static void metadata_catalog(void) {
    const char *responses[] = {
        "{\"components\":{\"schemas\":{\"Provider\":{\"properties\":{\"binary\":{\"enum\":[\"future-cli\",\"qwen\"]}}}}}}",
        "{\"providers\":[{\"binary\":\"claude\"}]}", "{}"
    };
    for (int scenario = 0; scenario < 3; scenario++) {
        Form f; setup(&f, CONNECTED); screen_fixture_pump(&f.fixture, 1);
        fill_unsaved(f.form, "fixture-key");
        screen_provider_settings_select_binary(f.form, "dsh");
        f.fixture.read_response = responses[scenario];
        if (scenario == 2) f.fixture.read_status = 503;
        screen_provider_settings_load_binaries(f.form); screen_fixture_pump(&f.fixture, 1);
        CHECK_STR(f.form->binary, "dsh"); CHECK(f.form->dirty);
        int tab = 0; Json *body = screen_provider_settings_form_body(f.form, NULL, &tab);
        check_fields(body, "dsh", "fixture-key"); json_free(body);
        HMENU menu = screen_provider_settings_binary_menu(f.form);
        CHECK_INT(GetMenuItemCount(menu), scenario == 0 ? 2 : 6);
        CHECK_STR(screen_provider_settings_binary_id(f.form, 0), scenario == 0 ? "future-cli" : "claude");
        DestroyMenu(menu);
        if (scenario == 0) {
            screen_provider_settings_select_binary(f.form, screen_provider_settings_binary_id(f.form, 0));
            CHECK_STR(f.form->binary, "future-cli"); CHECK(!screen_provider_settings_can_log_in(f.form));
        }
        screen_provider_settings_load_binaries(f.form); CHECK(f.form->req_binaries == NULL);
        cleanup(&f);
    }
}

static void probe_and_catalog_teardown(void) {
    for (int catalog = 0; catalog < 2; catalog++) {
        Form f; setup(&f, CONNECTED); screen_fixture_pump(&f.fixture, 1);
        fill_unsaved(f.form, "fixture-key");
        screen_provider_settings_select_binary(f.form, "dsh");
        if (catalog) { f.fixture.block_read = true; screen_provider_settings_load_binaries(f.form); }
        else { f.fixture.block_write = true; screen_provider_settings_test_endpoint(f.form); }
        CHECK_INT(WaitForSingleObject(f.fixture.entered, 10000), WAIT_OBJECT_0);
        f.form->base.vt->destroy(&f.form->base);
        SetEvent(f.fixture.resume); screen_fixture_pump(&f.fixture, 1);
        screen_fixture_clear(&f.fixture);
    }
}

void app_provider_settings_tests(void) {
    screen_fixture_prepare();
    test_run("provider metadata overrides fallback without changing form", metadata_catalog);
    test_run("provider teardown cancels pending catalog and probe", probe_and_catalog_teardown);
    test_run("provider native binary menu exposes all supported IDs", binary_catalog);
    test_run("provider binary authentication modes match backend", auth_modes);
    test_run("provider saves and probes serialize all binary contracts", serialized_contracts);
    test_run("provider unknown IDs and unsaved clone values survive switches", unknown_and_clone);
    test_run("provider new dsh and qwen defer empty model effort defaults", new_defaults);
    test_run("provider switching cancels stale endpoint probe", stale_probe);
    test_run("provider re-login retains code until account changes", retained_and_changed);
    test_run("provider first login clears code after connection", first_login);
    test_run("provider initial and previous reads gate new login", initial_and_previous_reads);
    test_run("provider pending login suppresses reads and handles errors", pending_login_and_errors);
    test_run("provider picker cancellation preserves initial status read", picker_cancellation_preserves_initial_read);
    test_run("provider cancelled old attempt cannot clear current code", cancelled_attempt);
    test_run("provider teardown cancels pending status and login", teardown);
}
