#include "screen_pulls_internal.h"
#include "screen_fixture.h"
#include "suites.h"

// A passed review whose high risk asks for two approvals: ana's (user 7) counts, leo's (8) was withdrawn, and a bot's on
// GitHub does not count. The rounds stay as read through every approval answer.
#define APPROVALS_READ "{\"codeReviews\":[{\"id\":20,\"status\":\"completed\",\"handlingStatus\":\"pending\",\"findings\":[]}]," \
    "\"reviewOutcome\":{\"outcome\":\"passed\",\"approvals\":{\"enabled\":true,\"result\":\"pending\",\"headSha\":\"abcdef1234567890\"," \
    "\"tier\":\"high\",\"required\":2,\"valid\":1,\"missing\":1,\"reviewCurrent\":true,\"explanation\":\"Needs two people.\"," \
    "\"risk\":{\"tier\":\"high\",\"reasons\":[{\"code\":\"sensitive_path\",\"tier\":\"high\",\"message\":\"migrations matched\"}]}," \
    "\"reasons\":[{\"code\":\"approvals_missing\",\"effect\":\"pending\",\"message\":\"One more approval\"}]," \
    "\"approvals\":[{\"id\":1,\"source\":\"briareus\",\"by\":{\"userId\":7,\"name\":\"ana\"},\"headSha\":\"abcdef1234567890\",\"counted\":true}," \
    "{\"id\":2,\"source\":\"github\",\"by\":{\"githubLogin\":\"bot\"},\"headSha\":\"abcdef1234567890\",\"counted\":false,\"message\":\"bot is a bot account\"}," \
    "{\"id\":3,\"source\":\"briareus\",\"by\":{\"userId\":8,\"name\":\"leo\"},\"headSha\":\"1111111\",\"withdrawnAt\":\"2026-11-12T11:00:00Z\"," \
    "\"withdrawNote\":\"pushed a fix\",\"counted\":false,\"reason\":\"withdrawn\"}]}}," \
    "\"reviewRounds\":{\"limit\":3,\"used\":2}}"
#define SATISFIED "\"approvals\":{\"enabled\":true,\"result\":\"satisfied\",\"headSha\":\"abcdef1234567890\",\"tier\":\"high\",\"required\":2," \
    "\"valid\":2,\"missing\":0,\"reviewCurrent\":true,\"approvals\":[{\"id\":1,\"source\":\"briareus\",\"by\":{\"userId\":7,\"name\":\"ana\"}," \
    "\"headSha\":\"abcdef1234567890\",\"counted\":true},{\"id\":4,\"source\":\"briareus\",\"by\":{\"userId\":9,\"name\":\"eve\"}," \
    "\"headSha\":\"abcdef1234567890\",\"counted\":true}]}"

static Route routes[] = {
    {"GET", "/pulls/{number}/code-reviews", "read"},
    {"PATCH", "/pulls/{number}/code-reviews/{reviewId}", "manage"},
    {"POST", "/pulls/{number}/approvals", "manage"},
    {"POST", "/pulls/{number}/approvals/{approvalId}/withdraw", "manage"},
};
static PullScreen *setup(ScreenFixture *f, char *permission, size_t route_count, int64_t me) {
    screen_fixture_init(f, routes, route_count); g_store.device.permission = permission; g_store.device.id = "device";
    g_store.device.user_id = me;
    Project p = {.repo = "o/r"}; PullScreen *s = (PullScreen *)pull_detail_screen_new(&p, 12, NULL, NULL);
    screen_fixture_pane(f, &s->base);
    s->tab = PR_TAB_CODE_REVIEWS; s->shown = true; s->reviews.visible = true;
    f->read_response = APPROVALS_READ;
    screen_pulls_reviews_load(s, false); screen_fixture_pump(f, 1);
    return s;
}
static void cleanup(ScreenFixture *f, PullScreen *s) { s->base.vt->destroy(&s->base); screen_fixture_clear(f); }
static void draw(PullScreen *s, Doc *d) { doc_begin(d, NULL, 1200); s->base.vt->layout(&s->base, d); doc_end(d); }
static int find(Doc *d, int action, intptr_t arg) {
    for (size_t i = 0; i < d->count; i++) if (d->items[i].action == action && d->items[i].arg == arg) return (int)i;
    return -1;
}
static bool contains(Doc *d, const char *text) {
    for (size_t i = 0; i < d->count; i++) if (d->items[i].text && strstr(d->items[i].text, text)) return true;
    return false;
}
static void act(PullScreen *s, int which, intptr_t arg) { s->base.vt->action(&s->base, ACT_APPROVAL_BASE + which, arg, (POINT){0}); }
// Types the note into the box the pane placed over the open form.
static void type_note(ScreenFixture *f, PullScreen *s, Doc *d, const wchar_t *text) {
    draw(s, d);
    RECT content = pane_content_rect(f->pane); s->base.vt->place(&s->base, &content, 0);
    HWND box = s->reviews.approvals.box; CHECK(box != NULL);
    if (!box) return;
    SetWindowTextW(box, text); s->base.vt->command(&s->base, 0, EN_CHANGE, box);
}

