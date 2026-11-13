#include "review_approvals.h"
#include "str.h"
#include "suites.h"
#include "test.h"
#include <stdlib.h>
#include <string.h>

// An answer with every part: an enforced outcome GitHub does not carry yet, a required security pass that did not run,
// pending approvals and rounds used to their limit.
static const char *FULL = "{\"codeReviews\":[],\"reviewOutcome\":{\"outcome\":\"changes_required\",\"stale\":false,\"ready\":false,\"enforced\":true,"
    "\"reasons\":[{\"code\":\"approved_label_ignored\",\"effect\":\"note\",\"message\":\"Label ignored\"},"
    "{\"code\":\"fix_required\",\"effect\":\"changes_required\",\"message\":\"Fix the guard\",\"findingId\":4},"
    "{\"code\":\"requirement_unmet\",\"effect\":\"incomplete\",\"message\":\"Security pass missing\"}],"
    "\"status\":{\"published\":false,\"state\":\"failure\"},"
    "\"security\":{\"policy\":\"required\",\"result\":\"not_run\",\"blocking\":true,\"reasons\":[{\"code\":\"security_not_run\",\"effect\":\"incomplete\",\"message\":\"Not run\"}]},"
    "\"approvals\":{\"enabled\":true,\"result\":\"pending\",\"tier\":\"high\",\"required\":2,\"valid\":1,\"missing\":1,\"explanation\":\"Needs two people.\"}},"
    "\"reviewRounds\":{\"limit\":3,\"used\":3,\"remaining\":0,\"limitReached\":true,\"continuations\":1}}";

static Json *state_of(const char *answer, ReviewOutcomeSource source, const Json *previous) {
    Json *j = json_parsez(answer), *state = review_outcome_state(j, source, previous);
    json_free(j); return state;
}
static bool parse_text(const char *state, ReviewOutcome *o) {
    Json *j = json_parsez(state); bool ok = review_outcome_parse(j, o); json_free(j); return ok;
}

