#include "code_reviews.h"
#include "security_review.h"
#include "str.h"
#include "suites.h"
#include "test.h"
#include <stdlib.h>

// A review whose required pass completed with a coverage record: two categories (one that does not apply), two recorded
// findings, an earlier claim checked, and counts of how its security findings stand.
static const char *REVIEW = "{\"id\":20,\"security\":{\"policy\":\"required\",\"result\":\"failed\",\"blocking\":true,\"requiredByRisk\":\"high\","
    "\"stale\":false,\"stage\":\"completed\",\"reviewId\":20,\"headSha\":\"abc\","
    "\"reasons\":[{\"code\":\"fix_required\",\"effect\":\"failed\",\"message\":\"Token logged\"}],"
    "\"coverage\":{\"findings\":2,\"categories\":[{\"id\":\"secrets\",\"applicable\":true,\"assessment\":\"Checked the logger\",\"findings\":1},"
    "{\"id\":\"dependencies\",\"applicable\":false,\"assessment\":\"No manifest changed\",\"findings\":0},{\"applicable\":true}],"
    "\"entries\":[{\"id\":1,\"category\":\"secrets\",\"severity\":\"high\",\"title\":\"Token logged\",\"filePath\":\"app/x.c\",\"startLine\":12,\"claim\":null},"
    "{\"id\":\"e2\",\"category\":\"untrusted_input\",\"title\":\"Unescaped path\"},{\"id\":3}],"
    "\"claims\":[{\"findingRef\":\"f-7\",\"status\":\"fixed\",\"evidence\":\"Guard added\"},{\"status\":\"stands\"}],\"widened\":[\"core/api.c\",\"app/store.c\"]},"
    "\"counts\":{\"findings\":3,\"fix\":1,\"optional\":1,\"dismissed\":0,\"suppressed\":0,\"unverified\":1}},"
    "\"findings\":[{\"id\":5,\"security\":true},{\"id\":6,\"security\":false},{\"id\":7}]}";

static bool parsed(const char *text, SecurityReview *s) { Json *j = json_parsez(text); bool ok = security_review_parse(j, s); json_free(j); return ok; }

static void a_review_reads_its_security_pass_and_coverage(void) {
    SecurityReview s; CHECK(parsed(REVIEW, &s));
    CHECK_STR(s.result.result, "failed"); CHECK_STR(s.result.policy, "required"); CHECK(s.result.blocking); CHECK_INT(s.result.reasons.count, 1);
    CHECK_STR(s.stage, "completed"); CHECK_OWNED_STR(security_review_risk_line(&s), "Required by the high risk tier"); CHECK(!s.stale);
    CHECK(s.has_coverage); CHECK_INT(s.recorded, 2);
    // A category without an id, an entry without a title and a claim without a reference are left out.
    CHECK_INT(s.category_count, 2); CHECK_STR(s.categories[0].id, "secrets"); CHECK(s.categories[0].applicable); CHECK_INT(s.categories[0].findings, 1);
    CHECK_STR(s.categories[0].assessment, "Checked the logger"); CHECK(!s.categories[1].applicable);
    CHECK_INT(s.entry_count, 2); CHECK_STR(s.entries[0].id, "1"); CHECK_STR(s.entries[1].id, "e2"); CHECK(s.entries[0].claim == NULL);
    CHECK_OWNED_STR(security_entry_line(&s.entries[0]), "high \xC2\xB7 Token logged \xC2\xB7 app/x.c:12 \xC2\xB7 Secrets");
    CHECK_OWNED_STR(security_entry_line(&s.entries[1]), "Unescaped path \xC2\xB7 Untrusted input");
    CHECK_INT(s.claim_count, 1); CHECK_OWNED_STR(security_claim_line(&s.claims[0]), "f-7: fixed \xC2\xB7 Guard added");
    CHECK_STR(s.widened, "core/api.c, app/store.c");
    CHECK_OWNED_STR(security_review_counts_line(&s), "3 security findings \xC2\xB7 1 to fix \xC2\xB7 1 optional \xC2\xB7 1 unverified");
    CHECK_STR(security_review_stage_line(&s), "Security pass completed");
    security_review_free(&s); CHECK(s.categories == NULL && s.entries == NULL && s.stage == NULL);
    security_review_free(NULL);
    // Which findings count towards the security result is the server's flag alone.
    Json *j = json_parsez(REVIEW); const Json *findings = json_get(j, "findings");
    CHECK(code_review_finding_is_security(json_at(findings, 0))); CHECK(!code_review_finding_is_security(json_at(findings, 1)));
    CHECK(!code_review_finding_is_security(json_at(findings, 2))); json_free(j);
}
static void a_pass_that_did_not_run_says_so_and_none_shows_nothing(void) {
    SecurityReview s;
    // A required pass the review ran without is shown, with no coverage or counts.
    CHECK(parsed("{\"security\":{\"policy\":\"required\",\"result\":\"not_run\",\"stage\":null,\"coverage\":null}}", &s));
    CHECK(!s.has_coverage); CHECK(security_review_counts_line(&s) == NULL); CHECK(s.stage == NULL);
    CHECK_STR(security_review_stage_line(&s), "No security pass ran in this review"); CHECK(security_review_risk_line(&s) == NULL);
    security_review_free(&s);
    // A pass that is still running or failed says so.
    CHECK(parsed("{\"security\":{\"policy\":\"advisory\",\"result\":\"pending\",\"stage\":\"running\"}}", &s));
    CHECK_STR(security_review_stage_line(&s), "Security pass running"); security_review_free(&s);
    CHECK(parsed("{\"security\":{\"policy\":\"advisory\",\"result\":\"incomplete\",\"stage\":\"failed\"}}", &s));
    CHECK_STR(security_review_stage_line(&s), "Security pass failed"); security_review_free(&s);
    // A policy that ran no pass, an older server and a result-less record show nothing.
    CHECK(!parsed("{\"security\":{\"policy\":\"off\",\"result\":\"not_required\",\"stage\":null}}", &s)); CHECK(s.stage == NULL);
    CHECK(!parsed("{\"id\":4}", &s)); CHECK(!parsed("{\"security\":{\"policy\":\"required\"}}", &s));
    // Zero counts read as what they are.
    CHECK(parsed("{\"security\":{\"policy\":\"advisory\",\"result\":\"passed\",\"stage\":\"completed\",\"counts\":{\"findings\":1,\"dismissed\":1}}}", &s));
    CHECK_OWNED_STR(security_review_counts_line(&s), "1 security finding \xC2\xB7 1 dismissed"); security_review_free(&s);
    CHECK_OWNED_STR(security_category_title("permissions"), "Permissions"); CHECK_OWNED_STR(security_category_title("supply_chain"), "supply chain");
}
void core_security_review_tests(void) {
    test_run("a stored review reads its security pass, coverage, recorded findings and claims", a_review_reads_its_security_pass_and_coverage);
    test_run("a security pass that did not run says so, and none to show reads as nothing", a_pass_that_did_not_run_says_so_and_none_shows_nothing);
}
