#include "review_security.h"
#include "str.h"
#include "suites.h"
#include "test.h"
#include <stdlib.h>
#include <string.h>

// An advisory project whose high-risk change made the pass required: it completed on the head with one finding to fix,
// a dismissed one, two categories examined and earlier claims checked.
static const char *FAILED = "{\"policy\":\"required\",\"result\":\"failed\",\"blocking\":true,\"requiredByRisk\":\"high\",\"stale\":false,"
    "\"reviewId\":20,\"headSha\":\"abcdef1234567\",\"stage\":\"completed\","
    "\"reasons\":[{\"code\":\"fix_required\",\"effect\":\"failed\",\"message\":\"A security finding stands as a required fix.\"}],"
    "\"coverage\":{\"findings\":2,\"categories\":["
    "{\"id\":\"secrets\",\"applicable\":true,\"assessment\":\"A token is logged.\",\"findings\":1},"
    "{\"id\":\"untrusted_input\",\"applicable\":false,\"assessment\":\"No input is read.\",\"findings\":0},"
    "{\"id\":\"supply_chain\",\"applicable\":true,\"findings\":0},{\"applicable\":true}],"
    "\"entries\":[{\"id\":\"s1\",\"category\":\"secrets\",\"severity\":\"high\",\"title\":\"Token in the log\",\"filePath\":\"core/api.c\",\"startLine\":42,\"claim\":\"f-7\"},"
    "{\"id\":\"s2\",\"title\":\"Whole file\",\"filePath\":\"core/x.c\"},{\"id\":\"s3\"}],"
    "\"claims\":[{\"findingRef\":\"a\",\"status\":\"fixed\",\"evidence\":\"Guard added\"},{\"findingRef\":\"b\",\"status\":\"fixed\"},{\"findingRef\":\"c\",\"status\":\"stands\"},{\"status\":\"other\"}],"
    "\"widened\":[\"core/api.c\",\"\",\"core/x.c\"]},"
    "\"counts\":{\"findings\":2,\"fix\":1,\"optional\":0,\"dismissed\":1,\"suppressed\":0,\"unverified\":0}}";

static void parse_security(const char *text, ReviewSecurity *out) { Json *j = json_parsez(text); review_security_parse(j, out); json_free(j); }

