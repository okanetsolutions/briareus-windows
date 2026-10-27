#include "screen_fixture.h"
#include "screen_settings_internal.h"
#include "modal_driver.h"
#include "suites.h"

static Route ROUTES[] = {
    {"GET", "/settings/projects", "admin"},
    {"POST", "/settings/projects", "admin"}, {"PUT", "/settings/projects/{id}", "admin"},
};
typedef struct { ScreenFixture fixture; FormScreen *form; Doc doc; } Form;

static void setup(Form *f, const Json *row, bool create) {
    memset(f, 0, sizeof *f);
    screen_fixture_init(&f->fixture, ROUTES, sizeof ROUTES / sizeof *ROUTES);
    f->form = (FormScreen *)project_settings_screen_new(create ? NULL : row, create ? row : NULL);
    doc_init(&f->doc); screen_fixture_pane(&f->fixture, &f->form->base);
    f->form->base.vt->visible(&f->form->base, true);
    screen_settings_g_tab = T_REVIEW;
}
static void cleanup(Form *f) {
    f->form->base.vt->destroy(&f->form->base); doc_free(&f->doc);
    screen_fixture_clear(&f->fixture); screen_settings_g_tab = T_PROJECT;
}
static void toggle(Form *f) { f->form->base.vt->action(&f->form->base, ACT_TOGGLE, F_BABYSIT, (POINT){0}); }
static bool offered_control(Form *f) {
    screen_fixture_draw(&f->form->base, &f->doc);
    for (size_t i = 0; i < f->doc.count; i++) {
        Item *it = &f->doc.items[i];
        if (it->action == ACT_TOGGLE && it->arg == F_BABYSIT) return true;
    }
    return false;
}
static void test_supported_edit_save_reopen(void) {
    for (int on = 0; on <= 1; on++) {
        Json *row = json_parsez("{\"id\":4,\"repo\":\"o/r\",\"enabled\":true,\"autonomousReviewLoop\":true,\"isSelf\":true,\"unknown\":17}");
        json_set_bool(row, "babysitPrs", on != 0);
        Form f; setup(&f, row, false);
        CHECK(offered_control(&f)); CHECK_INT(f.form->babysit.on, on);
        CHECK(!screen_settings_tab_changed(f.form, T_REVIEW));
        toggle(&f); CHECK(f.form->dirty); CHECK(screen_settings_tab_changed(f.form, T_REVIEW));
        toggle(&f); CHECK(!screen_settings_tab_changed(f.form, T_REVIEW));
        screen_settings_form_fill(f.form); CHECK(!f.form->dirty);
        toggle(&f);
        json_set_bool(row, "babysitPrs", !on);
        Json *reply = json_object(); json_object_set(reply, "project", json_clone(row));
        char *answer = json_serialize(reply, false); f.fixture.write_response = answer;
        screen_settings_form_save(f.form); screen_fixture_pump(&f.fixture, 1);
        CHECK_STR(f.fixture.method, "PUT"); CHECK_STR(f.fixture.url, "https://example.com/api/v1/settings/projects/4");
        CHECK(json_bool_is(json_get(f.fixture.body, "babysitPrs"), !on));
        CHECK(json_bool_is(json_get(f.fixture.body, "autonomousReviewLoop"), true));
        CHECK(json_bool_is(json_get(f.fixture.body, "enabled"), true));
        CHECK(json_bool_is(json_get(f.fixture.body, "isSelf"), true));
        CHECK_INT(json_int_or(json_get(f.fixture.body, "unknown"), 0), 17);
        CHECK(!f.form->dirty); CHECK(!screen_settings_tab_changed(f.form, T_REVIEW));
        CHECK_INT(f.form->babysit.on, !on);
        FormScreen *reopened = (FormScreen *)project_settings_screen_new(f.form->row, NULL);
        CHECK_INT(reopened->babysit.on, !on); CHECK(reopened->babysit.supported);
        reopened->base.vt->destroy(&reopened->base);
        cleanup(&f); free(answer); json_free(reply); json_free(row);
    }
}
static void test_create_clone_and_unsupported(void) {
    for (int mode = 0; mode < 4; mode++) {
        Json *defaults = json_parsez("{\"repo\":\"o/new\",\"autonomousReviewLoop\":true,\"unknown\":17}");
        if (mode >= 2) json_set_bool(defaults, "babysitPrs", mode == 3);
        if (mode == 0) { json_free(defaults); defaults = json_object(); }
        Form f; setup(&f, defaults, true);
        if (mode == 0) screen_settings_set_edit_text(f.form->edits[F_REPO], "o/new");
        CHECK_INT(offered_control(&f), mode >= 2);
        if (mode < 2) {
            toggle(&f); CHECK(!f.form->babysit.on);
            screen_settings_form_fill(f.form); CHECK(!screen_settings_tab_changed(f.form, T_REVIEW));
            if (mode == 0) screen_settings_set_edit_text(f.form->edits[F_REPO], "o/new");
        }
        if (mode == 2) toggle(&f);
        // Clones carry the current body without an id, then clear only the unique repository.
        char *why = NULL; int tab = T_PROJECT;
        Json *copy = screen_settings_form_body(f.form, &why, &tab); CHECK(copy != NULL); CHECK(why == NULL);
        json_set_str(copy, "repo", "");
        FormScreen *clone = (FormScreen *)project_settings_screen_new(copy, NULL);
        CHECK_INT(clone->babysit.supported, mode >= 2); CHECK_INT(clone->babysit.on, mode >= 2);
        CHECK_INT(clone->id, 0); CHECK_STR(json_str(json_get(clone->row, "repo")), "");
        if (mode) CHECK_INT(json_int_or(json_get(clone->row, "unknown"), 0), 17);
        clone->base.vt->destroy(&clone->base); json_free(copy); free(why);
        f.fixture.write_response = mode >= 2 ? "{\"project\":{\"id\":8,\"repo\":\"o/new\",\"babysitPrs\":false}}" : "{\"project\":{\"id\":8,\"repo\":\"o/new\"}}";
        screen_settings_form_save(f.form); screen_fixture_pump(&f.fixture, 1);
        CHECK_STR(f.fixture.method, "POST"); CHECK_STR(f.fixture.url, "https://example.com/api/v1/settings/projects");
        if (mode < 2) CHECK(json_is_null(json_get(f.fixture.body, "babysitPrs")));
        else CHECK(json_bool_is(json_get(f.fixture.body, "babysitPrs"), mode >= 2));
        CHECK_INT(f.form->id, 8); CHECK(!f.form->dirty);
        CHECK_INT(f.form->babysit.supported, mode >= 2); CHECK(!f.form->babysit.on);
        cleanup(&f); json_free(defaults);
    }
}
static bool leave_with_answer(Form *f, int answer) {
    ModalDriver driver = {GetCurrentThreadId(), answer, false, NULL};
    HANDLE worker = CreateThread(NULL, 0, modal_driver_thread, &driver, 0, NULL);
    CHECK(worker != NULL); if (!worker) return false;
    bool leave = f->form->base.vt->can_leave(&f->form->base);
    CHECK_INT(WaitForSingleObject(worker, 15000), WAIT_OBJECT_0); CHECK(driver.found);
    CloseHandle(worker); return leave;
}
static void test_failure_and_discard(void) {
    Json *row = json_parsez("{\"id\":4,\"repo\":\"o/r\",\"babysitPrs\":false}");
    Form f; setup(&f, row, false); toggle(&f);
    f.fixture.write_status = 503;
    screen_settings_form_save(f.form); screen_fixture_pump(&f.fixture, 1);
    CHECK(f.form->dirty); CHECK(f.form->babysit.on); CHECK(f.form->error != NULL);
    CHECK(!leave_with_answer(&f, IDCANCEL)); CHECK(f.form->dirty);
    CHECK(leave_with_answer(&f, IDOK)); CHECK(!f.form->dirty);
    FormScreen *reopened = (FormScreen *)project_settings_screen_new(f.form->row, NULL);
    CHECK(reopened->babysit.supported); CHECK(!reopened->babysit.on);
    reopened->base.vt->destroy(&reopened->base);
    cleanup(&f); json_free(row);
}
static void test_destroy_during_save(void) {
    Json *row = json_parsez("{\"id\":4,\"repo\":\"o/r\",\"babysitPrs\":false}");
    Form f; setup(&f, row, false); toggle(&f);
    f.fixture.block_write = true;
    f.fixture.write_response = "{\"project\":{\"id\":4,\"repo\":\"o/r\",\"babysitPrs\":true}}";
    screen_settings_form_save(f.form);
    CHECK_INT(WaitForSingleObject(f.fixture.entered, 10000), WAIT_OBJECT_0);
    f.form->base.vt->destroy(&f.form->base);
    FormScreen *next = (FormScreen *)project_settings_screen_new(NULL, NULL);
    SetEvent(f.fixture.resume); screen_fixture_pump(&f.fixture, 1);
    CHECK(!next->babysit.supported); CHECK(!next->dirty);
    next->base.vt->destroy(&next->base);
    doc_free(&f.doc); screen_fixture_clear(&f.fixture); json_free(row); screen_settings_g_tab = T_PROJECT;
}
void app_project_settings_tests(void) {
    screen_fixture_prepare();
    test_run("project babysit edit save refill and reopen", test_supported_edit_save_reopen);
    test_run("project babysit defaults clone and unsupported absence", test_create_clone_and_unsupported);
    test_run("project babysit failure keeps edits and discard preserves saved value", test_failure_and_discard);
    test_run("project babysit save completion is cancelled on navigation", test_destroy_during_save);
}
