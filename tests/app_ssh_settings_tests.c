#include "screen_fixture.h"
#include "screen_settings_internal.h"
#include "modal_driver.h"
#include "suites.h"

static Route ROUTES[] = {
    {"GET", "/settings/ssh/servers", "admin"},
    {"POST", "/settings/ssh/servers", "admin"},
    {"PUT", "/settings/ssh/servers/{id}", "admin"},
    {"GET", "/settings/ssh/servers/{id}/db-credentials", "admin"},
};
typedef struct { ScreenFixture fixture; SshForm *form; Doc doc; } Form;
static const char *ROW = "{\"id\":7,\"repo\":\"o/r\",\"host\":\"fixture.invalid\",\"username\":\"fixture\",\"port\":22,\"hasPassword\":true,\"hasSudoPassword\":false}";
static const char *SAVED = "{\"server\":{\"id\":7,\"repo\":\"o/r\",\"host\":\"fixture.invalid\",\"username\":\"fixture\",\"port\":22,\"hasPassword\":true,\"hasSudoPassword\":true}}";
static void setup(Form *f, const char *text) {
    memset(f, 0, sizeof *f); screen_fixture_init(&f->fixture, ROUTES, sizeof ROUTES / sizeof *ROUTES);
    Json *row = json_parsez(text); f->form = (SshForm *)screen_settings_ssh_settings_screen_new(row, NULL); json_free(row);
    doc_init(&f->doc); screen_fixture_pane(&f->fixture, &f->form->base);
    f->form->base.vt->visible(&f->form->base, true); screen_settings_g_ssh_tab = ST_SERVER;
}
static void cleanup(Form *f) {
    f->form->base.vt->destroy(&f->form->base); doc_free(&f->doc); screen_fixture_clear(&f->fixture);
}
static void type(Form *f, int field, const char *text) {
    screen_settings_set_edit_text(f->form->edits[field], text);
    f->form->base.vt->command(&f->form->base, ID_SSH_FIELD + field, EN_CHANGE, f->form->edits[field]);
}
static void assert_text(Form *f, int field, const char *expected) {
    char *text = screen_settings_edit_text(f->form->edits[field]); CHECK(str_eq(text, expected)); ssh_secret_free(text, strlen(text));
}
static void edit_save_and_keep(void) {
    Form f; setup(&f, ROW); CHECK(!f.form->dirty);
    CHECK(GetWindowLongPtrW(f.form->edits[S_PASSWORD], GWL_STYLE) & ES_PASSWORD);
    CHECK(GetWindowLongPtrW(f.form->edits[S_SUDO_PASSWORD], GWL_STYLE) & ES_PASSWORD);
    type(&f, S_LABEL, "Changed"); f.fixture.write_response = SAVED;
    screen_settings_ssh_save(f.form); screen_fixture_pump(&f.fixture, 1);
    CHECK_STR(f.fixture.method, "PUT"); CHECK_STR(f.fixture.url, "https://example.com/api/v1/settings/ssh/servers/7");
    CHECK(json_is_null(json_get(f.fixture.body, "password"))); CHECK(json_is_null(json_get(f.fixture.body, "sudoPassword")));
    type(&f, S_PASSWORD, "fixture login"); type(&f, S_SUDO_PASSWORD, "fixture sudo");
    screen_settings_ssh_save(f.form); screen_fixture_pump(&f.fixture, 1);
    CHECK(str_eq(json_str(json_get(f.fixture.body, "password")), "fixture login"));
    CHECK(str_eq(json_str(json_get(f.fixture.body, "sudoPassword")), "fixture sudo"));
    assert_text(&f, S_PASSWORD, ""); assert_text(&f, S_SUDO_PASSWORD, ""); CHECK(!f.form->dirty);
    CHECK(f.form->secrets.fields[0].stored); CHECK(f.form->secrets.fields[1].stored);
    CHECK(json_is_null(json_get(f.form->row, "password")));
    screen_fixture_draw(&f.form->base, &f.doc);
    for (size_t i = 0; i < f.doc.count; i++) CHECK(!str_icontains(f.doc.items[i].text, "fixture login"));
    cleanup(&f);
}
static void per_field_and_remove(void) {
    for (int mask = 0; mask < 4; mask++) {
        Form f; setup(&f, ROW);
        if (!(mask & 1)) json_object_remove(f.form->row, "hasPassword");
        if (!(mask & 2)) json_object_remove(f.form->row, "hasSudoPassword");
        ssh_secrets_free(&f.form->secrets); ssh_secrets_init(&f.form->secrets, f.form->row, NULL, false);
        screen_fixture_draw(&f.form->base, &f.doc);
        CHECK_INT(f.form->laid[S_PASSWORD], (mask & 1) != 0); CHECK_INT(f.form->laid[S_SUDO_PASSWORD], (mask & 2) != 0);
        type(&f, S_PASSWORD, "fixture login"); type(&f, S_SUDO_PASSWORD, "fixture sudo");
        char *why = NULL; Json *body = screen_settings_ssh_body(f.form, &why); CHECK(body != NULL);
        CHECK_INT(json_str(json_get(body, "password")) != NULL, (mask & 1) != 0);
        CHECK_INT(json_str(json_get(body, "sudoPassword")) != NULL, (mask & 2) != 0);
        json_free(body); free(why); cleanup(&f);
    }
    Form f; setup(&f, ROW);
    f.form->base.vt->action(&f.form->base, ACT_SSH_SECRET, 0, (POINT){0}); CHECK(f.form->secrets.fields[0].remove);
    type(&f, S_PASSWORD, "fixture replacement"); CHECK(!f.form->secrets.fields[0].remove);
    f.form->base.vt->action(&f.form->base, ACT_SSH_SECRET, 0, (POINT){0}); assert_text(&f, S_PASSWORD, "");
    f.fixture.write_response = "{\"server\":{\"id\":7,\"hasPassword\":false,\"hasSudoPassword\":false}}";
    screen_settings_ssh_save(f.form); screen_fixture_pump(&f.fixture, 1);
    CHECK_STR(json_str(json_get(f.fixture.body, "password")), ""); CHECK(!f.form->secrets.fields[0].stored); CHECK(!f.form->secrets.fields[0].remove);
    cleanup(&f);
}
static void failures_retain_draft(void) {
    for (int mode = 0; mode < 5; mode++) {
        Form f; setup(&f, ROW); type(&f, S_PASSWORD, "fixture login");
        f.fixture.write_status = mode == 0 ? 400 : 200; f.fixture.fail_write = mode == 1;
        f.fixture.write_response = mode == 2 ? "{\"server\":{\"id\":7}}" : mode == 3 ? "{}" : mode == 4 ? "{\"server\":{\"id\":99,\"hasPassword\":true}}" : SAVED;
        screen_settings_ssh_save(f.form); screen_fixture_pump(&f.fixture, 1);
        assert_text(&f, S_PASSWORD, "fixture login"); CHECK(f.form->dirty); CHECK(f.form->error != NULL);
        CHECK(!str_icontains(f.form->error, "fixture login")); CHECK_INT(f.fixture.writes, 1);
        cleanup(&f);
    }
}
static void edits_during_save(void) {
    Form f; setup(&f, ROW); type(&f, S_PASSWORD, "fixture login"); type(&f, S_SUDO_PASSWORD, "fixture sudo");
    f.fixture.write_response = SAVED; f.fixture.block_write = true;
    screen_settings_ssh_save(f.form); CHECK_INT(WaitForSingleObject(f.fixture.entered, 10000), WAIT_OBJECT_0);
    type(&f, S_PASSWORD, "newer draft"); type(&f, S_LABEL, "New label");
    SetEvent(f.fixture.resume); screen_fixture_pump(&f.fixture, 1);
    assert_text(&f, S_PASSWORD, "newer draft"); assert_text(&f, S_SUDO_PASSWORD, ""); assert_text(&f, S_LABEL, "New label"); CHECK(f.form->dirty);
    cleanup(&f);
}
static void database_edits_during_save(void) {
    for (int mode = 0; mode < 3; mode++) {
        Form f; setup(&f, ROW);
        type(&f, S_DBUSER, "fixture-db"); type(&f, S_DBPASS, "submitted password");
        f.fixture.write_response = "{\"server\":{\"id\":7,\"repo\":\"o/r\",\"host\":\"fixture.invalid\",\"username\":\"fixture\",\"port\":22,\"hasDbCredentials\":true,\"hasPassword\":true,\"hasSudoPassword\":false}}";
        f.fixture.block_write = true;
        screen_settings_ssh_save(f.form); CHECK_INT(WaitForSingleObject(f.fixture.entered, 10000), WAIT_OBJECT_0);
        type(&f, S_LABEL, "New label");
        if (mode) type(&f, S_DBPASS, "newer password");
        if (mode == 2) type(&f, S_DBPASS, "submitted password");
        SetEvent(f.fixture.resume); screen_fixture_pump(&f.fixture, 1);
        const char *expected = mode == 0 ? "" : mode == 1 ? "newer password" : "submitted password";
        assert_text(&f, S_DBPASS, expected); assert_text(&f, S_LABEL, "New label"); CHECK(f.form->dirty);
        char *why = NULL; Json *body = screen_settings_ssh_body(f.form, &why); CHECK(body != NULL); CHECK(why == NULL);
        if (mode == 0) CHECK(json_is_null(json_get(body, "dbPassword")));
        else CHECK_STR(json_str(json_get(body, "dbPassword")), expected);
        json_free(body); free(why); cleanup(&f);
    }
}
static void connection_and_navigation(void) {
    for (int mode = 0; mode < 3; mode++) {
        Form f; setup(&f, ROW); type(&f, S_PASSWORD, "fixture login");
        f.fixture.write_response = SAVED; f.fixture.block_write = true;
        screen_settings_ssh_save(f.form); CHECK_INT(WaitForSingleObject(f.fixture.entered, 10000), WAIT_OBJECT_0);
        SshForm *next = NULL;
        if (mode == 0) { f.form->base.vt->destroy(&f.form->base); Json *row = json_parsez(ROW); next = (SshForm *)screen_settings_ssh_settings_screen_new(row, NULL); json_free(row); }
        if (mode == 1) g_store.client = NULL;
        if (mode == 2) g_store.device.id = "different-device";
        SetEvent(f.fixture.resume); screen_fixture_pump(&f.fixture, 1);
        if (next) { CHECK(!next->dirty); CHECK(!next->secrets.fields[0].bytes); next->base.vt->destroy(&next->base); doc_free(&f.doc); screen_fixture_clear(&f.fixture); }
        else { assert_text(&f, S_PASSWORD, "fixture login"); CHECK(f.form->dirty); g_store.client = f.fixture.client; g_store.device.id = NULL; cleanup(&f); }
    }
}
static void discard(void) {
    Form f; setup(&f, ROW); type(&f, S_PASSWORD, "fixture login");
    for (int i = 0; i < 2; i++) {
        ModalDriver driver = {GetCurrentThreadId(), i ? IDOK : IDCANCEL, false, NULL};
        HANDLE worker = CreateThread(NULL, 0, modal_driver_thread, &driver, 0, NULL); CHECK(worker != NULL);
        bool leave = f.form->base.vt->can_leave(&f.form->base); CHECK_INT(leave, i != 0);
        CHECK_INT(WaitForSingleObject(worker, 15000), WAIT_OBJECT_0); CloseHandle(worker); CHECK(driver.found);
        if (!i) assert_text(&f, S_PASSWORD, "fixture login");
    }
    assert_text(&f, S_PASSWORD, ""); CHECK(!ssh_secrets_changed(&f.form->secrets));
    cleanup(&f);
}
static void new_form_capabilities(void) {
    for (int mode = 0; mode < 3; mode++) {
        ScreenFixture fixture; screen_fixture_init(&fixture, ROUTES, sizeof ROUTES / sizeof *ROUTES);
        SettingsScreen settings = {0}; settings.ssh_account = fixture.client; settings.ssh_loaded = true;
        settings.ssh = json_parsez(mode == 0 ? "{\"list\":[]}" : mode == 1 ? "{\"list\":[{\"hasPassword\":false}]}" : "{}");
        screen_settings_g_settings = &settings;
        Json *defaults = json_parsez("{\"repo\":\"o/r\",\"host\":\"fixture.invalid\",\"username\":\"fixture\",\"port\":22}");
        SshForm *form = (SshForm *)screen_settings_ssh_settings_screen_new(NULL, defaults);
        CHECK(form->secrets.fields[0].offered == (mode < 2)); CHECK(form->secrets.fields[1].offered == (mode == 0));
        screen_settings_g_settings = NULL;
        screen_fixture_pane(&fixture, &form->base); form->base.vt->visible(&form->base, true);
        if (mode < 2) {
            screen_settings_set_edit_text(form->edits[S_PASSWORD], "fixture login");
            form->base.vt->command(&form->base, ID_SSH_FIELD + S_PASSWORD, EN_CHANGE, form->edits[S_PASSWORD]);
        }
        fixture.write_response = "{\"server\":{\"id\":8,\"repo\":\"o/r\",\"host\":\"fixture.invalid\",\"username\":\"fixture\",\"port\":22,\"hasPassword\":true}}";
        screen_settings_ssh_save(form); screen_fixture_pump(&fixture, 1);
        CHECK_STR(fixture.method, "POST"); CHECK_STR(fixture.url, "https://example.com/api/v1/settings/ssh/servers"); CHECK_INT(form->id, 8);
        CHECK(json_is_null(json_get(fixture.body, "sudoPassword"))); CHECK(!ssh_secrets_changed(&form->secrets));
        form->base.vt->destroy(&form->base); json_free(defaults); json_free(settings.ssh); screen_fixture_clear(&fixture);
    }
}
void app_ssh_settings_tests(void) {
    screen_fixture_prepare();
    test_run("SSH create capability inference from current loaded list", new_form_capabilities);
    test_run("SSH masked controls keep replace and refill", edit_save_and_keep);
    test_run("SSH independently offered controls and removal", per_field_and_remove);
    test_run("SSH refusal uncertain and unconfirmed saves retain drafts", failures_retain_draft);
    test_run("SSH completion preserves newer secret and unrelated edits", edits_during_save);
    test_run("SSH database password acknowledgement preserves newer edits", database_edits_during_save);
    test_run("SSH save scope and cancellation on navigation", connection_and_navigation);
    test_run("SSH draft discard confirmation", discard);
}
