#include "code_reviews.h"
#include "str.h"
#include "suites.h"
#include "test.h"
#include "fixtures/code_reviews.h"
#include <stdlib.h>

static void security_findings_are_the_ones_the_server_marked(void) {
    Json *j = json_parsez("[{\"id\":1,\"security\":true,\"type\":\"correctness\"},{\"id\":2,\"security\":false,\"type\":\"security\"},{\"id\":3}]");
    // The server's mark decides, not the free-text type.
    CHECK(code_review_finding_is_security(json_at(j, 0))); CHECK(!code_review_finding_is_security(json_at(j, 1)));
    CHECK(!code_review_finding_is_security(json_at(j, 2))); CHECK(!code_review_finding_is_security(NULL));
    json_free(j);
}
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
static void handling_and_its_actions(void) {
    Json *r = json_parsez("{\"id\":5,\"status\":\"completed\",\"handlingStatus\":\"pending\",\"findings\":[]}");
    CHECK_STR(code_review_handling(r), "pending"); CHECK_INT(code_review_handling_action(r), CODE_REVIEW_MARK_HANDLED);
    CHECK_STR(code_review_handling_title(CODE_REVIEW_MARK_HANDLED), "Mark handled");
    Json *a = code_review_handling_args("o/r", 12, 5, CODE_REVIEW_MARK_HANDLED);
    CHECK_INT(json_count(a), 4); CHECK_STR(json_str(json_get(a, "repo")), "o/r"); CHECK(json_num_or(json_get(a, "pr"), 0) == 12);
    CHECK(json_num_or(json_get(a, "reviewId"), 0) == 5); CHECK_STR(json_str(json_get(a, "handlingStatus")), "handled"); json_free(a);
    json_set_str(r, "handlingStatus", "handled");
    CHECK_INT(code_review_handling_action(r), CODE_REVIEW_REOPEN); CHECK_STR(code_review_handling_title(CODE_REVIEW_REOPEN), "Reopen");
    a = code_review_handling_args("o/r", 12, 5, CODE_REVIEW_REOPEN); CHECK_STR(json_str(json_get(a, "handlingStatus")), "pending"); json_free(a);
    // A failed review can be handled too; a running one is still writing its findings.
    json_set_str(r, "status", "failed"); CHECK_INT(code_review_handling_action(r), CODE_REVIEW_REOPEN); CHECK(code_review_can_edit_findings(r));
    json_set_str(r, "status", "running"); CHECK_INT(code_review_handling_action(r), CODE_REVIEW_HANDLING_NONE); CHECK(!code_review_can_edit_findings(r));
    // A server that does not track handling, or an unknown value, offers nothing.
    json_set_str(r, "status", "completed"); json_object_remove(r, "handlingStatus");
    CHECK(code_review_handling(r) == NULL); CHECK_INT(code_review_handling_action(r), CODE_REVIEW_HANDLING_NONE);
    json_set_str(r, "handlingStatus", "done"); CHECK(code_review_handling(r) == NULL); CHECK_INT(code_review_handling_action(r), CODE_REVIEW_HANDLING_NONE);
    CHECK(!code_review_can_edit_findings(NULL)); json_free(r);
}
static void answers_fold_into_the_list(void) {
    Json *payload = json_parsez(CODE_REVIEWS_FIXTURE), *reviews = code_reviews_parse(payload); json_free(payload);
    Json *review = json_parsez("{\"id\":20,\"status\":\"completed\",\"handlingStatus\":\"handled\",\"findings\":[{\"id\":99,\"severity\":\"low\",\"title\":\"T\"}]}");
    CHECK(code_reviews_replace_review(reviews, review)); json_free(review);
    CHECK_INT(json_count(reviews), 2); CHECK_STR(code_review_handling(code_review_find(reviews, 20)), "handled");
    CHECK_INT(code_review_id(json_at(reviews, 1)), 20);
    // A review not in the list, or a malformed answer, changes nothing.
    const char *bad[] = {"{\"id\":77,\"status\":\"completed\",\"findings\":[]}", "{\"id\":20,\"status\":\"bogus\",\"findings\":[]}", "{\"id\":20,\"status\":\"completed\"}", "null"};
    for (size_t i = 0; i < 4; i++) {
        review = json_parsez(bad[i]); CHECK(!code_reviews_replace_review(reviews, review)); json_free(review);
    }
    CHECK_STR(code_review_handling(code_review_find(reviews, 20)), "handled");
    Json *finding = json_parsez("{\"id\":102,\"severity\":\"medium\",\"title\":\"Obsolete\",\"shouldFix\":\"optional\",\"verification\":\"plausible\",\"comments\":\"Later.\"}");
    CHECK(code_reviews_replace_finding(reviews, 21, finding));
    const Json *f = code_review_finding(code_review_find(reviews, 21), 102);
    CHECK_STR(json_str(json_get(f, "shouldFix")), "optional"); CHECK_STR(json_str(json_get(f, "comments")), "Later.");
    CHECK_INT(json_count(json_get(code_review_find(reviews, 21), "findings")), 2);
    // The finding has to be in that review, and the answer readable.
    CHECK(!code_reviews_replace_finding(reviews, 20, finding)); CHECK(!code_reviews_replace_finding(reviews, 5, finding));
    json_free(finding); finding = json_parsez("{\"id\":102,\"severity\":\"severe\",\"title\":\"Obsolete\"}");
    CHECK(!code_reviews_replace_finding(reviews, 21, finding)); json_free(finding);
    CHECK(!code_reviews_replace_finding(NULL, 21, NULL));
    json_free(reviews);
}
static CodeReviewFindingEdit edit_of(const char *finding_json) {
    Json *finding = json_parsez(finding_json), *review = json_parsez("{\"id\":2,\"status\":\"completed\"}");
    CodeReviewFindingEdit e; code_review_edit_init(&e, review, finding);
    json_free(finding); json_free(review); return e;
}
static size_t verdict_index(const char *v) {
    for (size_t i = 0; i < CODE_REVIEW_VERDICT_CHOICES; i++) if (str_eq(code_review_verdict_choices[i], v)) return i;
    return CODE_REVIEW_VERDICT_CHOICES - 1;
}
static size_t evidence_index(const char *v) {
    for (size_t i = 0; i < CODE_REVIEW_VERIFICATION_CHOICES; i++) if (str_eq(code_review_verification_choices[i], v)) return i;
    return CODE_REVIEW_VERIFICATION_CHOICES - 1;
}
static void a_verdict_edit_sends_only_what_changed(void) {
    CodeReviewFindingEdit e = edit_of("{\"id\":21,\"shouldFix\":\"fix\",\"verification\":\"confirmed\",\"comments\":\"Reproduced.\"}");
    CHECK(!code_review_edit_dirty(&e)); CHECK(!code_review_edit_can_save(&e)); CHECK(code_review_edit_reason_hint(&e) == NULL);
    code_review_edit_choose_verdict(&e, verdict_index("optional"));
    // The verify step's reason for fixing it cannot stand for not fixing it.
    CHECK(code_review_edit_needs_reason(&e)); CHECK(!code_review_edit_has_reason(&e)); CHECK(!code_review_edit_can_save(&e));
    CHECK(code_review_edit_reason_hint(&e) != NULL);
    code_review_edit_set_reason(&e, " Not worth it here. \n"); CHECK(code_review_edit_can_save(&e));
    Json *a = code_review_edit_args(&e, "o/r", 9);
    // repo, pr, reviewId, findingId, shouldFix and comments: the evidence it kept is not sent.
    CHECK_INT(json_count(a), 6); CHECK_STR(json_str(json_get(a, "shouldFix")), "optional"); CHECK_STR(json_str(json_get(a, "comments")), "Not worth it here.");
    CHECK(json_num_or(json_get(a, "reviewId"), 0) == 2); CHECK(json_num_or(json_get(a, "findingId"), 0) == 21);
    json_free(a);
    // Clearing the verdict and the reason sends nulls.
    code_review_edit_choose_verdict(&e, verdict_index(NULL)); code_review_edit_set_reason(&e, "");
    CHECK(code_review_edit_can_save(&e));
    a = code_review_edit_args(&e, "o/r", 9);
    CHECK_INT(json_count(a), 6); CHECK(json_is_null(json_get(a, "shouldFix"))); CHECK(json_is_null(json_get(a, "comments")));
    char *body = json_serialize(a, true);
    CHECK_STR(body, "{\"comments\":null,\"findingId\":21,\"pr\":9,\"repo\":\"o/r\",\"reviewId\":2,\"shouldFix\":null}"); free(body);
    json_free(a);
    // An index out of range changes nothing.
    code_review_edit_choose_verdict(&e, CODE_REVIEW_VERDICT_CHOICES); CHECK(e.verdict == NULL);
    code_review_edit_choose_verification(&e, CODE_REVIEW_VERIFICATION_CHOICES); CHECK_STR(e.verification, "confirmed");
    code_review_edit_free(&e);
}
static void not_fixing_needs_the_evidence_the_outcome_asks(void) {
    CodeReviewFindingEdit e = edit_of("{\"id\":1,\"shouldFix\":\"fix\"}");
    code_review_edit_set_reason(&e, "Refuted by the guard on line 4.");
    // Optional takes either level of evidence.
    code_review_edit_choose_verdict(&e, verdict_index("optional"));
    CHECK(!code_review_edit_has_verification(&e)); CHECK(!code_review_edit_can_save(&e)); CHECK(code_review_edit_verification_hint(&e) != NULL);
    code_review_edit_choose_verification(&e, evidence_index("plausible"));
    CHECK(code_review_edit_can_save(&e)); CHECK(code_review_edit_verification_hint(&e) == NULL);
    // Dismissed takes only Confirmed.
    code_review_edit_choose_verdict(&e, verdict_index("dismiss"));
    CHECK(!code_review_edit_can_save(&e)); CHECK(code_review_edit_verification_hint(&e) != NULL);
    code_review_edit_choose_verification(&e, evidence_index("confirmed")); CHECK(code_review_edit_can_save(&e));
    // Fix and no verdict need none.
    code_review_edit_choose_verification(&e, evidence_index(NULL));
    code_review_edit_choose_verdict(&e, verdict_index("fix")); CHECK(code_review_edit_has_verification(&e));
    code_review_edit_choose_verdict(&e, verdict_index(NULL)); CHECK(code_review_edit_has_verification(&e));
    code_review_edit_free(&e);
    // The choices come in one order, None last.
    CHECK_STR(code_review_verdict_choices[0], "fix"); CHECK_STR(code_review_verdict_choices[2], "dismiss");
    CHECK(code_review_verdict_choices[CODE_REVIEW_VERDICT_CHOICES - 1] == NULL); CHECK(code_review_verification_choices[CODE_REVIEW_VERIFICATION_CHOICES - 1] == NULL);
    CHECK_STR(code_review_verification_choices[0], "confirmed");
}
static void an_earlier_ruling_may_be_reworded_as_it_stands(void) {
    CodeReviewFindingEdit e = edit_of("{\"id\":5,\"shouldFix\":\"dismiss\",\"verification\":\"plausible\",\"comments\":\"Refuted.\"}");
    // A dismissal stored as only plausible lacks the evidence, but its reason may still be reworded.
    CHECK(!code_review_edit_dirty(&e)); CHECK(!code_review_edit_has_verification(&e)); CHECK(code_review_edit_verification_hint(&e) != NULL);
    code_review_edit_set_reason(&e, "Refuted by the guard on line 4.");
    CHECK(code_review_edit_can_save(&e));
    Json *a = code_review_edit_args(&e, "o/r", 9); CHECK_INT(json_count(a), 5); CHECK(json_str(json_get(a, "comments")) != NULL); json_free(a);
    // Changing the ruling to something still short of the evidence is held back.
    code_review_edit_choose_verdict(&e, verdict_index("optional")); code_review_edit_choose_verification(&e, evidence_index(NULL));
    CHECK(!code_review_edit_can_save(&e));
    code_review_edit_choose_verdict(&e, verdict_index("dismiss")); code_review_edit_set_reason(&e, "Refuted.");
    code_review_edit_choose_verification(&e, evidence_index("confirmed"));
    CHECK(code_review_edit_dirty(&e)); CHECK(code_review_edit_can_save(&e));
    a = code_review_edit_args(&e, "o/r", 9);
    CHECK_INT(json_count(a), 5); CHECK_STR(json_str(json_get(a, "verification")), "confirmed");
    json_free(a); code_review_edit_free(&e);
}
static void saving_waits_for_a_stopped_review(void) {
    Json *payload = json_parsez(CODE_REVIEWS_FIXTURE), *reviews = code_reviews_parse(payload); json_free(payload);
    const Json *older = code_review_find(reviews, 20);
    CodeReviewFindingEdit e; code_review_edit_init(&e, older, code_review_finding(older, 99));
    CHECK_STR(e.verdict, "dismiss"); CHECK_STR(e.verification, "plausible"); CHECK_STR(e.saved_reason, "Dismissed by verification, not marked fixed.");
    code_review_edit_choose_verification(&e, evidence_index("confirmed"));
    CHECK(code_reviews_allow_saving(reviews, &e));
    // A rerun rewrites the findings, so a review running again refuses the save; so does a list without the finding.
    Json *running = (Json *)code_review_find(reviews, 20); json_set_str(running, "status", "running");
    CHECK(code_review_edit_can_save(&e)); CHECK(!code_reviews_allow_saving(reviews, &e));
    json_set_str(running, "status", "completed"); e.finding_id = 98; CHECK(!code_reviews_allow_saving(reviews, &e));
    e.finding_id = 99; e.review_id = 1; CHECK(!code_reviews_allow_saving(reviews, &e));
    code_review_edit_free(&e); json_free(reviews);
}
void core_code_reviews_tests(void) {
    test_run("a stopped review is marked handled or reopened only where the server tracks handling", handling_and_its_actions);
    test_run("handling and verdict answers fold into the list only where they belong", answers_fold_into_the_list);
    test_run("a verdict edit needs a reason of its own and sends only what changed", a_verdict_edit_sends_only_what_changed);
    test_run("not fixing a finding needs the evidence the review outcome asks", not_fixing_needs_the_evidence_the_outcome_asks);
    test_run("a ruling stored before evidence was asked may be reworded as it stands", an_earlier_ruling_may_be_reworded_as_it_stands);
    test_run("a verdict save waits for its review to stop and its finding to be there", saving_waits_for_a_stopped_review);
    test_run("review totals sum each review once and preserve zero, unknown, estimated and unpriced values", usage_totals_preserve_unknowns);
    test_run("terminal null review verdicts remain inconclusive and only running nulls are pending", terminal_null_verdicts);
    test_run("stored code review nested public contract and null semantics", stored_contract);
    test_run("code reviews malformed is distinct from empty", malformed_and_empty);
    test_run("code reviews latest twenty, unique stable IDs and required fields", limit_and_validation);
    test_run("a finding counts as a security one only when the server marked it", security_findings_are_the_ones_the_server_marked);
}