static void every_approval_on_record_shows_with_approve_and_withdraw(void) {
    ScreenFixture f; PullScreen *s = setup(&f, "manage", 4, 7);
    Doc d; doc_init(&d); draw(s, &d);
    CHECK(contains(&d, "1 of 2 approvals")); CHECK(contains(&d, "Needs two people.")); CHECK(contains(&d, "One more approval"));
    CHECK(contains(&d, "ana")); CHECK(contains(&d, "bot is a bot account")); CHECK(contains(&d, "Withdrawn: pushed a fix"));
    // ana's approval already counts for this head, so only Withdraw on hers is offered.
    CHECK_INT(find(&d, ACT_APPROVAL_BASE + APPROVAL_ACT_APPROVE, 0), -1);
    CHECK(find(&d, ACT_APPROVAL_BASE + APPROVAL_ACT_WITHDRAW, 1) >= 0);
    CHECK_INT(find(&d, ACT_APPROVAL_BASE + APPROVAL_ACT_WITHDRAW, 2), -1); CHECK_INT(find(&d, ACT_APPROVAL_BASE + APPROVAL_ACT_WITHDRAW, 3), -1);
    // What set the risk is folded until opened.
    CHECK(!contains(&d, "migrations matched")); act(s, APPROVAL_ACT_RISK, 0); draw(s, &d); CHECK(contains(&d, "migrations matched"));
    doc_free(&d); cleanup(&f, s);
    // Another person may approve, and withdraws none of these.
    s = setup(&f, "manage", 4, 9); doc_init(&d); draw(s, &d);
    CHECK(find(&d, ACT_APPROVAL_BASE + APPROVAL_ACT_APPROVE, 0) >= 0); CHECK_INT(find(&d, ACT_APPROVAL_BASE + APPROVAL_ACT_WITHDRAW, 1), -1);
    doc_free(&d); cleanup(&f, s);
    // A read token, or a server without the routes, sees the approvals and changes none.
    s = setup(&f, "read", 4, 9); doc_init(&d); draw(s, &d);
    CHECK(contains(&d, "1 of 2 approvals")); CHECK_INT(find(&d, ACT_APPROVAL_BASE + APPROVAL_ACT_APPROVE, 0), -1);
    doc_free(&d); cleanup(&f, s);
    s = setup(&f, "manage", 2, 7); doc_init(&d); draw(s, &d);
    CHECK_INT(find(&d, ACT_APPROVAL_BASE + APPROVAL_ACT_APPROVE, 0), -1); CHECK_INT(find(&d, ACT_APPROVAL_BASE + APPROVAL_ACT_WITHDRAW, 1), -1);
    doc_free(&d); cleanup(&f, s);
}
static void approving_sends_the_reviewed_head_and_takes_the_answer(void) {
    ScreenFixture f; PullScreen *s = setup(&f, "manage", 4, 9);
    Doc d; doc_init(&d);
    act(s, APPROVAL_ACT_APPROVE, 0); type_note(&f, s, &d, L"  Read it all  ");
    CHECK(contains(&d, "abcdef1")); CHECK(find(&d, ACT_APPROVAL_BASE + APPROVAL_ACT_SEND, 0) >= 0);
    f.write_status = 201;
    f.write_response = "{\"approval\":{\"id\":4},\"reviewOutcome\":{\"outcome\":\"passed\",\"ready\":true," SATISFIED "}}";
    f.block_write = true; act(s, APPROVAL_ACT_SEND, 0);
    CHECK_INT(WaitForSingleObject(f.entered, 5000), WAIT_OBJECT_0);
    // While it is saved, a second click, a poll and a handling save all wait for it.
    act(s, APPROVAL_ACT_SEND, 0); screen_pulls_reviews_load(s, true); screen_pulls_review_set_handled(s, 20);
    draw(s, &d); CHECK(contains(&d, "Saving"));
    SetEvent(f.resume); screen_fixture_pump(&f, 1);
    CHECK_INT(f.writes, 1); CHECK_INT(f.reads, 1);
    CHECK_STR(f.method, "POST"); CHECK(strstr(f.url, "/api/v1/pulls/12/approvals") != NULL);
    CHECK_STR(json_str(json_get(f.body, "headSha")), "abcdef1234567890"); CHECK_STR(json_str(json_get(f.body, "note")), "Read it all");
    CHECK_STR(json_str(json_get(f.body, "repo")), "o/r");
    // The answer's outcome replaces the kept one, the rounds stay, and the form closes.
    const ReviewOutcome *o = s->reviews.outcome; CHECK(o != NULL);
    if (o) { CHECK(o->ready); CHECK_INT(o->rounds.used, 2); }
    CHECK(s->reviews.approvals.open == 0); draw(s, &d);
    CHECK(contains(&d, "2 of 2 approvals")); CHECK(contains(&d, "eve")); CHECK_INT(find(&d, ACT_APPROVAL_BASE + APPROVAL_ACT_APPROVE, 0), -1);
    doc_free(&d); cleanup(&f, s);
}
static void withdrawing_names_the_approval_and_cancel_sends_nothing(void) {
    ScreenFixture f; PullScreen *s = setup(&f, "manage", 4, 7);
    Doc d; doc_init(&d);
    act(s, APPROVAL_ACT_WITHDRAW, 1); draw(s, &d); CHECK(s->reviews.approvals.open == 1);
    act(s, APPROVAL_ACT_CANCEL, 0); CHECK(s->reviews.approvals.open == 0); CHECK_INT(f.writes, 0);
    // A note typed and cancelled is gone from the box when the form opens again, and is not sent.
    act(s, APPROVAL_ACT_WITHDRAW, 1); type_note(&f, s, &d, L"first thought"); act(s, APPROVAL_ACT_CANCEL, 0);
    act(s, APPROVAL_ACT_WITHDRAW, 1); draw(s, &d);
    RECT content = pane_content_rect(f.pane); s->base.vt->place(&s->base, &content, 0);
    CHECK(s->reviews.approvals.box != NULL); CHECK_INT(GetWindowTextLengthW(s->reviews.approvals.box), 0); CHECK(s->reviews.approvals.note == NULL);
    type_note(&f, s, &d, L"pushed again");
    // An answer carrying the requirement alone puts it in the kept outcome.
    f.write_response = "{\"approval\":{\"id\":1,\"counted\":false}," SATISFIED "}";
    act(s, APPROVAL_ACT_SEND, 0); screen_fixture_pump(&f, 1);
    CHECK_INT(f.writes, 1); CHECK(strstr(f.url, "/api/v1/pulls/12/approvals/1/withdraw") != NULL);
    CHECK_STR(json_str(json_get(f.body, "note")), "pushed again"); CHECK(json_is_null(json_get(f.body, "approvalId")));
    const ReviewOutcome *o = s->reviews.outcome; CHECK(o != NULL);
    if (o) { CHECK_STR(o->approvals.result, "satisfied"); CHECK_INT(o->outcome, REVIEW_OUTCOME_PASSED); CHECK_INT(o->rounds.limit, 3); }
    doc_free(&d); cleanup(&f, s);
}
static void a_refusal_shows_and_a_moved_head_reads_the_outcome_again(void) {
    ScreenFixture f; PullScreen *s = setup(&f, "manage", 4, 9);
    Doc d; doc_init(&d);
    // A refusal for another reason stays on the form, which stays open, and nothing is read again.
    act(s, APPROVAL_ACT_APPROVE, 0);
    f.write_status = 403; f.write_response = "{\"error\":\"You wrote this change.\",\"code\":\"own_change\"}";
    act(s, APPROVAL_ACT_SEND, 0); screen_fixture_pump(&f, 1);
    CHECK_INT(f.reads, 1); CHECK(s->reviews.approvals.open == APPROVAL_FORM_APPROVE); draw(s, &d); CHECK(contains(&d, "You wrote this change."));
    // A moved head reads the outcome again, so Approve follows the server's head; the refusal stays shown.
    f.write_status = 409; f.write_response = "{\"error\":\"The pull request moved on.\",\"code\":\"head_moved\",\"headSha\":\"fff\"}";
    act(s, APPROVAL_ACT_SEND, 0); screen_fixture_pump(&f, 2);
    CHECK_INT(f.writes, 2); CHECK_INT(f.reads, 2); draw(s, &d); CHECK(contains(&d, "The pull request moved on."));
    // A write whose outcome is unknown says so, and is not sent again by itself.
    f.fail_write = true; act(s, APPROVAL_ACT_SEND, 0); screen_fixture_pump(&f, 1);
    CHECK_INT(f.writes, 3); draw(s, &d); CHECK(contains(&d, "may still have been saved"));
    doc_free(&d); cleanup(&f, s);
}
void app_pull_review_approvals_tests(void) {
    test_run("the approvals card lists every approval with Approve and Withdraw for those who may", every_approval_on_record_shows_with_approve_and_withdraw);
    test_run("approving sends the reviewed head and a trimmed note, waits alone, and takes the answer", approving_sends_the_reviewed_head_and_takes_the_answer);
    test_run("withdrawing names the approval, cancel sends nothing, and a requirement alone is folded in", withdrawing_names_the_approval_and_cancel_sends_nothing);
    test_run("an approval refusal shows, and a moved head reads the outcome again", a_refusal_shows_and_a_moved_head_reads_the_outcome_again);
}
