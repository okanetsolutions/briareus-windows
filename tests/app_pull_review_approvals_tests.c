#include "screen_pulls_internal.h"
#include "screen_fixture.h"
#include "suites.h"

// A passed review whose high-risk change waits on one more person: user 7's earlier approval is of another head, user
// 8's counts, and one read from GitHub does not.
#define APPROVALS(result, valid, list) "{\"enabled\":true,\"result\":\"" result "\",\"headSha\":\"abcdef1234567\",\"tier\":\"high\",\"required\":2," \
    "\"valid\":" valid ",\"reviewCurrent\":true,\"explanation\":\"High risk needs two people.\"," \
    "\"risk\":{\"reasons\":[{\"code\":\"sensitive_path\",\"tier\":\"high\",\"message\":\"migrations/** matched 2 files\"}]}," \
    "\"reasons\":[{\"code\":\"approvals_missing\",\"effect\":\"pending\",\"message\":\"One more approval is missing.\"}],\"approvals\":[" list "]}"
#define MINE_OLD "{\"id\":2,\"source\":\"briareus\",\"by\":{\"userId\":7,\"name\":\"Ada\"},\"headSha\":\"0000000aaaa\",\"counted\":false,\"reason\":\"head_moved\",\"message\":\"Ada approved another head\"}"
#define THEIRS "{\"id\":3,\"source\":\"briareus\",\"by\":{\"userId\":8,\"name\":\"Lin\"},\"headSha\":\"abcdef1234567\",\"counted\":true,\"note\":\"Read the migration\"}"
#define GITHUB "{\"id\":4,\"source\":\"github\",\"by\":{\"githubLogin\":\"octo\"},\"headSha\":\"abcdef1234567\",\"counted\":false,\"reason\":\"github_not_accepted\"}"
#define MINE_NOW "{\"id\":5,\"source\":\"briareus\",\"by\":{\"userId\":7,\"name\":\"Ada\"},\"headSha\":\"abcdef1234567\",\"counted\":true,\"note\":\"Looked\"}"
#define READ "{\"codeReviews\":[{\"id\":20,\"status\":\"completed\",\"findings\":[]}],\"reviewOutcome\":{\"outcome\":\"passed\"," \
    "\"approvals\":" APPROVALS("pending", "1", MINE_OLD "," THEIRS "," GITHUB) "},\"reviewRounds\":{\"used\":1,\"limit\":3}}"

