#include "code_reviews.h"
#include "str.h"
#include "suites.h"
#include "test.h"
#include "fixtures/code_reviews.h"
#include <stdlib.h>

static void stored_contract(void) {
    Json *payload = json_parsez(CODE_REVIEWS_FIXTURE), *reviews = code_reviews_parse(payload);
    json_free(payload); CHECK(reviews != NULL); CHECK_INT(json_count(reviews), 2);
    CHECK_OWNED_STR(code_reviews_counts(reviews), "3 findings: 0 critical, 1 high, 1 medium, 1 low; 1 pending verdict; 0 without verdict");
    CHECK_INT(code_reviews_findings(reviews), 3); CHECK(code_reviews_running(reviews));
    const Json *r = code_review_find(reviews, 21), *f = code_review_finding(r, 101);
    CHECK(r != NULL); CHECK(f != NULL); CHECK(code_review_finding(r, 99) == NULL); CHECK(code_review_find(reviews, 0) == NULL);
    CHECK_STR(json_str(json_get(r, "sessionId")), "deleted-session"); CHECK_STR(json_str(json_get(f, "body")), "### Impact\nA long explanation wraps across the available width, with **evidence** and `source code`.\n\n- Keep stable IDs\n- Reject obsolete responses");
    CHECK_STR(code_review_head(r, "aaaaaaaaaa1111"), "current head");
    CHECK_STR(code_review_head(r, "new"), "older head"); CHECK_STR(code_review_head(r, NULL), "head unknown");
    CHECK_STR(code_review_head(NULL, "new"), "head unknown");
    CHECK_STR(code_review_verdict(r, f), "pending"); CHECK_STR(code_review_verification(r, f), "pending");
    CHECK_OWNED_STR(code_review_location(f), "app/screens/a/long/location/selected_record.c:120-128");
    f = code_review_finding(r, 102); CHECK_STR(code_review_verdict(r, f), "fix"); CHECK_STR(code_review_verification(r, f), "confirmed");
    CHECK_OWNED_STR(code_review_location(f), "core/connection.c:8");
    CHECK_OWNED_STR(code_review_usage(r), "unknown in / 0 out; Cost unknown; 2 unpriced turns");
    CHECK_OWNED_STR(code_review_counts(r), "2 findings: 0 critical, 1 high, 1 medium, 0 low; 1 pending verdict; 0 without verdict");
    r = code_review_find(reviews, 20); f = code_review_finding(r, 99);
    CHECK_STR(code_review_verdict(r, f), "dismiss"); CHECK_STR(code_review_verification(r, f), "plausible");
    CHECK_OWNED_STR(code_review_location(f), "No location");
    CHECK_OWNED_STR(code_review_usage(r), "1240 in / 180 out; $0.0123 estimated; 1 unpriced turns (partial cost)");
    Json *changed = json_clone(code_review_find(reviews, 21));
    Json *critical = (Json *)json_at(json_get(changed, "findings"), 0); json_set_str(critical, "severity", "critical");
    CHECK_OWNED_STR(code_review_counts(changed), "2 findings: 1 critical, 0 high, 1 medium, 0 low; 1 pending verdict; 0 without verdict");
    json_free(changed); json_free(reviews);
}
static void malformed_and_empty(void) {
    const char *bad[] = {"{}", "{\"codeReviews\":null}", "{\"codeReviews\":{}}", "{\"codeReviews\":[{}]}",
        "{\"codeReviews\":[{\"id\":1,\"status\":\"unknown\",\"findings\":[]}]}",
        "{\"codeReviews\":[{\"id\":1,\"status\":\"failed\",\"findings\":null}]}"};
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) { Json *j = json_parsez(bad[i]); CHECK(code_reviews_parse(j) == NULL); json_free(j); }
    Json *j = json_parsez("{\"codeReviews\":[]}"); Json *r = code_reviews_parse(j);
    CHECK(r != NULL); CHECK_INT(json_count(r), 0); CHECK(!code_reviews_running(r)); json_free(j); json_free(r);
    j = json_parsez("{\"id\":1.5}"); CHECK_INT(code_review_id(j), 0); json_free(j);
    j = json_parsez("{\"id\":9007199254740992}"); CHECK_INT(code_review_id(j), 0); json_free(j);
    j = json_parsez("{\"filePath\":\"file.c\"}"); CHECK_OWNED_STR(code_review_location(j), "file.c"); json_free(j);
    j = json_parsez("{\"costUsd\":0,\"inputTokens\":0,\"outputTokens\":0}");
    CHECK_OWNED_STR(code_review_usage(j), "0 in / 0 out; $0.0000; 0 unpriced turns"); json_free(j);
}
static void limit_and_validation(void) {
    Json *payload = json_parsez(CODE_REVIEWS_FIXTURE); Json *rows = (Json *)json_get(payload, "codeReviews");
    Json *r = (Json *)json_at(rows, 0), *findings = (Json *)json_get(r, "findings");
    json_array_push(findings, json_clone(json_at(findings, 0))); CHECK(code_reviews_parse(payload) == NULL);
    json_free(payload); payload = json_parsez(CODE_REVIEWS_FIXTURE); rows = (Json *)json_get(payload, "codeReviews");
    json_array_push(rows, json_clone(json_at(rows, 0))); CHECK(code_reviews_parse(payload) == NULL); json_free(payload);
    payload = json_object(); rows = json_array(); json_object_set(payload, "codeReviews", rows);
    for (int i = 1; i <= 21; i++) {
        r = json_object(); json_set_num(r, "id", i); json_set_str(r, "status", "failed"); json_object_set(r, "findings", json_array()); json_array_push(rows, r);
        Json *parsed = code_reviews_parse(payload); CHECK((parsed != NULL) == (i <= 20)); json_free(parsed);
    }
    json_free(payload);
    const char *fields[] = {"id", "title", "severity"};
    for (size_t i = 0; i < 3; i++) {
        payload = json_parsez(CODE_REVIEWS_FIXTURE); r = (Json *)json_at(json_get(payload, "codeReviews"), 0);
        Json *f = (Json *)json_at(json_get(r, "findings"), 0); json_object_remove(f, fields[i]);
        CHECK(code_reviews_parse(payload) == NULL); json_free(payload);
    }
}
static void terminal_null_verdicts(void) {
    Json *payload = json_parsez(CODE_REVIEWS_FINAL_NULL_FIXTURE), *reviews = code_reviews_parse(payload);
    json_free(payload); CHECK(reviews != NULL); CHECK(!code_reviews_running(reviews));
    for (size_t i = 0; i < 2; i++) {
        const Json *r = json_at(reviews, i), *f = json_at(json_get(r, "findings"), 0);
        CHECK_STR(code_review_verdict(r, f), "no verdict");
        CHECK_STR(code_review_verification(r, f), "not verified");
        CHECK(json_str(json_get(f, "comments")) != NULL);
        CHECK_OWNED_STR(code_review_counts(r), "1 finding: 0 critical, 0 high, 1 medium, 0 low; 0 pending verdicts; 1 without verdict");
    }
    CHECK_OWNED_STR(code_reviews_counts(reviews), "2 findings: 0 critical, 0 high, 2 medium, 0 low; 0 pending verdicts; 2 without verdict");
    Json *running_payload = json_parsez(CODE_REVIEWS_FIXTURE), *running = code_reviews_parse(running_payload);
    json_array_push(reviews, json_clone(json_at(running, 0)));
    CHECK_OWNED_STR(code_reviews_counts(reviews), "4 findings: 0 critical, 1 high, 3 medium, 0 low; 1 pending verdict; 2 without verdict");
    Json *mixed = json_clone(json_at(reviews, 0)); Json *f = (Json *)json_at(json_get(mixed, "findings"), 0);
    json_set_str(f, "shouldFix", "optional");
    CHECK_STR(code_review_verdict(mixed, f), "optional"); CHECK_STR(code_review_verification(mixed, f), "not verified");
    json_set_str(f, "verification", "plausible"); json_object_set(f, "shouldFix", json_null());
    CHECK_STR(code_review_verdict(mixed, f), "no verdict"); CHECK_STR(code_review_verification(mixed, f), "plausible");
    json_free(mixed); json_free(running_payload); json_free(running); json_free(reviews);
}
static void usage_totals_preserve_unknowns(void) {
    Json *payload = json_parsez(CODE_REVIEWS_FIXTURE), *rows = code_reviews_parse(payload);
    CodeReviewTotals t = code_reviews_totals(rows);
    CHECK_INT(t.reviews, 2); CHECK_INT(t.findings, 3);
    CHECK_INT(t.input.known, 1); CHECK_INT(t.input.unknown, 1); CHECK(t.input.value == 1240);
    CHECK_INT(t.output.known, 2); CHECK_INT(t.output.unknown, 0); CHECK(t.output.value == 180);
    CHECK_INT(t.cost.known, 1); CHECK_INT(t.cost.unknown, 1); CHECK(t.cost.value == 0.0123);
    CHECK(t.estimated); CHECK_INT(t.unpriced_turns, 3);
    // Adding findings to one review never multiplies its usage or cost.
    Json *r = (Json *)json_at(rows, 1), *findings = (Json *)json_get(r, "findings");
    json_array_push(findings, json_clone(json_at(findings, 0)));
    t = code_reviews_totals(rows); CHECK_INT(t.findings, 4); CHECK(t.cost.value == 0.0123); CHECK(t.input.value == 1240);
    Json *zero = json_parsez("{\"inputTokens\":0,\"outputTokens\":0,\"costUsd\":0,\"costEstimated\":false,\"unpricedTurns\":0}");
    CodeReviewTotals z = code_review_totals(zero);
    CHECK_INT(z.input.known, 1); CHECK_INT(z.output.known, 1); CHECK_INT(z.cost.known, 1);
    CHECK(z.input.value == 0); CHECK(z.output.value == 0); CHECK(z.cost.value == 0); CHECK(!z.estimated);
    json_array_push(rows, zero); t = code_reviews_totals(rows);
    CHECK_INT(t.reviews, 3); CHECK_INT(t.cost.known, 2); CHECK_INT(t.cost.unknown, 1); CHECK(t.cost.value == 0.0123);
    json_free(rows); json_free(payload);
    rows = json_parsez("[{\"costUsd\":null,\"inputTokens\":null,\"outputTokens\":null,\"costEstimated\":true,\"unpricedTurns\":2},{}]");
    t = code_reviews_totals(rows);
    CHECK_INT(t.input.known, 0); CHECK_INT(t.input.unknown, 2);
    CHECK_INT(t.output.known, 0); CHECK_INT(t.output.unknown, 2);
    CHECK_INT(t.cost.known, 0); CHECK_INT(t.cost.unknown, 2); CHECK(!t.estimated); CHECK_INT(t.unpriced_turns, 2);
    json_free(rows); rows = json_array(); t = code_reviews_totals(rows);
    CHECK_INT(t.reviews, 0); CHECK_INT(t.cost.known, 0); json_free(rows);
    Json *r1 = json_parsez("{\"status\":\"failed\",\"findings\":[{\"severity\":\"high\"}]}");
    CHECK_OWNED_STR(code_review_counts_compact(r1), "1 finding\n1 high\n1 no verdict");
    json_set_str(r1, "status", "running");
    CHECK_OWNED_STR(code_review_counts_compact(r1), "1 finding\n1 high\n1 pending"); json_free(r1);
}
void core_code_reviews_tests(void) {
    test_run("review totals sum each review once and preserve zero, unknown, estimated and unpriced values", usage_totals_preserve_unknowns);
    test_run("terminal null review verdicts remain inconclusive and only running nulls are pending", terminal_null_verdicts);
    test_run("stored code review nested public contract and null semantics", stored_contract);
    test_run("code reviews malformed is distinct from empty", malformed_and_empty);
    test_run("code reviews latest twenty, unique stable IDs and required fields", limit_and_validation);
}