static void a_pass_reads_its_coverage_counts_and_scope(void) {
    ReviewSecurity s; parse_security(FAILED, &s);
    CHECK(s.present); CHECK(s.blocking); CHECK(!s.stale); CHECK(s.covered);
    CHECK_STR(s.policy, "required"); CHECK_STR(s.result, "failed"); CHECK_STR(s.required_by_risk, "high"); CHECK_STR(s.stage, "completed");
    CHECK_INT(s.reasons.count, 1); CHECK_INT(review_reason_tone(&s.reasons.items[0]), REVIEW_TONE_DANGER);
    CHECK_INT(s.counts.findings, 2); CHECK_INT(s.counts.fix, 1); CHECK_INT(s.counts.dismissed, 1);
    // Categories and entries without an id or a title are skipped.
    CHECK_INT(s.category_count, 3); CHECK_INT(s.entry_count, 2); CHECK_INT(s.claim_count, 3); CHECK_INT(s.recorded, 2);
    // Each claim checked with its evidence, a standing one as a warning; an entry names the claim it raises again; what
    // an incremental pass widened to reads as a list.
    CHECK_OWNED_STR(review_security_claim_line(&s.claims[0]), "a: fixed \xC2\xB7 Guard added");
    CHECK_OWNED_STR(review_security_claim_line(&s.claims[2]), "c: stands");
    CHECK_INT(review_security_claim_tone(&s.claims[0]), REVIEW_TONE_OK); CHECK_INT(review_security_claim_tone(&s.claims[2]), REVIEW_TONE_WARN);
    CHECK_STR(s.entries[0].claim, "f-7"); CHECK(s.entries[1].claim == NULL); CHECK_STR(s.widened, "core/api.c, core/x.c");
    CHECK_OWNED_STR(review_security_risk_note(&s), "Required by this change's high risk tier.");
    CHECK_OWNED_STR(review_security_scope_line(&s), "Pass completed on abcdef1");
    CHECK_OWNED_STR(review_security_findings_line(&s), "2 findings \xC2\xB7 1 to fix \xC2\xB7 1 dismissed");
    CHECK_OWNED_STR(review_security_claims_line(&s), "Earlier claims: 2 fixed \xC2\xB7 1 still stands");
    CHECK_OWNED_STR(review_security_category_title(&s.categories[0]), "Secrets");
    CHECK_OWNED_STR(review_security_category_title(&s.categories[1]), "Untrusted input");
    CHECK_OWNED_STR(review_security_category_title(&s.categories[2]), "supply chain");
    CHECK_OWNED_STR(review_security_category_line(&s.categories[0]), "Applies \xC2\xB7 1 finding");
    CHECK_OWNED_STR(review_security_category_line(&s.categories[1]), "Does not apply");
    CHECK_OWNED_STR(review_security_category_line(&s.categories[2]), "Applies");
    CHECK_INT(review_security_category_tone(&s.categories[0]), REVIEW_TONE_WARN);
    CHECK_INT(review_security_category_tone(&s.categories[1]), REVIEW_TONE_MUTED);
    CHECK_INT(review_security_category_tone(&s.categories[2]), REVIEW_TONE_OK);
    CHECK_STR(s.categories[0].assessment, "A token is logged.");
    CHECK_OWNED_STR(review_security_entry_location(&s.entries[0]), "core/api.c:42");
    CHECK_OWNED_STR(review_security_entry_location(&s.entries[1]), "core/x.c");
    CHECK_STR(s.entries[0].severity, "high"); CHECK(s.entries[1].severity == NULL);
    review_security_free(&s); CHECK(s.categories == NULL && s.entry_count == 0);
    review_security_free(NULL);
}
static void a_pass_not_run_or_out_of_date_says_so(void) {
    // Before any pass completed: nothing counted, no coverage, no findings line.
    ReviewSecurity s; parse_security("{\"policy\":\"advisory\",\"result\":\"not_run\",\"stage\":null,\"headSha\":\"abc1234999\",\"counts\":{\"findings\":0}}", &s);
    CHECK(!s.covered); CHECK(review_security_findings_line(&s) == NULL); CHECK(review_security_claims_line(&s) == NULL);
    CHECK(review_security_risk_note(&s) == NULL);
    CHECK_OWNED_STR(review_security_scope_line(&s), "No security pass ran on abc1234");
    review_security_free(&s);
    // A clean completed pass on a head the pull request moved on from.
    parse_security("{\"policy\":\"required\",\"result\":\"pending\",\"stale\":true,\"stage\":\"completed\",\"headSha\":\"abcdef1234567\","
         "\"coverage\":{\"findings\":0,\"categories\":[]},\"counts\":{\"findings\":0}}", &s);
    CHECK_OWNED_STR(review_security_scope_line(&s), "Pass completed on abcdef1 \xC2\xB7 out of date");
    CHECK_OWNED_STR(review_security_findings_line(&s), "No security findings");
    review_security_free(&s);
    // A running pass without a head, a stage this client does not know, and a pass with neither.
    parse_security("{\"result\":\"pending\",\"stage\":\"running\"}", &s); CHECK_STR(s.policy, "off");
    CHECK_OWNED_STR(review_security_scope_line(&s), "Pass running"); review_security_free(&s);
    parse_security("{\"result\":\"pending\",\"stage\":\"bogus\"}", &s); CHECK(review_security_scope_line(&s) == NULL); review_security_free(&s);
    parse_security("{\"policy\":\"required\"}", &s); CHECK(!s.present); review_security_free(&s);
}
static void pull_requests_rank_worst_first_and_sum_up(void) {
    const char *results[] = { "failed", "incomplete", "not_run", "pending", "passed", "not_required" };
    for (size_t i = 0; i < 6; i++) {
        ReviewSecurity s = { .present = true, .policy = "advisory", .result = (char *)results[i] };
        CHECK_INT(review_security_rank(&s), (int)i);
    }
    ReviewSecurity absent = {0}; CHECK_INT(review_security_rank(&absent), SECURITY_RANK_OFF); CHECK_INT(review_security_rank(NULL), SECURITY_RANK_OFF);
    ReviewSecurity odd = { .present = true, .result = "bogus" }; CHECK_INT(review_security_rank(&odd), SECURITY_RANK_PENDING);
    CHECK_STR(review_security_rank_title(SECURITY_RANK_NOT_RUN), "Not run"); CHECK_STR(review_security_rank_title(SECURITY_RANK_OFF), "Not asked");
    size_t counts[SECURITY_RANKS] = { 2, 0, 1, 0, 10, 3 };
    CHECK_OWNED_STR(review_security_summary(counts), "2 failed \xC2\xB7 1 not run \xC2\xB7 10 passed \xC2\xB7 3 not asked");
    size_t none[SECURITY_RANKS] = {0}; CHECK_OWNED_STR(review_security_summary(none), "No open pull requests");
}
static bool of_review(const char *text, ReviewSecurity *out) {
    Json *j = json_parsez(text); bool shown = review_security_of_review(j, out); json_free(j); return shown;
}
static void a_stored_review_shows_its_own_pass_only_when_it_has_one(void) {
    ReviewSecurity s;
    CHECK(of_review("{\"id\":21,\"security\":{\"policy\":\"advisory\",\"result\":\"passed\",\"stage\":\"completed\",\"widened\":\"x\","
        "\"coverage\":{\"findings\":0,\"widened\":\"The whole package\",\"claims\":[{\"findingRef\":\"f-1\"}]}}}", &s));
    CHECK_STR(s.result, "passed"); CHECK_STR(s.widened, "The whole package");
    CHECK_OWNED_STR(review_security_claim_line(&s.claims[0]), "f-1: unchecked"); CHECK_INT(review_security_claim_tone(&s.claims[0]), REVIEW_TONE_MUTED);
    review_security_free(&s);
    // A policy that ran no pass in it, or an older server without a result, shows nothing; a stage alone still shows.
    CHECK(!of_review("{\"id\":20,\"security\":{\"policy\":\"off\",\"result\":\"not_required\",\"stage\":null}}", &s)); CHECK(!s.present);
    CHECK(!of_review("{\"id\":19}", &s));
    CHECK(of_review("{\"id\":18,\"security\":{\"policy\":\"off\",\"result\":\"not_required\",\"stage\":\"failed\"}}", &s));
    review_security_free(&s);
}
void core_review_security_tests(void) {
    test_run("a stored review shows its own security pass only when it ran one", a_stored_review_shows_its_own_pass_only_when_it_has_one);
    test_run("pull requests rank by security result, worst first, and sum up", pull_requests_rank_worst_first_and_sum_up);
    test_run("a security pass reads its coverage, counts, claims and the head it covered", a_pass_reads_its_coverage_counts_and_scope);
    test_run("a security pass not run, still running or out of date says so", a_pass_not_run_or_out_of_date_says_so);
}
