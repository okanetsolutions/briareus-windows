#include "review_approvals.h"
#include "str.h"
#include "suites.h"
#include "test.h"
#include <stdlib.h>
#include <string.h>

// A requirement with every part: a risk set by a rule and a threshold, a pending reason and a note, an approval that
// counts, one from a bot on GitHub that does not, and one withdrawn with why.
static const char *REQUIREMENT = "{\"enabled\":true,\"result\":\"pending\",\"headSha\":\"abcdef1234567890\",\"baseRef\":\"main\",\"tier\":\"high\","
    "\"required\":2,\"valid\":1,\"missing\":1,\"reviewCurrent\":true,\"explanation\":\"Needs two people.\","
    "\"risk\":{\"tier\":\"high\",\"reasons\":[{\"code\":\"sensitive_path\",\"tier\":\"high\",\"message\":\"migrations/** matched 2 files\"},"
    "{\"code\":\"size_lines\",\"tier\":\"medium\"},{\"tier\":\"low\",\"message\":\"no code\"}]},"
    "\"reasons\":[{\"code\":\"security_signal\",\"effect\":\"note\",\"message\":\"Raised by security\"},"
    "{\"code\":\"approvals_missing\",\"effect\":\"pending\",\"message\":\"One more approval\"}],"
    "\"approvals\":["
    "{\"id\":1,\"source\":\"briareus\",\"by\":{\"userId\":7,\"name\":\"ana\",\"githubLogin\":\"ana-gh\"},\"headSha\":\"abcdef1234567890\","
    "\"at\":\"2026-11-12T10:00:00Z\",\"note\":\"Looks right\",\"counted\":true,\"reason\":null},"
    "{\"id\":2,\"source\":\"github\",\"by\":{\"userId\":null,\"name\":null,\"githubLogin\":\"bot\"},\"headSha\":\"0000000\",\"counted\":false,"
    "\"reason\":\"bot_account\",\"message\":\"bot is a bot account\"},"
    "{\"id\":3,\"source\":\"briareus\",\"by\":{\"userId\":8,\"name\":\"leo\"},\"headSha\":\"1111111\",\"withdrawnAt\":\"2026-11-12T11:00:00Z\","
    "\"withdrawNote\":\"pushed a fix\",\"counted\":false,\"reason\":\"withdrawn\"},"
    "{\"source\":\"briareus\",\"by\":{\"userId\":9},\"headSha\":\"abc\",\"counted\":true}]}";

static void parsed(const char *text, ReviewApprovals *a) { Json *j = json_parsez(text); review_approvals_parse(j, a); json_free(j); }

