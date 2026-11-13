#include "screen_pulls_internal.h"
#include "screen_fixture.h"
#include "suites.h"

// Review 21 ran a required security pass that failed, with a coverage record and one security finding; review 20 ran
// none under a policy that is off.
#define REVIEWS "{\"codeReviews\":[" \
    "{\"id\":21,\"status\":\"completed\",\"security\":{\"policy\":\"required\",\"result\":\"failed\",\"blocking\":true,\"stage\":\"completed\"," \
    "\"reasons\":[{\"code\":\"fix_required\",\"effect\":\"failed\",\"message\":\"Fix the logged token\"}]," \
    "\"coverage\":{\"findings\":1,\"categories\":[{\"id\":\"secrets\",\"applicable\":true,\"assessment\":\"Checked the logger\",\"findings\":1}," \
    "{\"id\":\"dependencies\",\"applicable\":false,\"assessment\":\"No manifest changed\",\"findings\":0}]," \
    "\"entries\":[{\"id\":1,\"category\":\"secrets\",\"severity\":\"high\",\"title\":\"Token logged\",\"filePath\":\"app/x.c\",\"startLine\":12}]," \
    "\"claims\":[{\"findingRef\":\"f-7\",\"status\":\"fixed\",\"evidence\":\"Guard added\"}]}," \
    "\"counts\":{\"findings\":1,\"fix\":1}}," \
    "\"findings\":[{\"id\":101,\"severity\":\"high\",\"title\":\"Token logged\",\"security\":true,\"shouldFix\":\"fix\"}," \
    "{\"id\":102,\"severity\":\"low\",\"title\":\"Naming\",\"security\":false}]}," \
    "{\"id\":20,\"status\":\"completed\",\"security\":{\"policy\":\"off\",\"result\":\"not_required\",\"stage\":null},\"findings\":[]}]}"

static Route routes[] = {{"GET", "/pulls/{number}/code-reviews", "read"}};
static PullScreen *setup(ScreenFixture *f) {
    screen_fixture_init(f, routes, 1); g_store.device.permission = "read"; g_store.device.id = "device";
    Project p = {.repo = "o/r"}; PullScreen *s = (PullScreen *)pull_detail_screen_new(&p, 12, NULL, NULL);
    screen_fixture_pane(f, &s->base);
    s->tab = PR_TAB_CODE_REVIEWS; s->shown = true; s->reviews.visible = true;
    f->read_response = REVIEWS;
    screen_pulls_reviews_load(s, false); screen_fixture_pump(f, 1);
    return s;
}
static void draw(PullScreen *s, Doc *d) { doc_begin(d, NULL, 1200); s->base.vt->layout(&s->base, d); doc_end(d); }
static bool contains(Doc *d, const char *text) {
    for (size_t i = 0; i < d->count; i++) if (d->items[i].text && strstr(d->items[i].text, text)) return true;
    return false;
}
static void act(PullScreen *s, int action, intptr_t arg) { s->base.vt->action(&s->base, action, arg, (POINT){0}); }

static void an_opened_review_unfolds_its_security_pass(void) {
    ScreenFixture f; PullScreen *s = setup(&f);
    Doc d; doc_init(&d);
    act(s, ACT_REVIEW_ROW, 21); draw(s, &d);
    CHECK(contains(&d, "Security review \xC2\xB7 Failed \xC2\xB7 required")); CHECK(!contains(&d, "Checked the logger"));
    act(s, ACT_REVIEW_SECURITY, 0); draw(s, &d);
    CHECK(contains(&d, "Security pass completed")); CHECK(contains(&d, "1 security finding \xC2\xB7 1 to fix")); CHECK(contains(&d, "Fix the logged token"));
    CHECK(contains(&d, "Secrets \xC2\xB7 1 finding")); CHECK(contains(&d, "Checked the logger")); CHECK(contains(&d, "Dependencies \xC2\xB7 does not apply"));
    CHECK(contains(&d, "high \xC2\xB7 Token logged \xC2\xB7 app/x.c:12 \xC2\xB7 Secrets")); CHECK(contains(&d, "f-7: fixed \xC2\xB7 Guard added"));
    // The selected security finding says so; the table marks it among the rest.
    CHECK(contains(&d, "Security finding"));
    act(s, ACT_REVIEW_FINDING, 102); draw(s, &d); CHECK(!contains(&d, "Security finding"));
    // Folding hides the detail; another review starts folded, and one that ran no pass shows no section.
    act(s, ACT_REVIEW_SECURITY, 0); draw(s, &d); CHECK(!contains(&d, "Checked the logger"));
    act(s, ACT_REVIEW_SECURITY, 0); act(s, ACT_REVIEW_BACK, 0); act(s, ACT_REVIEW_ROW, 20); draw(s, &d);
    CHECK(!contains(&d, "Security review")); CHECK(!contains(&d, "Checked the logger"));
    doc_free(&d); s->base.vt->destroy(&s->base); screen_fixture_clear(&f);
}
void app_pull_review_security_tests(void) {
    test_run("an opened code review unfolds its own security pass, coverage and recorded findings", an_opened_review_unfolds_its_security_pass);
}