static void an_answer_reads_every_part(void) {
    Json *state = state_of(FULL, REVIEW_OUTCOME_FROM_READ, NULL);
    ReviewOutcome o; CHECK(review_outcome_parse(state, &o));
    CHECK_INT(o.outcome, REVIEW_OUTCOME_CHANGES_REQUIRED); CHECK(!o.stale); CHECK(!o.ready); CHECK(o.enforced); CHECK(!o.published);
    CHECK_INT(o.reasons.count, 3); CHECK_STR(o.reasons.items[0].code, "approved_label_ignored"); CHECK_STR(o.reasons.items[2].code, "requirement_unmet");
    CHECK(o.security.present); CHECK_STR(o.security.policy, "required"); CHECK_STR(o.security.result, "not_run");
    CHECK(o.security.blocking); CHECK(review_security_shown(&o.security)); CHECK_INT(o.security.reasons.count, 1);
    CHECK(review_approvals_shown(&o.approvals)); CHECK_INT(o.approvals.required, 2); CHECK_INT(o.approvals.valid, 1); CHECK_INT(o.approvals.missing, 1);
    CHECK_STR(o.approvals.tier, "high"); CHECK_STR(o.approvals.explanation, "Needs two people.");
    CHECK(o.rounds.present); CHECK_INT(o.rounds.limit, 3); CHECK_INT(o.rounds.used, 3); CHECK(o.rounds.limit_reached); CHECK_INT(o.rounds.continuations, 1);
    // The reasons that block come before the notes, each tone after its effect.
    const ReviewReason *listed[3] = {0}; review_reasons_listed(&o.reasons, listed);
    CHECK_STR(listed[0]->code, "fix_required"); CHECK_STR(listed[1]->code, "requirement_unmet"); CHECK_STR(listed[2]->code, "approved_label_ignored");
    CHECK_INT(review_reason_tone(listed[0]), REVIEW_TONE_DANGER); CHECK_INT(review_reason_tone(listed[1]), REVIEW_TONE_WARN);
    CHECK_INT(review_reason_tone(listed[2]), REVIEW_TONE_MUTED); CHECK(review_reason_is_note(listed[2]));
    CHECK_OWNED_STR(review_rounds_line(&o.rounds), "3 of 3 rounds \xC2\xB7 1 granted past the limit");
    CHECK_INT(review_rounds_tone(&o.rounds), REVIEW_TONE_WARN);
    review_outcome_free(&o); json_free(state);
}
static void every_outcome_has_its_tone(void) {
    CHECK_INT(review_outcome_tone(REVIEW_OUTCOME_PENDING), REVIEW_TONE_RUNNING); CHECK_INT(review_outcome_tone(REVIEW_OUTCOME_INCOMPLETE), REVIEW_TONE_WARN);
    CHECK_INT(review_outcome_tone(REVIEW_OUTCOME_CHANGES_REQUIRED), REVIEW_TONE_DANGER); CHECK_INT(review_outcome_tone(REVIEW_OUTCOME_PASSED), REVIEW_TONE_OK);
    ReviewOutcome o; CHECK(parse_text("{\"reviewOutcome\":{\"outcome\":\"passed\"}}", &o)); CHECK_INT(o.outcome, REVIEW_OUTCOME_PASSED);
    CHECK_STR(review_outcome_title(o.outcome), "Passed"); review_outcome_free(&o);
}
static void no_outcome_from_an_older_server_or_an_unknown_one(void) {
    Json *state = state_of("{\"codeReviews\":[]}", REVIEW_OUTCOME_FROM_READ, NULL);
    CHECK(state == NULL);
    ReviewOutcome o; CHECK(!review_outcome_parse(NULL, &o)); CHECK_INT(o.reasons.count, 0);
    CHECK(!parse_text("{\"reviewOutcome\":{\"outcome\":\"bogus\"}}", &o));
    CHECK(!parse_text("{\"reviewOutcome\":[],\"reviewRounds\":{\"used\":1}}", &o));
    review_outcome_free(&o);
}
static void a_verdict_save_replaces_the_outcome_and_keeps_the_rounds(void) {
    Json *before = state_of(FULL, REVIEW_OUTCOME_FROM_READ, NULL);
    Json *saved = state_of("{\"finding\":{\"id\":4},\"reviewOutcome\":{\"outcome\":\"passed\",\"ready\":true}}", REVIEW_OUTCOME_FROM_VERDICT, before);
    ReviewOutcome o; CHECK(review_outcome_parse(saved, &o));
    CHECK_INT(o.outcome, REVIEW_OUTCOME_PASSED); CHECK(o.ready); CHECK(review_outcome_ready_note(&o) != NULL);
    CHECK_INT(o.rounds.used, 3); review_outcome_free(&o); json_free(saved);
    // A handling save answers neither: everything is kept.
    Json *kept = state_of("{\"review\":{\"id\":1}}", REVIEW_OUTCOME_FROM_OTHER, before);
    CHECK(json_equal(kept, before)); json_free(kept);
    CHECK(review_outcome_state(NULL, REVIEW_OUTCOME_FROM_OTHER, NULL) == NULL);
    // A verdict save the server could not compute an outcome for leaves none standing, and keeps the rounds.
    Json *uncomputed = state_of("{\"finding\":{\"id\":4}}", REVIEW_OUTCOME_FROM_VERDICT, before);
    CHECK(!review_outcome_parse(uncomputed, &o)); CHECK(json_equal(json_get(uncomputed, "reviewRounds"), json_get(before, "reviewRounds")));
    json_free(uncomputed);
    // A read replaces both: what it lacks is gone.
    Json *reread = state_of("{\"reviewOutcome\":{\"outcome\":\"pending\"}}", REVIEW_OUTCOME_FROM_READ, before);
    CHECK(review_outcome_parse(reread, &o)); CHECK(!o.rounds.present); review_outcome_free(&o);
    json_free(reread); json_free(before);
}
static void what_is_hidden_when_nothing_is_asked(void) {
    ReviewOutcome o;
    CHECK(parse_text("{\"reviewOutcome\":{\"outcome\":\"passed\",\"enforced\":false,\"stale\":false,\"security\":{\"policy\":\"off\",\"result\":\"not_required\"},"
                     "\"approvals\":{\"enabled\":false,\"result\":\"not_required\"}}}", &o));
    CHECK(o.security.present); CHECK(!review_security_shown(&o.security)); CHECK(!review_approvals_shown(&o.approvals)); CHECK(!o.rounds.present);
    CHECK(review_outcome_status_note(&o) == NULL); CHECK(review_outcome_stale_note(&o) == NULL); CHECK(review_outcome_ready_note(&o) == NULL);
    review_outcome_free(&o);
    CHECK(parse_text("{\"reviewOutcome\":{\"outcome\":\"pending\",\"stale\":true,\"enforced\":true,\"status\":{\"published\":true}}}", &o));
    CHECK(review_outcome_stale_note(&o) != NULL); CHECK(review_outcome_status_note(&o) == NULL);
    review_outcome_free(&o);
}
static void tones_and_lines_follow_the_results(void) {
    const char *results[] = { "passed", "failed", "incomplete", "not_run", "pending" };
    const ReviewTone tones[] = { REVIEW_TONE_OK, REVIEW_TONE_DANGER, REVIEW_TONE_WARN, REVIEW_TONE_WARN, REVIEW_TONE_RUNNING };
    for (size_t i = 0; i < 5; i++) {
        ReviewSecurity s = { .present = true, .policy = "advisory", .result = (char *)results[i] };
        CHECK_INT(review_security_tone(&s), tones[i]);
    }
    // A required pass reads apart from an advisory one with the same result, and a blocking one counts as required.
    ReviewSecurity required = { .present = true, .policy = "required", .result = "failed" }, advisory = { .present = true, .policy = "advisory", .result = "failed" };
    ReviewSecurity blocking = { .present = true, .blocking = true, .policy = "advisory", .result = "failed" }, off = { .present = true, .policy = "off", .result = "failed" };
    CHECK_OWNED_STR(review_security_line(&required), "Failed \xC2\xB7 required"); CHECK_OWNED_STR(review_security_line(&advisory), "Failed \xC2\xB7 advisory");
    CHECK_OWNED_STR(review_security_line(&blocking), "Failed \xC2\xB7 required"); CHECK_OWNED_STR(review_security_line(&off), "Failed \xC2\xB7 off");
    ReviewApprovals a = { .present = true, .enabled = true, .result = "satisfied", .required = 2, .valid = 1 };
    CHECK_INT(review_approvals_tone(&a), REVIEW_TONE_OK); CHECK_OWNED_STR(review_approvals_line(&a), "1 of 2 approvals");
    a.result = "pending"; a.tier = "high"; a.required = 1;
    CHECK_INT(review_approvals_tone(&a), REVIEW_TONE_WARN); CHECK_OWNED_STR(review_approvals_line(&a), "1 of 1 approval \xC2\xB7 high risk");
    ReviewRounds r = { .present = true, .used = 2, .limit = 3 };
    CHECK_INT(review_rounds_tone(&r), REVIEW_TONE_MUTED); CHECK_OWNED_STR(review_rounds_line(&r), "2 of 3 rounds");
    r.limit = 0; CHECK_OWNED_STR(review_rounds_line(&r), "2 rounds");
    r.used = 1; CHECK_OWNED_STR(review_rounds_line(&r), "1 round");
    ReviewOutcome o; CHECK(!parse_text("{\"reviewOutcome\":null,\"reviewRounds\":{\"limit\":3}}", &o));
    CHECK(parse_text("{\"reviewOutcome\":{\"outcome\":\"pending\"},\"reviewRounds\":{\"limit\":3}}", &o)); CHECK(!o.rounds.present); review_outcome_free(&o);
}
static void a_reason_without_effect_or_message_still_says_something(void) {
    ReviewOutcome o;
    CHECK(parse_text("{\"reviewOutcome\":{\"outcome\":\"incomplete\",\"reasons\":[{\"code\":\"label_check_failed\"},{\"effect\":\"pending\"},"
                     "{\"code\":\"head_moved\",\"effect\":\"pending\",\"message\":\"Moved\"}]}}", &o));
    CHECK_INT(o.reasons.count, 2);
    CHECK_STR(o.reasons.items[0].code, "label_check_failed"); CHECK(review_reason_is_note(&o.reasons.items[0]));
    CHECK_STR(o.reasons.items[0].message, "label check failed");
    CHECK_STR(o.reasons.items[1].message, "Moved"); CHECK_INT(review_reason_tone(&o.reasons.items[1]), REVIEW_TONE_RUNNING);
    review_outcome_free(&o);
}
static void the_notes_follow_the_rounds_and_the_status_write(void) {
    ReviewOutcome open, spent;
    CHECK(parse_text("{\"reviewOutcome\":{\"outcome\":\"pending\",\"stale\":true},\"reviewRounds\":{\"used\":1,\"limit\":3}}", &open));
    CHECK(parse_text("{\"reviewOutcome\":{\"outcome\":\"incomplete\",\"stale\":true},\"reviewRounds\":{\"used\":3,\"limit\":3,\"limitReached\":true}}", &spent));
    CHECK(review_outcome_stale_note(&open) && review_outcome_stale_note(&spent));
    CHECK(!str_eq(review_outcome_stale_note(&open), review_outcome_stale_note(&spent)));
    review_outcome_free(&open); review_outcome_free(&spent);
    // An unpublished status: not yet, retried after a refusal, or refused for good, each with GitHub's reason.
    const char *states[] = {
        "{\"reviewOutcome\":{\"outcome\":\"passed\",\"enforced\":true,\"status\":{\"published\":false}}}",
        "{\"reviewOutcome\":{\"outcome\":\"passed\",\"enforced\":true,\"status\":{\"published\":false,\"error\":\"Resource not accessible\",\"retryAt\":\"2026-11-12T15:00:00Z\"}}}",
        "{\"reviewOutcome\":{\"outcome\":\"passed\",\"enforced\":true,\"status\":{\"published\":false,\"error\":\"Resource not accessible\",\"retryAt\":null}}}",
    };
    char *notes[3];
    for (size_t i = 0; i < 3; i++) {
        ReviewOutcome o; CHECK(parse_text(states[i], &o));
        CHECK((o.status_error != NULL) == (i > 0)); CHECK(o.status_retried == (i == 1));
        notes[i] = review_outcome_status_note(&o); CHECK(notes[i] != NULL);
        if (i) CHECK(notes[i] && strstr(notes[i], "Resource not accessible"));
        review_outcome_free(&o);
    }
    CHECK(!str_eq(notes[0], notes[1]) && !str_eq(notes[1], notes[2]) && !str_eq(notes[0], notes[2]));
    for (size_t i = 0; i < 3; i++) free(notes[i]);
}
static void a_security_pass_reads_its_policy_and_reasons(void) {
    ReviewOutcome o;
    CHECK(parse_text("{\"reviewOutcome\":{\"outcome\":\"passed\",\"security\":{\"result\":\"failed\",\"reasons\":[{\"code\":\"n\",\"effect\":\"note\",\"message\":\"N\"},"
                     "{\"code\":\"f\",\"effect\":\"failed\",\"message\":\"F\"}]}}}", &o));
    // A policy left out reads as off.
    CHECK_STR(o.security.policy, "off");
    const ReviewReason *listed[2] = {0}; review_reasons_listed(&o.security.reasons, listed);
    CHECK_STR(listed[0]->code, "f"); CHECK_STR(listed[1]->code, "n"); CHECK_INT(review_reason_tone(listed[0]), REVIEW_TONE_DANGER);
    review_outcome_free(&o);
    // A security pass or approvals without a result are not read.
    CHECK(parse_text("{\"reviewOutcome\":{\"outcome\":\"passed\",\"security\":{\"policy\":\"required\"},\"approvals\":{\"enabled\":true}}}", &o));
    CHECK(!o.security.present); CHECK(!o.approvals.present); CHECK(!review_security_shown(&o.security)); CHECK(!review_approvals_shown(&o.approvals));
    review_outcome_free(&o);
}
void core_review_outcome_tests(void) {
    test_run("a review outcome answer reads its outcome, reasons, security pass, approvals and rounds", an_answer_reads_every_part);
    test_run("every review outcome has its own tone and title", every_outcome_has_its_tone);
    test_run("an older server or an unknown outcome leaves none", no_outcome_from_an_older_server_or_an_unknown_one);
    test_run("a verdict save replaces the outcome and keeps the rounds; a read replaces both; other saves keep both", a_verdict_save_replaces_the_outcome_and_keeps_the_rounds);
    test_run("the security pass, approvals, rounds and notes stay hidden when nothing is asked", what_is_hidden_when_nothing_is_asked);
    test_run("security, approval and round tones and lines tell apart what they read", tones_and_lines_follow_the_results);
    test_run("a reason without effect or message is a note that still says something", a_reason_without_effect_or_message_still_says_something);
    test_run("the stale and status notes follow the rounds and the status write", the_notes_follow_the_rounds_and_the_status_write);
    test_run("a security pass reads its policy, as sent or off, and its own reasons", a_security_pass_reads_its_policy_and_reasons);
}
