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
static bool has_control(Form *f, int action, int field) {
    screen_fixture_draw(&f->form->base, &f->doc);
    for (size_t i = 0; i < f->doc.count; i++) {
        Item *it = &f->doc.items[i];
        if (it->action == action && it->arg == field) return true;
    }
    return false;
}
static bool offered_control(Form *f) { return has_control(f, ACT_TOGGLE, F_AUTO_LOOP); }
static Json *body_of(Form *f, char **why, int *tab) { *why = NULL; *tab = T_PROJECT; return screen_settings_form_body(f->form, why, tab); }
// json_get answers a missing key with null, so a key sent as null is told from one left out by the object's keys.
static bool has_key(const Json *object, const char *key) {
    for (size_t i = 0; i < json_count(object); i++) if (str_eq(json_key(object, i), key)) return true;
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
static const char *const REVIEW_ROW = "{\"id\":4,\"repo\":\"o/r\",\"prAuthor\":\"octocat\",\"reviewRoundLimit\":3,"
    "\"securityReview\":\"off\",\"enforceReviewOutcome\":false,\"reviewBenchmark\":false,\"workerBudgetUsd\":null}";
static void test_review_round_limit(void) {
    Json *row = json_parsez(REVIEW_ROW);
    Form f; setup(&f, row, false);
    CHECK(f.form->edits[F_ROUND_LIMIT] != NULL); CHECK_OWNED_STR(screen_settings_edit_text(f.form->edits[F_ROUND_LIMIT]), "3");
    CHECK(has_control(&f, ACT_FOCUS, F_ROUND_LIMIT)); CHECK(!screen_settings_tab_changed(f.form, T_REVIEW));
    static const struct { const char *typed; int sent; } valid[] = { { "0", 0 }, { "1", 1 }, { " 7 ", 7 }, { "20", 20 } };
    for (size_t i = 0; i < sizeof valid / sizeof *valid; i++) {
        screen_settings_set_edit_text(f.form->edits[F_ROUND_LIMIT], valid[i].typed);
        char *why; int tab; Json *body = body_of(&f, &why, &tab);
        CHECK(body != NULL); CHECK(why == NULL);
        CHECK_INT(json_int_or(json_get(body, "reviewRoundLimit"), -1), valid[i].sent);
        free(why); json_free(body);
    }
    // Left empty, the server's default: the key goes as null rather than being dropped.
    screen_settings_set_edit_text(f.form->edits[F_ROUND_LIMIT], "  ");
    char *why; int tab; Json *body = body_of(&f, &why, &tab);
    CHECK(body != NULL); CHECK(has_key(body, "reviewRoundLimit")); CHECK(json_is_null(json_get(body, "reviewRoundLimit")));
    CHECK(screen_settings_tab_changed(f.form, T_REVIEW));
    free(why); json_free(body);
    static const char *const invalid[] = { "21", "2.5", "-1", "1e1", "+3", "three" };
    for (size_t i = 0; i < sizeof invalid / sizeof *invalid; i++) {
        screen_settings_set_edit_text(f.form->edits[F_ROUND_LIMIT], invalid[i]);
        body = body_of(&f, &why, &tab);
        CHECK(body == NULL); CHECK(why != NULL); CHECK_INT(tab, T_REVIEW);
        free(why); json_free(body);
    }
    // The budget keeps taking dollars and cents.
    screen_settings_set_edit_text(f.form->edits[F_ROUND_LIMIT], "");
    screen_settings_set_edit_text(f.form->edits[F_BUDGET], "12.5");
    body = body_of(&f, &why, &tab);
    CHECK(body != NULL); CHECK(json_num_or(json_get(body, "workerBudgetUsd"), 0) == 12.5);
    free(why); json_free(body);
    cleanup(&f); json_free(row);
}
static void test_review_rules_choice_and_switches(void) {
    Json *row = json_parsez(REVIEW_ROW);
    Form f; setup(&f, row, false);
    CHECK_STR(f.form->choices[F_SECURITY], "off");
    CHECK(has_control(&f, ACT_CHOOSE, F_SECURITY));
    CHECK(has_control(&f, ACT_TOGGLE, F_ENFORCE_OUTCOME)); CHECK(has_control(&f, ACT_TOGGLE, F_BENCHMARK));
    CHECK(!screen_settings_tab_changed(f.form, T_REVIEW));
    set_string(&f.form->choices[F_SECURITY], "required"); screen_settings_form_changed(f.form);
    CHECK(f.form->dirty); CHECK(screen_settings_tab_changed(f.form, T_REVIEW));
    f.form->base.vt->action(&f.form->base, ACT_TOGGLE, F_ENFORCE_OUTCOME, (POINT){0});
    f.form->base.vt->action(&f.form->base, ACT_TOGGLE, F_BENCHMARK, (POINT){0});
    Json *reply = json_parsez("{\"project\":{\"id\":4,\"repo\":\"o/r\",\"reviewRoundLimit\":3,\"securityReview\":\"required\","
                              "\"enforceReviewOutcome\":true,\"reviewBenchmark\":true}}");
    char *answer = json_serialize(reply, false); f.fixture.write_response = answer;
    screen_settings_form_save(f.form); screen_fixture_pump(&f.fixture, 1);
    CHECK_STR(f.fixture.method, "PUT");
    CHECK_STR(json_str(json_get(f.fixture.body, "securityReview")), "required");
    CHECK(json_bool_is(json_get(f.fixture.body, "enforceReviewOutcome"), true));
    CHECK(json_bool_is(json_get(f.fixture.body, "reviewBenchmark"), true));
    CHECK_INT(json_int_or(json_get(f.fixture.body, "reviewRoundLimit"), -1), 3);
    // The saved row refills the form, and nothing is left unsaved.
    CHECK(!f.form->dirty); CHECK(!screen_settings_tab_changed(f.form, T_REVIEW));
    CHECK_STR(f.form->choices[F_SECURITY], "required"); CHECK(f.form->bools[F_ENFORCE_OUTCOME]); CHECK(f.form->bools[F_BENCHMARK]);
    // A value the field does not list is refused, on the tab that holds it.
    set_string(&f.form->choices[F_SECURITY], "strict");
    char *why; int tab; Json *body = body_of(&f, &why, &tab);
    CHECK(body == NULL); CHECK(why != NULL); CHECK_INT(tab, T_REVIEW);
    free(why); json_free(body);
    cleanup(&f); free(answer); json_free(reply); json_free(row);
    // A row from before the setting, empty or null, leaves the key out so the server keeps what it has.
    static const char *const empty[] = { "{\"id\":4,\"repo\":\"o/r\",\"securityReview\":\"\"}", "{\"id\":4,\"repo\":\"o/r\",\"securityReview\":null}" };
    for (size_t i = 0; i < sizeof empty / sizeof *empty; i++) {
        row = json_parsez(empty[i]);
        setup(&f, row, false);
        CHECK(has_control(&f, ACT_CHOOSE, F_SECURITY));
        body = body_of(&f, &why, &tab);
        CHECK(body != NULL); CHECK(why == NULL); CHECK(!has_key(body, "securityReview"));
        free(why); json_free(body); cleanup(&f); json_free(row);
    }
}
static void test_row_without_review_rules(void) {
    Json *row = json_parsez("{\"id\":4,\"repo\":\"o/r\",\"prAuthor\":\"octocat\",\"autonomousReviewLoop\":false}");
    Form f; setup(&f, row, false);
    static const int fields[] = { F_ROUND_LIMIT, F_SECURITY, F_ENFORCE_OUTCOME, F_BENCHMARK };
    for (size_t i = 0; i < sizeof fields / sizeof *fields; i++) CHECK(!screen_settings_field_offered(f.form, fields[i]));
    CHECK(!has_control(&f, ACT_FOCUS, F_ROUND_LIMIT)); CHECK(!f.form->laid[F_ROUND_LIMIT]);
    CHECK(!has_control(&f, ACT_CHOOSE, F_SECURITY));
    CHECK(!has_control(&f, ACT_TOGGLE, F_ENFORCE_OUTCOME)); CHECK(!has_control(&f, ACT_TOGGLE, F_BENCHMARK));
    CHECK(has_control(&f, ACT_FOCUS, F_REVIEW_AUTHOR));
    f.fixture.write_response = "{\"project\":{\"id\":4,\"repo\":\"o/r\"}}";
    toggle(&f); screen_settings_form_save(f.form); screen_fixture_pump(&f.fixture, 1);
    CHECK_STR(f.fixture.method, "PUT"); CHECK(json_bool_is(json_get(f.fixture.body, "autonomousReviewLoop"), true));
    static const char *const keys[] = { "reviewRoundLimit", "securityReview", "enforceReviewOutcome", "reviewBenchmark" };
    for (size_t i = 0; i < sizeof keys / sizeof *keys; i++) CHECK(!has_key(f.fixture.body, keys[i]));
    cleanup(&f); json_free(row);
}

void app_project_settings_tests(void) {
    screen_fixture_prepare();
    test_run("project settings round trip current action defaults and PR author", test_current_action_defaults_and_pr_author);
    test_run("project review flag edit save refill and reopen", test_review_flag_edit_save_reopen);
    test_run("project create and clone send only offered settings, never removed ones", test_create_and_clone_send_only_offered_settings);
    test_run("project save failure keeps edits and discard preserves saved value", test_failure_and_discard);
    test_run("project save completion is cancelled on navigation", test_destroy_during_save);
    test_run("project review round limit takes 0 to 20 or empty for the default", test_review_round_limit);
    test_run("project security review choice and review switches round trip", test_review_rules_choice_and_switches);
    test_run("project row without the review rules neither shows nor sends them", test_row_without_review_rules);
}
