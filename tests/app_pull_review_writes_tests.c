#include "screen_pulls_internal.h"
#include "screen_fixture.h"
#include "suites.h"

// A stopped review (20) with one dismissed finding, and a running one (21); both track handling.
#define HANDLED_REVIEWS "{\"codeReviews\":[" \
    "{\"id\":21,\"status\":\"running\",\"handlingStatus\":\"pending\",\"findings\":[{\"id\":101,\"severity\":\"high\",\"title\":\"Still verifying\"}]}," \
    "{\"id\":20,\"status\":\"completed\",\"handlingStatus\":\"pending\",\"findings\":[{\"id\":99,\"severity\":\"low\",\"title\":\"Historical duplicate\"," \
    "\"shouldFix\":\"dismiss\",\"verification\":\"plausible\",\"comments\":\"Dismissed by verification.\"}]}]}"

static Route routes[] = {
    {"GET", "/pulls/{number}/code-reviews", "read"},
    {"PATCH", "/pulls/{number}/code-reviews/{reviewId}", "manage"},
    {"PATCH", "/pulls/{number}/code-reviews/{reviewId}/findings/{findingId}", "manage"},
};
static PullScreen *setup(ScreenFixture *f, char *permission, size_t route_count, const char *reviews) {
    screen_fixture_init(f, routes, route_count); g_store.device.permission = permission; g_store.device.id = "device";
    Project p = {.repo = "o/r"}; PullScreen *s = (PullScreen *)pull_detail_screen_new(&p, 12, NULL, NULL);
    screen_fixture_pane(f, &s->base);
    s->tab = PR_TAB_CODE_REVIEWS; s->shown = true; s->reviews.visible = true;
    f->read_response = reviews;
    screen_pulls_reviews_load(s, false); screen_fixture_pump(f, 1);
    return s;
}
static void cleanup(ScreenFixture *f, PullScreen *s) { s->base.vt->destroy(&s->base); screen_fixture_clear(f); }
static void draw(PullScreen *s, Doc *d) { doc_begin(d, NULL, 1200); s->base.vt->layout(&s->base, d); doc_end(d); }
static int find(Doc *d, int action, intptr_t arg) {
    for (size_t i = 0; i < d->count; i++) if (d->items[i].action == action && d->items[i].arg == arg) return (int)i;
    return -1;
}
static bool has_text(Doc *d, const char *text) {
    for (size_t i = 0; i < d->count; i++) if (str_eq(d->items[i].text, text)) return true;
    return false;
}
static bool contains(Doc *d, const char *text) {
    for (size_t i = 0; i < d->count; i++) if (d->items[i].text && strstr(d->items[i].text, text)) return true;
    return false;
}
static void act(PullScreen *s, int action, intptr_t arg) { s->base.vt->action(&s->base, action, arg, (POINT){0}); }
// Types into the editor's reason box as a person would, after the pane placed it.
static void type_reason(ScreenFixture *f, PullScreen *s, Doc *d, const wchar_t *text) {
    draw(s, d);
    RECT content = pane_content_rect(f->pane); s->base.vt->place(&s->base, &content, 0);
    HWND box = s->reviews.reason; CHECK(box != NULL);
    if (!box) return;
    SetWindowTextW(box, text); s->base.vt->command(&s->base, 0, EN_CHANGE, box);
}

