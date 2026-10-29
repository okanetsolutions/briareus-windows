#include "code_reviews.h"
#include "str.h"
#include "suites.h"
#include "test.h"
#include "fixtures/code_reviews.h"
#include <stdlib.h>

static void stored_contract(void) {
    Json *payload = json_parsez(CODE_REVIEWS_FIXTURE), *reviews = code_reviews_parse(payload);
    json_free(payload); CHECK(reviews != NULL); CHECK_INT(json_count(reviews), 2);
    CHECK_OWNED_STR(code_reviews_counts(reviews), "3 findings: 0 critical, 1 high, 1 medium, 1 low; 1 pending verdicts");
    CHECK_INT(code_reviews_findings(reviews), 3); CHECK(code_reviews_running(reviews));
    const Json *r = code_review_find(reviews, 21), *f = code_review_finding(r, 101);
    CHECK(r != NULL); CHECK(f != NULL); CHECK(code_review_finding(r, 99) == NULL); CHECK(code_review_find(reviews, 0) == NULL);
    CHECK_STR(json_str(json_get(r, "sessionId")), "deleted-session"); CHECK_STR(json_str(json_get(f, "body")), "### Impact\nA long explanation wraps across the available width, with **evidence** and `source code`.\n\n- Keep stable IDs\n- Reject obsolete responses");
    CHECK_STR(code_review_head(r, "aaaaaaaaaa1111"), "current head");
    CHECK_STR(code_review_head(r, "new"), "older head"); CHECK_STR(code_review_head(r, NULL), "head unknown");
    CHECK_STR(code_review_head(NULL, "new"), "head unknown");
    CHECK_STR(code_review_verdict(f), "pending"); CHECK_STR(code_review_verification(f), "pending");
    CHECK_OWNED_STR(code_review_location(f), "app/screens/a/long/location/selected_record.c:120-128");
    f = code_review_finding(r, 102); CHECK_STR(code_review_verdict(f), "fix"); CHECK_STR(code_review_verification(f), "confirmed");
    CHECK_OWNED_STR(code_review_location(f), "core/connection.c:8");
    CHECK_OWNED_STR(code_review_usage(r), "unknown in / 0 out; Cost unknown; 2 unpriced turns (partial cost)");
    CHECK_OWNED_STR(code_review_counts(r), "2 findings: 0 critical, 1 high, 1 medium, 0 low; 1 pending verdicts");
    r = code_review_find(reviews, 20); f = code_review_finding(r, 99);
    CHECK_STR(code_review_verdict(f), "dismiss"); CHECK_STR(code_review_verification(f), "plausible");
    CHECK_OWNED_STR(code_review_location(f), "No location");
    CHECK_OWNED_STR(code_review_usage(r), "1240 in / 180 out; $0.0123 estimated; 1 unpriced turns (partial cost)");
    Json *changed = json_clone(code_review_find(reviews, 21));
    Json *critical = (Json *)json_at(json_get(changed, "findings"), 0); json_set_str(critical, "severity", "critical");
    CHECK_OWNED_STR(code_review_counts(changed), "2 findings: 1 critical, 0 high, 1 medium, 0 low; 1 pending verdicts");
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
void core_code_reviews_tests(void) {
    test_run("stored code review nested public contract and null semantics", stored_contract);
    test_run("code reviews malformed is distinct from empty", malformed_and_empty);
    test_run("code reviews latest twenty, unique stable IDs and required fields", limit_and_validation);
}
