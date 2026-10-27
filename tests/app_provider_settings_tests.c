#include "screen_fixture.h"
#include "screen_provider_settings_internal.h"
#include "suites.h"

static Route ROUTES[] = {
    {"GET", "/settings/providers", "admin"},
    {"GET", "/settings/providers/{id}/status", "admin"},
    {"POST", "/settings/providers/{id}/login", "admin"},
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
void app_provider_settings_tests(void) {
    screen_fixture_prepare();
    test_run("provider re-login retains code until account changes", retained_and_changed);
    test_run("provider first login clears code after connection", first_login);
    test_run("provider initial and previous reads gate new login", initial_and_previous_reads);
    test_run("provider pending login suppresses reads and handles errors", pending_login_and_errors);
    test_run("provider picker cancellation preserves initial status read", picker_cancellation_preserves_initial_read);
    test_run("provider cancelled old attempt cannot clear current code", cancelled_attempt);
    test_run("provider teardown cancels pending status and login", teardown);
}