static void a_stopped_review_is_marked_handled_and_reopened(void) {
    ScreenFixture f; PullScreen *s = setup(&f, "manage", 3, HANDLED_REVIEWS);
    Doc d; doc_init(&d);
    // A running review offers neither handling nor verdict changes.
    act(s, ACT_REVIEW_ROW, 21); draw(s, &d);
    CHECK_INT(find(&d, ACT_REVIEW_HANDLING, 21), -1); CHECK_INT(find(&d, ACT_REVIEW_EDIT, 101), -1); CHECK(!has_text(&d, "Handling: Pending"));
    act(s, ACT_REVIEW_BACK, 0); act(s, ACT_REVIEW_ROW, 20); draw(s, &d);
    CHECK(has_text(&d, "Handling: Pending")); CHECK(find(&d, ACT_REVIEW_HANDLING, 20) >= 0); CHECK(has_text(&d, "Mark handled"));
    f.write_response = "{\"review\":{\"id\":20,\"status\":\"completed\",\"handlingStatus\":\"handled\",\"findings\":[{\"id\":99,\"severity\":\"low\",\"title\":\"Historical duplicate\"}]}}";
    f.block_write = true; act(s, ACT_REVIEW_HANDLING, 20);
    CHECK_INT(WaitForSingleObject(f.entered, 5000), WAIT_OBJECT_0);
    // While it is saved, a second click and a poll both wait for it.
    draw(s, &d); CHECK(has_text(&d, "Saving\xE2\x80\xA6")); CHECK_INT(find(&d, ACT_REVIEW_HANDLING, 20), -1);
    act(s, ACT_REVIEW_HANDLING, 20); screen_pulls_reviews_load(s, true);
    SetEvent(f.resume); screen_fixture_pump(&f, 1);
    CHECK_INT(f.writes, 1); CHECK_INT(f.reads, 1);
    CHECK_STR(f.method, "PATCH"); CHECK_STR(f.url, "https://example.com/api/v1/pulls/12/code-reviews/20");
    CHECK_INT(json_count(f.body), 2); CHECK_STR(json_str(json_get(f.body, "repo")), "o/r"); CHECK_STR(json_str(json_get(f.body, "handlingStatus")), "handled");
    // The answer is folded in: the review reads handled and offers Reopen, without another read.
    CHECK_STR(code_review_handling(code_review_find(s->reviews.items, 20)), "handled");
    draw(s, &d); CHECK(has_text(&d, "Handling: Handled")); CHECK(has_text(&d, "Reopen"));
    act(s, ACT_REVIEW_BACK, 0); draw(s, &d); CHECK(has_text(&d, "completed\nHandled"));
    // A refusal is said as it is; an unknown outcome may still have been saved.
    f.block_write = false; f.write_status = 409; f.write_response = "{\"error\":\"Review changed\"}";
    act(s, ACT_REVIEW_HANDLING, 20); screen_fixture_pump(&f, 1);
    CHECK_STR(json_str(json_get(f.body, "handlingStatus")), "pending"); CHECK(s->reviews.save_error && strstr(s->reviews.save_error, "Review changed"));
    CHECK_STR(code_review_handling(code_review_find(s->reviews.items, 20)), "handled");
    draw(s, &d); CHECK(contains(&d, "Review changed"));
    f.fail_write = true; act(s, ACT_REVIEW_HANDLING, 20); screen_fixture_pump(&f, 1);
    CHECK(s->reviews.save_error && strstr(s->reviews.save_error, "may still have been saved"));
    CHECK_INT(f.writes, 3); CHECK_INT(f.reads, 1);
    // A fresh read clears the stale error.
    screen_pulls_reviews_load(s, true); screen_fixture_pump(&f, 1); CHECK(s->reviews.save_error == NULL);
    doc_free(&d); cleanup(&f, s);
}
static void write_controls_follow_permission_routes_and_fields(void) {
    const char *without_field = "{\"codeReviews\":[{\"id\":20,\"status\":\"completed\",\"findings\":[{\"id\":99,\"severity\":\"low\",\"title\":\"T\"}]}]}";
    struct { char *permission; size_t routes; const char *reviews; bool tracked, handle, edit; } cases[] = {
        {"manage", 3, HANDLED_REVIEWS, true, true, true},
        {"read", 3, HANDLED_REVIEWS, true, false, false},   // a read token may not write
        {"manage", 1, HANDLED_REVIEWS, true, false, false},  // an older server without the routes
        {"manage", 3, without_field, false, false, true},    // a server that does not track handling
    };
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        ScreenFixture f; PullScreen *s = setup(&f, cases[i].permission, cases[i].routes, cases[i].reviews);
        f.write_response = "{\"review\":{\"id\":20,\"status\":\"completed\",\"handlingStatus\":\"handled\",\"findings\":[]}}";
        Doc d; doc_init(&d);
        act(s, ACT_REVIEW_ROW, 20); draw(s, &d);
        CHECK((find(&d, ACT_REVIEW_HANDLING, 20) >= 0) == cases[i].handle);
        CHECK((find(&d, ACT_REVIEW_EDIT, 99) >= 0) == cases[i].edit);
        CHECK(has_text(&d, "Handling: Pending") == cases[i].tracked);
        // An action the screen does not offer sends nothing.
        act(s, ACT_REVIEW_HANDLING, 20); act(s, ACT_REVIEW_EDIT, 99); act(s, ACT_REVIEW_SAVE, 0);
        if (cases[i].handle) screen_fixture_pump(&f, 1);
        CHECK_INT(f.writes, cases[i].handle ? 1 : 0);
        // A handling write under way holds the editor back.
        CHECK((s->reviews.editing != NULL) == (cases[i].edit && !cases[i].handle));
        doc_free(&d); cleanup(&f, s);
    }
}
static void a_verdict_is_overruled_with_its_evidence_and_reason(void) {
    ScreenFixture f; PullScreen *s = setup(&f, "manage", 3, HANDLED_REVIEWS);
    Doc d; doc_init(&d);
    act(s, ACT_REVIEW_ROW, 20); act(s, ACT_REVIEW_EDIT, 99);
    CHECK(s->reviews.editing != NULL); draw(s, &d);
    CHECK(has_text(&d, "Save")); CHECK_INT(find(&d, ACT_REVIEW_SAVE, 0), -1); CHECK(find(&d, ACT_REVIEW_CANCEL, 0) >= 0);
    // A stored dismissal that is only plausible may be reworded as it stands.
    type_reason(&f, s, &d, L"Refuted by the guard\r\non line 4.");
    CHECK_STR(s->reviews.editing->reason, "Refuted by the guard\non line 4.");
    draw(s, &d); CHECK(find(&d, ACT_REVIEW_SAVE, 0) >= 0);
    // Optional with the same old reason is held back until it has one of its own.
    act(s, ACT_REVIEW_VERDICT, 1); type_reason(&f, s, &d, L"Dismissed by verification.");
    draw(s, &d); CHECK_INT(find(&d, ACT_REVIEW_SAVE, 0), -1);
    act(s, ACT_REVIEW_SAVE, 0); CHECK_INT(f.writes, 0);
    type_reason(&f, s, &d, L"Not worth fixing here.");
    // Dismissed needs Confirmed evidence.
    act(s, ACT_REVIEW_VERDICT, 2); act(s, ACT_REVIEW_EVIDENCE, 0); draw(s, &d); CHECK(find(&d, ACT_REVIEW_SAVE, 0) >= 0);
    act(s, ACT_REVIEW_VERDICT, 1); draw(s, &d); CHECK(find(&d, ACT_REVIEW_SAVE, 0) >= 0);
    // A refusal keeps the editor open with the reason; nothing is retried.
    f.write_status = 400; f.write_response = "{\"error\":\"Unknown verdict\"}";
    act(s, ACT_REVIEW_SAVE, 0); screen_fixture_pump(&f, 1);
    CHECK_INT(f.writes, 1); CHECK(s->reviews.editing != NULL); CHECK(s->reviews.edit_error && strstr(s->reviews.edit_error, "Unknown verdict"));
    CHECK_STR(f.url, "https://example.com/api/v1/pulls/12/code-reviews/20/findings/99");
    CHECK_INT(json_count(f.body), 4); CHECK_STR(json_str(json_get(f.body, "shouldFix")), "optional");
    CHECK_STR(json_str(json_get(f.body, "verification")), "confirmed"); CHECK_STR(json_str(json_get(f.body, "comments")), "Not worth fixing here.");
    draw(s, &d); CHECK(contains(&d, "Unknown verdict"));
    f.write_status = 200;
    f.write_response = "{\"finding\":{\"id\":99,\"severity\":\"low\",\"title\":\"Historical duplicate\",\"shouldFix\":\"optional\",\"verification\":\"confirmed\",\"comments\":\"Not worth fixing here.\"}}";
    act(s, ACT_REVIEW_SAVE, 0); screen_fixture_pump(&f, 1);
    CHECK_INT(f.writes, 2); CHECK_INT(f.reads, 1); CHECK(s->reviews.editing == NULL); CHECK(s->reviews.edit_error == NULL);
    const Json *saved = code_review_finding(code_review_find(s->reviews.items, 20), 99);
    CHECK_STR(json_str(json_get(saved, "shouldFix")), "optional"); CHECK_STR(json_str(json_get(saved, "verification")), "confirmed");
    draw(s, &d); CHECK_INT(find(&d, ACT_REVIEW_SAVE, 0), -1); CHECK(!has_text(&d, "Cancel"));
    RECT content = pane_content_rect(f.pane); s->base.vt->place(&s->base, &content, 0);
    CHECK(s->reviews.reason && !IsWindowVisible(s->reviews.reason));
    doc_free(&d); cleanup(&f, s);
}
static void an_editor_waits_out_a_rerun_and_cancels(void) {
    ScreenFixture f; PullScreen *s = setup(&f, "manage", 3, HANDLED_REVIEWS);
    Doc d; doc_init(&d);
    act(s, ACT_REVIEW_ROW, 20); act(s, ACT_REVIEW_EDIT, 99); act(s, ACT_REVIEW_EVIDENCE, 0);
    draw(s, &d); CHECK(find(&d, ACT_REVIEW_SAVE, 0) >= 0);
    // The review runs again: its findings are being rewritten, so Save waits and Change verdict goes.
    f.read_response = "{\"codeReviews\":[{\"id\":20,\"status\":\"running\",\"handlingStatus\":\"pending\",\"findings\":[{\"id\":99,\"severity\":\"low\",\"title\":\"T\",\"shouldFix\":\"dismiss\"}]}]}";
    screen_pulls_reviews_load(s, true); screen_fixture_pump(&f, 1);
    draw(s, &d); CHECK_INT(find(&d, ACT_REVIEW_SAVE, 0), -1); CHECK_INT(find(&d, ACT_REVIEW_EDIT, 99), -1);
    act(s, ACT_REVIEW_SAVE, 0); CHECK_INT(f.writes, 0);
    act(s, ACT_REVIEW_CANCEL, 0); CHECK(s->reviews.editing == NULL);
    draw(s, &d); CHECK(!has_text(&d, "Cancel"));
    doc_free(&d); cleanup(&f, s);
}
static void a_held_write_is_cancelled_with_the_screen(void) {
    ScreenFixture f; PullScreen *s = setup(&f, "manage", 3, HANDLED_REVIEWS);
    act(s, ACT_REVIEW_ROW, 20); f.block_write = true; act(s, ACT_REVIEW_HANDLING, 20);
    CHECK_INT(WaitForSingleObject(f.entered, 5000), WAIT_OBJECT_0);
    s->base.vt->destroy(&s->base); SetEvent(f.resume); screen_fixture_pump(&f, 1);
    CHECK_INT(f.writes, 1); screen_fixture_clear(&f);
}
void app_pull_review_writes_tests(void) {
    test_run("a stopped code review is marked handled and reopened, its answer folded in and never retried", a_stopped_review_is_marked_handled_and_reopened);
    test_run("handling and verdict controls follow permission, server routes and the handling field", write_controls_follow_permission_routes_and_fields);
    test_run("a finding's verdict is overruled with the evidence and reason the review outcome asks", a_verdict_is_overruled_with_its_evidence_and_reason);
    test_run("the verdict editor waits out a rerun of its review and cancels", an_editor_waits_out_a_rerun_and_cancels);
    test_run("a held code review write is cancelled with the screen", a_held_write_is_cancelled_with_the_screen);
}