static void the_requirement_reads_its_approvals_and_risk(void) {
    ReviewApprovals a; parsed(REQUIREMENT, &a);
    CHECK(a.present); CHECK(a.enabled); CHECK(a.review_current); CHECK_STR(a.head_sha, "abcdef1234567890");
    // An entry without a code, or an approval without an id, is left out.
    CHECK_INT(a.risk_count, 2); CHECK_STR(a.risk[0].code, "sensitive_path"); CHECK_STR(a.risk[0].tier, "high");
    CHECK_STR(a.risk[0].message, "migrations/** matched 2 files"); CHECK_STR(a.risk[1].tier, "medium"); CHECK_STR(a.risk[1].message, "size lines");
    CHECK_INT(a.count, 3); CHECK(a.items[0].id == 1 && a.items[1].id == 2 && a.items[2].id == 3);
    const ReviewReason *listed[2] = {0}; review_reasons_listed(&a.reasons, listed);
    CHECK_STR(listed[0]->code, "approvals_missing"); CHECK_STR(listed[1]->code, "security_signal");
    const ReviewApproval *first = &a.items[0], *bot = &a.items[1], *gone = &a.items[2];
    CHECK(first->user_id == 7); CHECK_OWNED_STR(review_approval_who(first), "ana"); CHECK_STR(first->note, "Looks right");
    CHECK_STR(first->at, "2026-11-12T10:00:00Z"); CHECK(first->counted); CHECK_INT(review_approval_tone(first), REVIEW_TONE_OK);
    CHECK_OWNED_STR(review_approval_state(first), "Counts"); CHECK_OWNED_STR(review_approval_detail(first), "abcdef1");
    CHECK(first->withdrawn_at == NULL); CHECK(review_approval_withdraw_line(first) == NULL);
    CHECK(bot->from_github); CHECK(bot->user_id == 0); CHECK_OWNED_STR(review_approval_who(bot), "@bot"); CHECK_STR(bot->reason, "bot_account");
    CHECK_OWNED_STR(review_approval_state(bot), "bot is a bot account"); CHECK_INT(review_approval_tone(bot), REVIEW_TONE_MUTED);
    CHECK_OWNED_STR(review_approval_detail(bot), "on GitHub \xC2\xB7 0000000");
    CHECK(gone->withdrawn); CHECK(!gone->counted); CHECK_STR(gone->withdrawn_at, "2026-11-12T11:00:00Z");
    CHECK_OWNED_STR(review_approval_state(gone), "Withdrawn"); CHECK_OWNED_STR(review_approval_withdraw_line(gone), "Withdrawn: pushed a fix");
    review_approvals_free(&a); CHECK(a.items == NULL && a.count == 0 && a.risk == NULL);
    review_approvals_free(NULL);
    // Without the server's words a reason reads as its code, and without either as not counting; nobody named is Someone.
    ReviewApproval bare = { .id = 4, .reason = "head_moved" };
    CHECK_OWNED_STR(review_approval_state(&bare), "head moved"); CHECK_OWNED_STR(review_approval_who(&bare), "Someone");
    bare.reason = NULL; CHECK_OWNED_STR(review_approval_state(&bare), "Does not count"); CHECK_OWNED_STR(review_approval_detail(&bare), "");
    // Anything but a requirement with a result is not read.
    parsed("{\"enabled\":true}", &a); CHECK(!a.present); review_approvals_free(&a);
    parsed("[]", &a); CHECK(!a.present); review_approvals_free(&a);
}
static void withdraw_is_offered_on_ones_own_approvals_recorded_here(void) {
    ReviewApprovals a; parsed(REQUIREMENT, &a);
    bool ana[] = {true, false, false}, other[] = {false, false, false}, unknown[] = {true, false, false};
    for (size_t i = 0; i < 3; i++) {
        CHECK(review_approval_can_withdraw(&a.items[i], 7) == ana[i]);
        CHECK(review_approval_can_withdraw(&a.items[i], 9) == other[i]);
        // Without the token's user every live approval recorded here is offered; the server refuses another person's.
        CHECK(review_approval_can_withdraw(&a.items[i], 0) == unknown[i]);
    }
    review_approvals_free(&a);
}
static void approve_waits_for_a_reviewed_head_and_once_a_person(void) {
    ReviewApprovals a; parsed(REQUIREMENT, &a);
    CHECK(!review_approvals_can_approve(&a, 7));   // ana's approval already counts for this head
    CHECK(review_approvals_can_approve(&a, 8)); CHECK(review_approvals_can_approve(&a, 0));
    a.review_current = false; CHECK(!review_approvals_can_approve(&a, 8));
    a.review_current = true; char *head = a.head_sha; a.head_sha = NULL; CHECK(!review_approvals_can_approve(&a, 8));
    a.head_sha = head; char *result = a.result; a.result = "not_required"; CHECK(!review_approvals_can_approve(&a, 8));
    a.result = "satisfied"; CHECK(review_approvals_can_approve(&a, 8));
    a.enabled = false; CHECK(!review_approvals_can_approve(&a, 8));
    a.result = result; review_approvals_free(&a);
    // An approval of ana's that counts for another head leaves Approve offered for this one.
    parsed("{\"enabled\":true,\"result\":\"pending\",\"headSha\":\"new\",\"reviewCurrent\":true,\"required\":1,"
           "\"approvals\":[{\"id\":1,\"source\":\"briareus\",\"by\":{\"userId\":7},\"headSha\":\"old\",\"counted\":true}]}", &a);
    CHECK(review_approvals_can_approve(&a, 7));
    review_approvals_free(&a);
}
static void the_bodies_name_the_head_and_keep_a_note_only_when_given(void) {
    Json *args = review_approval_approve_args("o/r", 9, "abc", "  \n");
    char *text = json_serialize(args, true); CHECK_STR(text, "{\"headSha\":\"abc\",\"pr\":9,\"repo\":\"o/r\"}"); free(text); json_free(args);
    args = review_approval_approve_args("o/r", 9, "abc", " ok \n"); CHECK_STR(json_str(json_get(args, "note")), "ok"); json_free(args);
    args = review_approval_withdraw_args("o/r", 9, 1, "pushed a fix");
    text = json_serialize(args, true); CHECK_STR(text, "{\"approvalId\":1,\"note\":\"pushed a fix\",\"pr\":9,\"repo\":\"o/r\"}"); free(text); json_free(args);
    args = review_approval_withdraw_args("o/r", 9, 1, NULL);
    text = json_serialize(args, true); CHECK_STR(text, "{\"approvalId\":1,\"pr\":9,\"repo\":\"o/r\"}"); free(text); json_free(args);
}
static void a_refusal_saying_the_head_or_review_moved_on_reads_again(void) {
    CHECK(review_approval_rereads("head_moved")); CHECK(review_approval_rereads("review_not_current")); CHECK(review_approval_rereads("review_running"));
    CHECK(!review_approval_rereads("github_account_required")); CHECK(!review_approval_rereads("own_change")); CHECK(!review_approval_rereads(NULL));
}
static void what_approving_means_names_the_reviewed_head(void) {
    ReviewApprovals a; parsed(REQUIREMENT, &a);
    char *said = review_approvals_approve_consequence(&a);
    CHECK(strstr(said, "abcdef1") != NULL); CHECK(strstr(said, "abcdef12") == NULL); free(said);
    review_approvals_free(&a);
}
static Json *state_of(const char *answer, ReviewOutcomeSource source, const Json *previous) {
    Json *j = json_parsez(answer), *state = review_outcome_state(j, source, previous);
    json_free(j); return state;
}
static void an_approval_answer_replaces_the_outcome_and_keeps_the_rounds(void) {
    Json *kept = state_of("{\"reviewOutcome\":{\"outcome\":\"passed\",\"approvals\":{\"enabled\":true,\"result\":\"pending\",\"required\":1,\"valid\":0,\"missing\":1}},"
                          "\"reviewRounds\":{\"limit\":3,\"used\":1}}", REVIEW_OUTCOME_FROM_READ, NULL);
    Json *answered = state_of("{\"approval\":{\"id\":5},\"reviewOutcome\":{\"outcome\":\"passed\",\"ready\":true,"
                              "\"approvals\":{\"enabled\":true,\"result\":\"satisfied\",\"required\":1,\"valid\":1,\"missing\":0}}}", REVIEW_OUTCOME_FROM_APPROVAL, kept);
    ReviewOutcome o; CHECK(review_outcome_parse(answered, &o));
    CHECK(o.ready); CHECK_STR(o.approvals.result, "satisfied"); CHECK_INT(o.rounds.used, 1);
    review_outcome_free(&o); json_free(answered);
    // An answer carrying the requirement alone puts it in the kept outcome.
    answered = state_of("{\"approvals\":{\"enabled\":true,\"result\":\"satisfied\",\"required\":1,\"valid\":1,\"missing\":0}}", REVIEW_OUTCOME_FROM_APPROVAL, kept);
    CHECK(review_outcome_parse(answered, &o));
    CHECK_INT(o.outcome, REVIEW_OUTCOME_PASSED); CHECK_INT(o.approvals.valid, 1); CHECK_INT(o.rounds.limit, 3);
    review_outcome_free(&o); json_free(answered);
    // An answer with neither changes nothing, and a requirement alone over nothing kept stays nothing.
    answered = state_of("{\"approval\":{\"id\":5}}", REVIEW_OUTCOME_FROM_APPROVAL, kept);
    CHECK(json_equal(answered, kept)); json_free(answered);
    CHECK(state_of("{\"approvals\":{\"enabled\":true,\"result\":\"satisfied\"}}", REVIEW_OUTCOME_FROM_APPROVAL, NULL) == NULL);
    // An outcome over nothing kept is kept, without rounds.
    answered = state_of("{\"reviewOutcome\":{\"outcome\":\"pending\"}}", REVIEW_OUTCOME_FROM_APPROVAL, NULL);
    CHECK(review_outcome_parse(answered, &o)); CHECK(!o.rounds.present); review_outcome_free(&o); json_free(answered);
    json_free(kept);
}
static void each_write_names_what_its_answer_replaces(void) {
    CHECK_INT(review_outcome_source_after("approve_pull"), REVIEW_OUTCOME_FROM_APPROVAL);
    CHECK_INT(review_outcome_source_after("withdraw_pull_approval"), REVIEW_OUTCOME_FROM_APPROVAL);
    CHECK_INT(review_outcome_source_after("update_code_review_finding"), REVIEW_OUTCOME_FROM_VERDICT);
    CHECK_INT(review_outcome_source_after("update_code_review"), REVIEW_OUTCOME_FROM_OTHER);
}
static void approvals_on_record_stay_shown_when_the_tier_asks_for_none(void) {
    ReviewApprovals a;
    parsed("{\"enabled\":true,\"result\":\"not_required\",\"tier\":\"low\",\"required\":0,\"valid\":0,\"missing\":0,"
           "\"approvals\":[{\"id\":1,\"source\":\"briareus\",\"by\":{\"userId\":7},\"headSha\":\"abc\",\"counted\":false,\"reason\":\"head_moved\"}]}", &a);
    CHECK(review_approvals_shown(&a)); CHECK_INT(review_approvals_tone(&a), REVIEW_TONE_MUTED);
    CHECK_OWNED_STR(review_approvals_line(&a), "None needed \xC2\xB7 low risk"); CHECK(!review_approvals_can_approve(&a, 8));
    review_approvals_free(&a);
    // Nothing asked and nothing on record: hidden, as before.
    parsed("{\"enabled\":true,\"result\":\"not_required\",\"tier\":\"low\",\"required\":0,\"approvals\":[]}", &a);
    CHECK(!review_approvals_shown(&a)); review_approvals_free(&a);
    // Without a policy the server lists approvals as not counting; still hidden.
    parsed("{\"enabled\":false,\"result\":\"not_required\",\"approvals\":[{\"id\":1,\"source\":\"briareus\",\"by\":{},\"headSha\":\"abc\",\"counted\":false}]}", &a);
    CHECK(!review_approvals_shown(&a)); review_approvals_free(&a);
}
void core_review_approvals_tests(void) {
    test_run("an approval requirement reads every approval on record and what set its risk", the_requirement_reads_its_approvals_and_risk);
    test_run("withdraw is offered on one's own approvals recorded here", withdraw_is_offered_on_ones_own_approvals_recorded_here);
    test_run("approve waits for a reviewed head and is offered once a person", approve_waits_for_a_reviewed_head_and_once_a_person);
    test_run("approval bodies name the head and keep a note only when given", the_bodies_name_the_head_and_keep_a_note_only_when_given);
    test_run("an approval refusal saying the head or review moved on reads the outcome again", a_refusal_saying_the_head_or_review_moved_on_reads_again);
    test_run("what approving means names the reviewed head", what_approving_means_names_the_reviewed_head);
    test_run("an approval answer replaces the outcome, or only its approvals, and keeps the rounds", an_approval_answer_replaces_the_outcome_and_keeps_the_rounds);
    test_run("each review write names what its answer replaces", each_write_names_what_its_answer_replaces);
    test_run("approvals on record stay shown when the tier asks for none", approvals_on_record_stay_shown_when_the_tier_asks_for_none);
}