static Route routes[] = {
    {"GET", "/pulls/{number}/code-reviews", "read"},
    {"POST", "/pulls/{number}/approvals", "manage"},
    {"POST", "/pulls/{number}/approvals/{approvalId}/withdraw", "manage"},
};
static PullScreen *setup(ScreenFixture *f, char *permission, size_t route_count, long long user) {
    screen_fixture_init(f, routes, route_count); g_store.device.permission = permission; g_store.device.id = "device";
    g_store.device.user_id = user;
    Project p = {.repo = "o/r"}; PullScreen *s = (PullScreen *)pull_detail_screen_new(&p, 12, NULL, NULL);
    screen_fixture_pane(f, &s->base);
    s->tab = PR_TAB_CODE_REVIEWS; s->shown = true; s->reviews.visible = true;
    f->read_response = READ;
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
// The approvals card's actions on the Code reviews tab.
enum { APPROVE = ACT_REVIEW_APPROVALS + APPROVAL_ACT_APPROVE, WITHDRAW = ACT_REVIEW_APPROVALS + APPROVAL_ACT_WITHDRAW,
    SEND = ACT_REVIEW_APPROVALS + APPROVAL_ACT_SEND, CANCEL = ACT_REVIEW_APPROVALS + APPROVAL_ACT_CANCEL, RISK = ACT_REVIEW_APPROVALS + APPROVAL_ACT_RISK };
static void act(PullScreen *s, int action, intptr_t arg) { s->base.vt->action(&s->base, action, arg, (POINT){0}); }
// Types the note as a person would, after the pane placed its box.
static void type_note(ScreenFixture *f, PullScreen *s, Doc *d, const wchar_t *text) {
    draw(s, d);
    RECT content = pane_content_rect(f->pane); s->base.vt->place(&s->base, &content, 0);
    HWND box = s->reviews.approvals.box; CHECK(box != NULL);
    if (box) { CHECK(IsWindowVisible(box)); SetWindowTextW(box, text); }
}

static void a_person_approves_the_reviewed_head_with_a_note(void) {
    ScreenFixture f; PullScreen *s = setup(&f, "manage", 3, 7);
    Doc d; doc_init(&d); draw(s, &d);
    // The block says what is asked, why, every approval on record, and offers Approve; the risk is folded.
    CHECK(contains(&d, "1 of 2 approvals \xC2\xB7 high risk")); CHECK(contains(&d, "High risk needs two people."));
    CHECK(contains(&d, "One more approval is missing.")); CHECK(contains(&d, "Ada \xC2\xB7 Ada approved another head"));
    CHECK(contains(&d, "Lin \xC2\xB7 Counts")); CHECK(contains(&d, "Read the migration")); CHECK(contains(&d, "@octo \xC2\xB7 github not accepted"));
    CHECK(contains(&d, "on GitHub \xC2\xB7 abcdef1")); CHECK(!contains(&d, "migrations/** matched"));
    act(s, RISK, 0); draw(s, &d); CHECK(contains(&d, "high \xC2\xB7 migrations/** matched 2 files"));
    CHECK(find(&d, APPROVE, 0) >= 0);
    // Withdraw is offered on one's own approval recorded here only.
    CHECK(find(&d, WITHDRAW, 2) >= 0); CHECK_INT(find(&d, WITHDRAW, 3), -1); CHECK_INT(find(&d, WITHDRAW, 4), -1);
    act(s, APPROVE, 0); draw(s, &d);
    CHECK(contains(&d, "counts for abcdef1 only")); CHECK(find(&d, SEND, 0) >= 0); CHECK_INT(find(&d, APPROVE, 0), -1);
    type_note(&f, s, &d, L"  Checked the migration\r\nand rollback  ");
    f.write_response = "{\"approval\":{\"id\":5},\"reviewOutcome\":{\"outcome\":\"passed\",\"ready\":true,\"approvals\":"
        APPROVALS("satisfied", "2", MINE_OLD "," THEIRS "," GITHUB "," MINE_NOW) "}}";
    f.block_write = true; act(s, SEND, 0);
    CHECK_INT(WaitForSingleObject(f.entered, 5000), WAIT_OBJECT_0);
    // While it is saved, a second send, a withdrawal and a poll all wait for it.
    draw(s, &d); CHECK(contains(&d, "Saving\xE2\x80\xA6"));
    act(s, SEND, 0); act(s, WITHDRAW, 2); screen_pulls_reviews_load(s, true);
    SetEvent(f.resume); screen_fixture_pump(&f, 1);
    CHECK_INT(f.writes, 1); CHECK_INT(f.reads, 1);
    CHECK_STR(f.method, "POST"); CHECK_STR(f.url, "https://example.com/api/v1/pulls/12/approvals");
    CHECK_INT(json_count(f.body), 3); CHECK_STR(json_str(json_get(f.body, "repo")), "o/r");
    CHECK_STR(json_str(json_get(f.body, "headSha")), "abcdef1234567"); CHECK_STR(json_str(json_get(f.body, "note")), "Checked the migration\nand rollback");
    // The answer's outcome replaces the kept one and keeps the rounds; the form closes and Approve is no longer offered.
    const ReviewOutcome *o = s->reviews.outcome; CHECK(o != NULL);
    if (o) { CHECK(o->ready); CHECK_INT(o->rounds.used, 1); }
    CHECK_INT(s->reviews.approvals.open, 0);
    draw(s, &d); CHECK(contains(&d, "2 of 2 approvals")); CHECK_INT(find(&d, APPROVE, 0), -1); CHECK_INT(find(&d, SEND, 0), -1);
    CHECK(find(&d, WITHDRAW, 5) >= 0);
    doc_free(&d); cleanup(&f, s);
}
static void a_person_withdraws_their_own_approval_and_refusals_say_why(void) {
    ScreenFixture f; PullScreen *s = setup(&f, "manage", 3, 7);
    Doc d; doc_init(&d);
    act(s, WITHDRAW, 3); CHECK_INT(s->reviews.approvals.open, 0);   // another person's
    act(s, WITHDRAW, 2); draw(s, &d);
    CHECK(contains(&d, "no longer counts")); CHECK(find(&d, SEND, 0) >= 0);
    // Without a note, only the repository is sent; a refusal is said in the block and nothing is retried.
    f.write_status = 403; f.write_response = "{\"error\":\"Only the approver withdraws it\",\"code\":\"not_yours\"}";
    act(s, SEND, 0); screen_fixture_pump(&f, 1);
    CHECK_STR(f.url, "https://example.com/api/v1/pulls/12/approvals/2/withdraw"); CHECK_INT(json_count(f.body), 1);
    CHECK(s->reviews.approvals.error && strstr(s->reviews.approvals.error, "Only the approver"));
    CHECK_INT(s->reviews.approvals.open, 2); CHECK_INT(f.writes, 1); CHECK_INT(f.reads, 1);
    draw(s, &d); CHECK(contains(&d, "Only the approver"));
    // A head that moved on reads the reviews again, so Approve follows the server's head.
    act(s, CANCEL, 0); CHECK_INT(s->reviews.approvals.open, 0); CHECK(s->reviews.approvals.error == NULL);
    f.write_status = 409; f.write_response = "{\"error\":\"The pull request moved on\",\"code\":\"head_moved\",\"headSha\":\"fff\"}";
    act(s, APPROVE, 0); act(s, SEND, 0); screen_fixture_pump(&f, 2);
    CHECK_INT(f.writes, 2); CHECK_INT(f.reads, 2); CHECK(s->reviews.approvals.error && strstr(s->reviews.approvals.error, "moved on"));
    // An answer that never came may still have been saved.
    f.fail_write = true; act(s, SEND, 0); screen_fixture_pump(&f, 1);
    CHECK(s->reviews.approvals.error && strstr(s->reviews.approvals.error, "may still have been saved")); CHECK_INT(f.writes, 3);
    doc_free(&d); cleanup(&f, s);
}
static void approvals_follow_the_token_and_the_routes(void) {
    // A read token sees the block without Approve or Withdraw.
    ScreenFixture f; PullScreen *s = setup(&f, "read", 3, 7);
    Doc d; doc_init(&d); draw(s, &d);
    CHECK(contains(&d, "1 of 2 approvals")); CHECK_INT(find(&d, APPROVE, 0), -1); CHECK_INT(find(&d, WITHDRAW, 2), -1);
    act(s, APPROVE, 0); CHECK_INT(s->reviews.approvals.open, 0);
    doc_free(&d); cleanup(&f, s);
    // A server without the routes offers neither; one that does not say who the token is offers every Withdraw here.
    s = setup(&f, "manage", 1, 0); doc_init(&d); draw(s, &d);
    CHECK_INT(find(&d, APPROVE, 0), -1); CHECK_INT(find(&d, WITHDRAW, 2), -1);
    doc_free(&d); cleanup(&f, s);
    s = setup(&f, "manage", 3, 0); doc_init(&d); draw(s, &d);
    CHECK(find(&d, APPROVE, 0) >= 0); CHECK(find(&d, WITHDRAW, 2) >= 0); CHECK(find(&d, WITHDRAW, 3) >= 0);
    CHECK_INT(find(&d, WITHDRAW, 4), -1);
    doc_free(&d); cleanup(&f, s);
}
void app_pull_review_approvals_tests(void) {
    test_run("a person approves the reviewed head with a note, its answer folded in and never retried", a_person_approves_the_reviewed_head_with_a_note);
    test_run("a person withdraws their own approval; refusals say why and a moved head reads again", a_person_withdraws_their_own_approval_and_refusals_say_why);
    test_run("approve and withdraw follow the token's permission, the routes and who the token is", approvals_follow_the_token_and_the_routes);
}
