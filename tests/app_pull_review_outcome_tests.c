#include "screen_pulls_internal.h"
#include "screen_fixture.h"
#include "suites.h"

#define REVIEWS "\"codeReviews\":[{\"id\":20,\"status\":\"completed\",\"handlingStatus\":\"pending\",\"findings\":[{\"id\":99,\"severity\":\"low\"," \
    "\"title\":\"Historical duplicate\",\"shouldFix\":\"fix\",\"verification\":\"confirmed\"}]}]"
#define OUTCOME_READ "{" REVIEWS ",\"reviewOutcome\":{\"outcome\":\"changes_required\",\"enforced\":true,\"status\":{\"published\":false}," \
    "\"reasons\":[{\"code\":\"fix_required\",\"effect\":\"changes_required\",\"message\":\"Fix the guard first\"}]," \
    "\"approvals\":{\"enabled\":true,\"result\":\"pending\",\"tier\":\"high\",\"required\":2,\"valid\":1,\"explanation\":\"Needs two people.\"}}," \
    "\"reviewRounds\":{\"limit\":3,\"used\":2}}"

static Route routes[] = {
    {"GET", "/pulls/{number}/code-reviews", "read"},
    {"PATCH", "/pulls/{number}/code-reviews/{reviewId}", "manage"},
    {"PATCH", "/pulls/{number}/code-reviews/{reviewId}/findings/{findingId}", "manage"},
};
static PullScreen *setup(ScreenFixture *f, const char *reviews) {
    screen_fixture_init(f, routes, 3); g_store.device.permission = "manage"; g_store.device.id = "device";
    Project p = {.repo = "o/r"}; PullScreen *s = (PullScreen *)pull_detail_screen_new(&p, 12, NULL, NULL);
    screen_fixture_pane(f, &s->base);
    s->tab = PR_TAB_CODE_REVIEWS; s->shown = true; s->reviews.visible = true;
    f->read_response = reviews;
    screen_pulls_reviews_load(s, false); screen_fixture_pump(f, 1);
    return s;
}
static void draw(PullScreen *s, Doc *d) { doc_begin(d, NULL, 1200); s->base.vt->layout(&s->base, d); doc_end(d); }
static bool contains(Doc *d, const char *text) {
    for (size_t i = 0; i < d->count; i++) if (d->items[i].text && strstr(d->items[i].text, text)) return true;
    return false;
}
static void act(PullScreen *s, int action, intptr_t arg) { s->base.vt->action(&s->base, action, arg, (POINT){0}); }

