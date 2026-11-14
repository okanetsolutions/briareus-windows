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

// The review rules on the Code review tab: which of them the form draws, as items a click reaches.
static bool drawn(Form *f, int action, intptr_t arg) {
    screen_fixture_draw(&f->form->base, &f->doc);
    for (size_t i = 0; i < f->doc.count; i++) if (f->doc.items[i].action == action && f->doc.items[i].arg == arg) return true;
    return false;
}
static bool has_key(const Json *object, const char *key) {
    for (size_t i = 0; i < json_count(object); i++) if (str_eq(json_key(object, i), key)) return true;
    return false;
}
static Json *body_of(Form *f, char **why, int *tab) { *why = NULL; *tab = T_PROJECT; return screen_settings_form_body(f->form, why, tab); }
static void test_review_rules_edit_and_save(void) {
    Json *row = json_parsez("{\"id\":4,\"repo\":\"o/r\",\"prAuthor\":\"octocat\",\"reviewRoundLimit\":3,\"securityReview\":\"advisory\","
        "\"enforceReviewOutcome\":false,\"reviewBenchmark\":true}");
    Form f; setup(&f, row, false);
    CHECK(drawn(&f, ACT_CHOOSE, F_SECURITY_REVIEW * CHOICE_STRIDE));
    CHECK(drawn(&f, ACT_TOGGLE, F_ENFORCE_OUTCOME)); CHECK(drawn(&f, ACT_TOGGLE, F_BENCHMARK));
    CHECK(f.form->laid[F_ROUND_LIMIT]); CHECK_STR(f.form->choices[F_SECURITY_REVIEW], "advisory");
    CHECK_OWNED_STR(screen_settings_edit_text(f.form->edits[F_ROUND_LIMIT]), "3");
    CHECK(f.form->bools[F_BENCHMARK]); CHECK(!screen_settings_tab_changed(f.form, T_REVIEW));
    // Required, then back to Advisory, leaves nothing changed; an index past the choices changes nothing.
    f.form->base.vt->action(&f.form->base, ACT_CHOOSE, F_SECURITY_REVIEW * CHOICE_STRIDE + 2, (POINT){0});
    CHECK_STR(f.form->choices[F_SECURITY_REVIEW], "required"); CHECK(screen_settings_tab_changed(f.form, T_REVIEW));
    f.form->base.vt->action(&f.form->base, ACT_CHOOSE, F_SECURITY_REVIEW * CHOICE_STRIDE + 1, (POINT){0});
    CHECK(!screen_settings_tab_changed(f.form, T_REVIEW));
    CHECK(!screen_settings_choose(f.form, F_SECURITY_REVIEW, 3)); CHECK(!screen_settings_choose(f.form, F_ENFORCE_OUTCOME, 0));
    CHECK_STR(f.form->choices[F_SECURITY_REVIEW], "advisory");
    f.form->base.vt->action(&f.form->base, ACT_CHOOSE, F_SECURITY_REVIEW * CHOICE_STRIDE, (POINT){0});
    f.form->base.vt->action(&f.form->base, ACT_TOGGLE, F_ENFORCE_OUTCOME, (POINT){0});
    screen_settings_set_edit_text(f.form->edits[F_ROUND_LIMIT], "");
    f.fixture.write_response = "{\"project\":{\"id\":4,\"repo\":\"o/r\",\"reviewRoundLimit\":null,\"securityReview\":\"off\",\"enforceReviewOutcome\":true}}";
    screen_settings_form_save(f.form); screen_fixture_pump(&f.fixture, 1);
    CHECK_STR(f.fixture.method, "PUT");
    CHECK_STR(json_str(json_get(f.fixture.body, "securityReview")), "off");
    CHECK(json_bool_is(json_get(f.fixture.body, "enforceReviewOutcome"), true));
    CHECK(json_bool_is(json_get(f.fixture.body, "reviewBenchmark"), true));
    CHECK(has_key(f.fixture.body, "reviewRoundLimit")); CHECK(json_is_null(json_get(f.fixture.body, "reviewRoundLimit")));
    CHECK_STR(json_str(json_get(f.fixture.body, "prAuthor")), "octocat");
    // The saved row refills the form.
    CHECK(!f.form->dirty); CHECK_STR(f.form->choices[F_SECURITY_REVIEW], "off"); CHECK(f.form->bools[F_ENFORCE_OUTCOME]);
    cleanup(&f); json_free(row);
}
static void test_review_round_limit_bounds(void) {
    Json *row = json_parsez("{\"id\":4,\"repo\":\"o/r\",\"reviewRoundLimit\":null}");
    Form f; setup(&f, row, false);
    CHECK_OWNED_STR(screen_settings_edit_text(f.form->edits[F_ROUND_LIMIT]), "");
    static const struct { const char *typed; int sent; } accepted[] = { {"0", 0}, {"1", 1}, {" 20 ", 20} };
    for (size_t i = 0; i < sizeof accepted / sizeof *accepted; i++) {
        screen_settings_set_edit_text(f.form->edits[F_ROUND_LIMIT], accepted[i].typed);
        char *why; int tab; Json *body = body_of(&f, &why, &tab);
        CHECK(body != NULL); CHECK(why == NULL);
        CHECK_INT(json_int_or(json_get(body, "reviewRoundLimit"), -1), accepted[i].sent);
        json_free(body); free(why);
    }
    static const char *const refused[] = { "21", "-1", "2.5", "three", "3x" };
    for (size_t i = 0; i < sizeof refused / sizeof *refused; i++) {
        screen_settings_set_edit_text(f.form->edits[F_ROUND_LIMIT], refused[i]);
        char *why; int tab; Json *body = body_of(&f, &why, &tab);
        CHECK(body == NULL); CHECK(why != NULL); CHECK_INT(tab, T_REVIEW);
        free(why);
    }
    // A refused save names the field and sends nothing.
    screen_settings_form_save(f.form);
    CHECK(f.form->error != NULL); CHECK(f.form->req_save == NULL); CHECK_INT(screen_settings_g_tab, T_REVIEW);
    cleanup(&f); json_free(row);
}
static void test_security_review_empty_and_unknown(void) {
    // Empty, a row from before the setting: the key is left out and the server keeps what it has.
    Json *empty = json_parsez("{\"id\":4,\"repo\":\"o/r\",\"securityReview\":null}");
    Form f; setup(&f, empty, false);
    CHECK(drawn(&f, ACT_CHOOSE, F_SECURITY_REVIEW * CHOICE_STRIDE)); CHECK_STR(f.form->choices[F_SECURITY_REVIEW], "");
    char *why; int tab; Json *body = body_of(&f, &why, &tab);
    CHECK(body != NULL); CHECK(!has_key(body, "securityReview"));
    json_free(body); free(why); cleanup(&f); json_free(empty);
    // A value this client does not list is refused rather than sent back blind.
    Json *unknown = json_parsez("{\"id\":4,\"repo\":\"o/r\",\"securityReview\":\"strict\"}");
    setup(&f, unknown, false);
    body = body_of(&f, &why, &tab);
    CHECK(body == NULL); CHECK(why != NULL); CHECK_INT(tab, T_REVIEW);
    free(why);
    CHECK(screen_settings_choose(f.form, F_SECURITY_REVIEW, 1));
    body = body_of(&f, &why, &tab);
    CHECK(body != NULL); CHECK_STR(json_str(json_get(body, "securityReview")), "advisory");
    json_free(body); free(why); cleanup(&f); json_free(unknown);
}
static void test_review_rules_absent_on_older_server(void) {
    Json *row = json_parsez("{\"id\":4,\"repo\":\"o/r\",\"prAuthor\":\"octocat\",\"autonomousReviewLoop\":false}");
    Form f; setup(&f, row, false);
    CHECK(!drawn(&f, ACT_CHOOSE, F_SECURITY_REVIEW * CHOICE_STRIDE));
    CHECK(!drawn(&f, ACT_TOGGLE, F_ENFORCE_OUTCOME)); CHECK(!drawn(&f, ACT_TOGGLE, F_BENCHMARK));
    CHECK(f.form->laid[F_REVIEW_AUTHOR]); CHECK(!f.form->laid[F_ROUND_LIMIT]);
    CHECK(!screen_settings_choose(f.form, F_SECURITY_REVIEW, 0));
    f.form->base.vt->action(&f.form->base, ACT_TOGGLE, F_BENCHMARK, (POINT){0});
    CHECK(!f.form->dirty);
    char *why; int tab; Json *body = body_of(&f, &why, &tab);
    CHECK(body != NULL);
    static const char *const keys[] = { "reviewRoundLimit", "securityReview", "enforceReviewOutcome", "reviewBenchmark" };
    for (size_t i = 0; i < sizeof keys / sizeof *keys; i++) CHECK(!has_key(body, keys[i]));
    json_free(body); free(why); cleanup(&f); json_free(row);
}

void app_project_settings_tests(void) {
    screen_fixture_prepare();
    test_run("project settings round trip current action defaults and PR author", test_current_action_defaults_and_pr_author);
    test_run("project review flag edit save refill and reopen", test_review_flag_edit_save_reopen);
    test_run("project create and clone send only offered settings, never removed ones", test_create_and_clone_send_only_offered_settings);
    test_run("project save failure keeps edits and discard preserves saved value", test_failure_and_discard);
    test_run("project save completion is cancelled on navigation", test_destroy_during_save);
    test_run("project review rules edit choose toggle and save", test_review_rules_edit_and_save);
    test_run("project review round limit takes 0 to 20 or empty", test_review_round_limit_bounds);
    test_run("project security review left out when empty and refused when unknown", test_security_review_empty_and_unknown);
    test_run("project review rules neither shown nor sent when the server lacks them", test_review_rules_absent_on_older_server);
}
