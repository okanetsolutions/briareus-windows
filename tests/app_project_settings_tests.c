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
static void toggle(Form *f) { f->form->base.vt->action(&f->form->base, ACT_TOGGLE, F_AUTO_LOOP, (POINT){0}); }
static bool offered_control(Form *f) {
    screen_fixture_draw(&f->form->base, &f->doc);
    for (size_t i = 0; i < f->doc.count; i++) {
        Item *it = &f->doc.items[i];
        if (it->action == ACT_TOGGLE && it->arg == F_AUTO_LOOP) return true;
    }
    return false;
}
static void test_review_flag_edit_save_reopen(void) {
    for (int on = 0; on <= 1; on++) {
        Json *row = json_parsez("{\"id\":4,\"repo\":\"o/r\",\"enabled\":true,\"isSelf\":true,\"unknown\":17}");
        json_set_bool(row, "autonomousReviewLoop", on != 0);
        Form f; setup(&f, row, false);
        CHECK(offered_control(&f)); CHECK_INT(f.form->bools[F_AUTO_LOOP], on);
        CHECK(!screen_settings_tab_changed(f.form, T_REVIEW));
        toggle(&f); CHECK(f.form->dirty); CHECK(screen_settings_tab_changed(f.form, T_REVIEW));
        toggle(&f); CHECK(!screen_settings_tab_changed(f.form, T_REVIEW));
        screen_settings_form_fill(f.form); CHECK(!f.form->dirty);
        toggle(&f);
        json_set_bool(row, "autonomousReviewLoop", !on);
        Json *reply = json_object(); json_object_set(reply, "project", json_clone(row));
        char *answer = json_serialize(reply, false); f.fixture.write_response = answer;
        screen_settings_form_save(f.form); screen_fixture_pump(&f.fixture, 1);
        CHECK_STR(f.fixture.method, "PUT"); CHECK_STR(f.fixture.url, "https://example.com/api/v1/settings/projects/4");
        CHECK(json_bool_is(json_get(f.fixture.body, "autonomousReviewLoop"), !on));
        CHECK(json_bool_is(json_get(f.fixture.body, "enabled"), true));
        CHECK(json_bool_is(json_get(f.fixture.body, "isSelf"), true));
        CHECK_INT(json_int_or(json_get(f.fixture.body, "unknown"), 0), 17);
        CHECK(!f.form->dirty); CHECK(!screen_settings_tab_changed(f.form, T_REVIEW));
        CHECK_INT(f.form->bools[F_AUTO_LOOP], !on);
        FormScreen *reopened = (FormScreen *)project_settings_screen_new(f.form->row, NULL);
        CHECK_INT(reopened->bools[F_AUTO_LOOP], !on);
        reopened->base.vt->destroy(&reopened->base);
        cleanup(&f); free(answer); json_free(reply); json_free(row);
    }
}
static void test_create_and_clone_send_only_offered_settings(void) {
    // Empty defaults offer every field; defaults without a key leave its control and key out.
    for (int mode = 0; mode < 2; mode++) {
        Json *defaults = mode ? json_parsez("{\"repo\":\"o/new\",\"dbPoolDatabase\":\"app\",\"unknown\":17}") : json_object();
        Form f; setup(&f, defaults, true);
        if (mode == 0) screen_settings_set_edit_text(f.form->edits[F_REPO], "o/new");
        CHECK_INT(offered_control(&f), mode == 0);
        // Clones carry the current body without an id, then clear only the unique repository.
        char *why = NULL; int tab = T_PROJECT;
        Json *copy = screen_settings_form_body(f.form, &why, &tab); CHECK(copy != NULL); CHECK(why == NULL);
        json_set_str(copy, "repo", "");
        FormScreen *clone = (FormScreen *)project_settings_screen_new(copy, NULL);
        CHECK_INT(clone->id, 0); CHECK_STR(json_str(json_get(clone->row, "repo")), "");
        if (mode) CHECK_INT(json_int_or(json_get(clone->row, "unknown"), 0), 17);
        clone->base.vt->destroy(&clone->base); json_free(copy); free(why);
        f.fixture.write_response = "{\"project\":{\"id\":8,\"repo\":\"o/new\"}}";
        screen_settings_form_save(f.form); screen_fixture_pump(&f.fixture, 1);
        CHECK_STR(f.fixture.method, "POST"); CHECK_STR(f.fixture.url, "https://example.com/api/v1/settings/projects");
        CHECK_STR(json_str(json_get(f.fixture.body, "repo")), "o/new");
        CHECK_STR(json_str(json_get(f.fixture.body, "dbPoolDatabase")), mode ? "app" : "");
        CHECK_INT(json_is_null(json_get(f.fixture.body, "autonomousReviewLoop")), mode == 1);
        // Settings the server removed are never sent, whatever the defaults say.
        CHECK(json_is_null(json_get(f.fixture.body, "babysitPrs")));
        CHECK(json_is_null(json_get(f.fixture.body, "dbPoolEnabled")));
        CHECK(json_is_null(json_get(f.fixture.body, "dbRestoreSql")));
        CHECK_INT(f.form->id, 8); CHECK(!f.form->dirty);
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
    Json *row = json_parsez("{\"id\":4,\"repo\":\"o/r\",\"autonomousReviewLoop\":false}");
    Form f; setup(&f, row, false); toggle(&f);
    f.fixture.write_status = 503;
    screen_settings_form_save(f.form); screen_fixture_pump(&f.fixture, 1);
    CHECK(f.form->dirty); CHECK(f.form->bools[F_AUTO_LOOP]); CHECK(f.form->error != NULL);
    CHECK(!leave_with_answer(&f, IDCANCEL)); CHECK(f.form->dirty);
    CHECK(leave_with_answer(&f, IDOK)); CHECK(!f.form->dirty);
    FormScreen *reopened = (FormScreen *)project_settings_screen_new(f.form->row, NULL);
    CHECK(!reopened->bools[F_AUTO_LOOP]);
    reopened->base.vt->destroy(&reopened->base);
    cleanup(&f); json_free(row);
}
static void test_destroy_during_save(void) {
    Json *row = json_parsez("{\"id\":4,\"repo\":\"o/r\",\"autonomousReviewLoop\":false}");
    Form f; setup(&f, row, false); toggle(&f);
    f.fixture.block_write = true;
    f.fixture.write_response = "{\"project\":{\"id\":4,\"repo\":\"o/r\",\"autonomousReviewLoop\":true}}";
    screen_settings_form_save(f.form);
    CHECK_INT(WaitForSingleObject(f.fixture.entered, 10000), WAIT_OBJECT_0);
    f.form->base.vt->destroy(&f.form->base);
    FormScreen *next = (FormScreen *)project_settings_screen_new(NULL, NULL);
    SetEvent(f.fixture.resume); screen_fixture_pump(&f.fixture, 1);
    CHECK(!next->bools[F_AUTO_LOOP]); CHECK(!next->dirty); CHECK_INT(next->id, 0);
    next->base.vt->destroy(&next->base);
    doc_free(&f.doc); screen_fixture_clear(&f.fixture); json_free(row); screen_settings_g_tab = T_PROJECT;
}
static void test_current_action_defaults_and_pr_author(void) {
    Json *row = json_parsez("{\"id\":4,\"repo\":\"o/r\",\"prAuthor\":\"octocat\",\"actionProviderId\":2,\"actionModel\":\"opus\",\"actionEffort\":\"high\"}");
    Form f; setup(&f, row, false);
    CHECK_INT(f.form->picks[R_REVIEW].provider_id, 2); CHECK_STR(f.form->picks[R_REVIEW].model, "opus");
    CHECK_STR(f.form->picks[R_REVIEW].effort, "high"); CHECK(screen_settings_field_offered(f.form, F_REVIEW_AUTHOR));
    CHECK_OWNED_STR(screen_settings_field_text(row, F_REVIEW_AUTHOR), "octocat");
    set_string(&f.form->picks[R_REVIEW].model, "sonnet"); set_string(&f.form->picks[R_REVIEW].effort, "medium");
    char *why = NULL; int tab = 0; Json *body = screen_settings_form_body(f.form, &why, &tab);
    CHECK(body != NULL); CHECK(why == NULL); CHECK_INT(json_int_or(json_get(body, "actionProviderId"), 0), 2);
    CHECK_STR(json_str(json_get(body, "actionModel")), "sonnet"); CHECK_STR(json_str(json_get(body, "actionEffort")), "medium");
    CHECK_STR(json_str(json_get(body, "prAuthor")), "octocat");
    CHECK(json_is_null(json_get(body, "reviewProviderId"))); CHECK(json_is_null(json_get(body, "reviewAuthor")));
    free(why); json_free(body); cleanup(&f); json_free(row);
}

void app_project_settings_tests(void) {
    screen_fixture_prepare();
    test_run("project settings round trip current action defaults and PR author", test_current_action_defaults_and_pr_author);
    test_run("project review flag edit save refill and reopen", test_review_flag_edit_save_reopen);
    test_run("project create and clone send only offered settings, never removed ones", test_create_and_clone_send_only_offered_settings);
    test_run("project save failure keeps edits and discard preserves saved value", test_failure_and_discard);
    test_run("project save completion is cancelled on navigation", test_destroy_during_save);
}