static void the_outcome_heads_the_tab_and_follows_each_answer(void) {
    ScreenFixture f; PullScreen *s = setup(&f, OUTCOME_READ);
    Doc d; doc_init(&d); draw(s, &d);
    CHECK(contains(&d, "Review outcome: Changes required")); CHECK(contains(&d, "2 of 3 rounds"));
    CHECK(contains(&d, "Fix the guard first")); CHECK(contains(&d, "1 of 2 approvals")); CHECK(contains(&d, "Needs two people."));
    CHECK(contains(&d, "briareus/code-review"));
    // A handling save answers neither the outcome nor the rounds: both stay.
    f.write_response = "{\"review\":{\"id\":20,\"status\":\"completed\",\"handlingStatus\":\"handled\",\"findings\":[]}}";
    act(s, ACT_REVIEW_ROW, 20); act(s, ACT_REVIEW_HANDLING, 20); screen_fixture_pump(&f, 1);
    act(s, ACT_REVIEW_BACK, 0); draw(s, &d); CHECK(contains(&d, "Review outcome: Changes required")); CHECK(contains(&d, "2 of 3 rounds"));
    // A verdict save answers a fresh outcome, which replaces the old one and keeps the rounds.
    f.read_response = OUTCOME_READ; screen_pulls_reviews_load(s, true); screen_fixture_pump(&f, 1);
    act(s, ACT_REVIEW_ROW, 20); act(s, ACT_REVIEW_EDIT, 99); act(s, ACT_REVIEW_VERDICT, 3);
    f.write_response = "{\"finding\":{\"id\":99,\"severity\":\"low\",\"title\":\"Historical duplicate\",\"verification\":\"confirmed\"},"
                       "\"reviewOutcome\":{\"outcome\":\"passed\",\"ready\":true}}";
    act(s, ACT_REVIEW_SAVE, 0); screen_fixture_pump(&f, 1);
    CHECK_INT(f.writes, 2); CHECK(s->reviews.outcome != NULL);
    act(s, ACT_REVIEW_BACK, 0); draw(s, &d);
    CHECK(contains(&d, "Review outcome: Passed")); CHECK(contains(&d, "2 of 3 rounds")); CHECK(!contains(&d, "Fix the guard first"));
    // A verdict save without an outcome leaves none standing until the next read.
    act(s, ACT_REVIEW_ROW, 20); act(s, ACT_REVIEW_EDIT, 99); act(s, ACT_REVIEW_VERDICT, 0);
    f.write_response = "{\"finding\":{\"id\":99,\"severity\":\"low\",\"title\":\"Historical duplicate\",\"shouldFix\":\"fix\",\"verification\":\"confirmed\"}}";
    act(s, ACT_REVIEW_SAVE, 0); screen_fixture_pump(&f, 1);
    act(s, ACT_REVIEW_BACK, 0); draw(s, &d); CHECK(!contains(&d, "Review outcome")); CHECK(s->reviews.outcome == NULL);
    CHECK_INT(f.reads, 2);
    doc_free(&d); s->base.vt->destroy(&s->base); screen_fixture_clear(&f);
}
static void the_security_pass_shows_in_full_under_the_outcome(void) {
    ScreenFixture f; PullScreen *s = setup(&f, "{" REVIEWS ",\"reviewOutcome\":{\"outcome\":\"changes_required\",\"security\":{\"policy\":\"required\","
        "\"result\":\"failed\",\"blocking\":true,\"requiredByRisk\":\"high\",\"headSha\":\"abcdef1234567\",\"stage\":\"completed\","
        "\"reasons\":[{\"code\":\"fix_required\",\"effect\":\"failed\",\"message\":\"Fix the logged token\"}],"
        "\"coverage\":{\"categories\":[{\"id\":\"secrets\",\"applicable\":true,\"assessment\":\"A token is logged.\",\"findings\":1}],"
        "\"entries\":[{\"id\":\"s1\",\"severity\":\"high\",\"title\":\"Token in the log\",\"filePath\":\"core/api.c\",\"startLine\":42}]},"
        "\"counts\":{\"findings\":1,\"fix\":1}}}}");
    Doc d; doc_init(&d); draw(s, &d);
    CHECK(contains(&d, "Failed \xC2\xB7 required")); CHECK(contains(&d, "high risk tier")); CHECK(contains(&d, "Pass completed on abcdef1"));
    CHECK(contains(&d, "1 finding \xC2\xB7 1 to fix")); CHECK(contains(&d, "Fix the logged token"));
    CHECK(contains(&d, "Token in the log")); CHECK(contains(&d, "core/api.c:42"));
    // What the pass examined is folded until it is opened.
    CHECK(!contains(&d, "A token is logged.")); CHECK(contains(&d, "What the pass examined"));
    act(s, ACT_REVIEW_SECURITY, 0); draw(s, &d);
    CHECK(contains(&d, "Secrets")); CHECK(contains(&d, "Applies \xC2\xB7 1 finding")); CHECK(contains(&d, "A token is logged."));
    // A project whose policy is off ran no pass: no block.
    f.read_response = "{" REVIEWS ",\"reviewOutcome\":{\"outcome\":\"passed\",\"security\":{\"policy\":\"off\",\"result\":\"not_required\"}}}";
    screen_pulls_reviews_load(s, true); screen_fixture_pump(&f, 1); draw(s, &d);
    CHECK(contains(&d, "Review outcome: Passed")); CHECK(!contains(&d, "What the pass examined")); CHECK(!contains(&d, "Token in the log"));
    doc_free(&d); s->base.vt->destroy(&s->base); screen_fixture_clear(&f);
}
static void an_older_server_shows_no_outcome(void) {
    ScreenFixture f; PullScreen *s = setup(&f, "{" REVIEWS "}");
    Doc d; doc_init(&d); draw(s, &d);
    CHECK(s->reviews.outcome == NULL); CHECK(!contains(&d, "Review outcome"));
    // An empty list with an outcome still shows it, above the empty note.
    f.read_response = "{\"codeReviews\":[],\"reviewOutcome\":{\"outcome\":\"pending\",\"reasons\":[{\"code\":\"no_review\",\"effect\":\"pending\",\"message\":\"No review yet\"}]}}";
    screen_pulls_reviews_load(s, true); screen_fixture_pump(&f, 1); draw(s, &d);
    CHECK(contains(&d, "Review outcome: Pending")); CHECK(contains(&d, "No review yet")); CHECK(contains(&d, "No stored code reviews"));
    // A malformed list keeps what was shown.
    f.read_response = "{\"codeReviews\":null,\"reviewOutcome\":{\"outcome\":\"passed\"}}";
    screen_pulls_reviews_load(s, true); screen_fixture_pump(&f, 1); draw(s, &d);
    CHECK(contains(&d, "Review outcome: Pending"));
    doc_free(&d); s->base.vt->destroy(&s->base); screen_fixture_clear(&f);
}
void app_pull_review_outcome_tests(void) {
    test_run("the review outcome heads the Code reviews tab and follows reads, verdict saves and handling saves", the_outcome_heads_the_tab_and_follows_each_answer);
    test_run("an older server shows no review outcome; an empty list still shows one", an_older_server_shows_no_outcome);
    test_run("the security pass shows in full under the outcome, what it examined folded", the_security_pass_shows_in_full_under_the_outcome);
}
