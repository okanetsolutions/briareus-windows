#include "review_approvals.h"
#include "str.h"
#include "suites.h"
#include "test.h"
#include <stdlib.h>
#include <string.h>

// A high-risk requirement waiting on one more person: user 7's approval counts, user 8's approved an earlier head, user
// 7's earlier one was withdrawn, and one read from GitHub does not count.
#define REQUIREMENT "{\"enabled\":true,\"result\":\"pending\",\"headSha\":\"abcdef1234567\",\"baseRef\":\"main\",\"tier\":\"high\"," \
    "\"required\":2,\"valid\":1,\"missing\":1,\"reviewCurrent\":true,\"explanation\":\"High risk needs two people.\"," \
    "\"risk\":{\"tier\":\"high\",\"reasons\":[{\"code\":\"sensitive_path\",\"tier\":\"high\",\"message\":\"migrations/** matched 2 files\"},{\"code\":\"size_lines\"},{\"tier\":\"low\"}]}," \
    "\"reasons\":[{\"code\":\"github_approvals_unread\",\"effect\":\"note\",\"message\":\"GitHub unread\"},{\"code\":\"approvals_missing\",\"effect\":\"pending\",\"message\":\"One more\"}]," \
    "\"approvals\":[" \
    "{\"id\":1,\"source\":\"briareus\",\"by\":{\"userId\":7,\"name\":\"Ada\"},\"headSha\":\"0000000aaaa\",\"withdrawnAt\":\"2026-11-12T10:00:00Z\",\"withdrawNote\":\"Too early\",\"counted\":false,\"reason\":\"withdrawn\"}," \
    "{\"id\":2,\"source\":\"briareus\",\"by\":{\"userId\":7,\"name\":\"Ada\"},\"headSha\":\"abcdef1234567\",\"at\":\"2026-11-13T09:00:00Z\",\"note\":\"Looked at it\",\"counted\":true}," \
    "{\"id\":3,\"source\":\"briareus\",\"by\":{\"userId\":8,\"name\":\"Lin\"},\"headSha\":\"1111111bbbb\",\"counted\":false,\"reason\":\"head_moved\",\"message\":\"Lin approved another head\"}," \
    "{\"id\":4,\"source\":\"github\",\"by\":{\"userId\":null,\"githubLogin\":\"octo\"},\"headSha\":\"abcdef1234567\",\"counted\":false,\"reason\":\"github_not_accepted\"}," \
    "{\"source\":\"briareus\",\"headSha\":\"x\"}]}"

static void parse_requirement(const char *text, ReviewApprovals *out) { Json *j = json_parsez(text); review_approvals_parse(j, out); json_free(j); }

static void a_requirement_reads_its_risk_reasons_and_every_approval(void) {
    ReviewApprovals a; parse_requirement(REQUIREMENT, &a);
    CHECK(a.present); CHECK(a.enabled); CHECK(a.review_current); CHECK_STR(a.head_sha, "abcdef1234567"); CHECK_STR(a.tier, "high");
    CHECK_INT(a.required, 2); CHECK_INT(a.valid, 1); CHECK_INT(a.missing, 1);
    // What set the tier, each with a message; an entry without a code is skipped.
    CHECK_INT(a.risk_count, 2); CHECK_STR(a.risk[0].tier, "high"); CHECK_STR(a.risk[0].message, "migrations/** matched 2 files");
    CHECK(a.risk[1].tier == NULL); CHECK_STR(a.risk[1].message, "size lines");
    // Why it stands as it does: what it waits on first, the notes after.
    CHECK_INT(a.reasons.count, 2);
    const ReviewReason *listed[2] = {0}; review_reasons_listed(&a.reasons, listed);
    CHECK_STR(listed[0]->code, "approvals_missing"); CHECK_STR(listed[1]->code, "github_approvals_unread");
    // Every approval with an id, oldest first.
    CHECK_INT(a.count, 4);
    const ReviewApproval *withdrawn = &a.items[0], *counts = &a.items[1], *moved = &a.items[2], *github = &a.items[3];
    CHECK(withdrawn->withdrawn); CHECK_STR(withdrawn->withdraw_note, "Too early"); CHECK(!withdrawn->counted);
    CHECK(counts->counted); CHECK(counts->user_id == 7); CHECK_STR(counts->note, "Looked at it"); CHECK_STR(counts->at, "2026-11-13T09:00:00Z");
    CHECK(github->from_github); CHECK(github->user_id == 0); CHECK_STR(github->github_login, "octo");
    CHECK_OWNED_STR(review_approval_who(counts), "Ada"); CHECK_OWNED_STR(review_approval_who(github), "@octo");
    CHECK_OWNED_STR(review_approval_state(counts), "Counts"); CHECK_OWNED_STR(review_approval_state(withdrawn), "Withdrawn");
    CHECK_OWNED_STR(review_approval_state(moved), "Lin approved another head"); CHECK_OWNED_STR(review_approval_state(github), "github not accepted");
    CHECK_INT(review_approval_tone(counts), REVIEW_TONE_OK); CHECK_INT(review_approval_tone(moved), REVIEW_TONE_MUTED);
    CHECK_OWNED_STR(review_approval_detail(counts), "abcdef1"); CHECK_OWNED_STR(review_approval_detail(github), "on GitHub \xC2\xB7 abcdef1");
    CHECK(review_approvals_find(&a, 3) == moved); CHECK(review_approvals_find(&a, 99) == NULL); CHECK(review_approvals_find(NULL, 1) == NULL);
    CHECK(review_approvals_shown(&a)); CHECK_OWNED_STR(review_approvals_line(&a), "1 of 2 approvals \xC2\xB7 high risk");
    review_approvals_free(&a); CHECK(a.items == NULL && a.count == 0);
    review_approvals_free(NULL);
}
static void approve_waits_for_a_reviewed_head_and_offers_itself_once(void) {
    ReviewApprovals a; parse_requirement(REQUIREMENT, &a);
    // User 7's approval already counts for the head; user 8's and an unknown user's would not.
    CHECK(!review_approvals_can_approve(&a, 7)); CHECK(review_approvals_can_approve(&a, 8)); CHECK(review_approvals_can_approve(&a, 0));
    CHECK_OWNED_STR(review_approvals_approve_consequence(&a), "Your approval counts for abcdef1 only, and stops counting once the pull request moves on.");
    // Nothing is offered while no ended review covers the head, without a head, or when nothing is asked.
    a.review_current = false; CHECK(!review_approvals_can_approve(&a, 8)); a.review_current = true;
    char *head = a.head_sha; a.head_sha = NULL; CHECK(!review_approvals_can_approve(&a, 8)); a.head_sha = head;
    char *result = a.result; a.result = "not_required"; CHECK(!review_approvals_can_approve(&a, 8)); a.result = result;
    a.enabled = false; CHECK(!review_approvals_can_approve(&a, 8));
    review_approvals_free(&a);
}
static void only_an_approver_withdraws_their_own_recorded_approval(void) {
    ReviewApprovals a; parse_requirement(REQUIREMENT, &a);
    const ReviewApproval *withdrawn = &a.items[0], *counts = &a.items[1], *moved = &a.items[2], *github = &a.items[3];
    CHECK(review_approval_can_withdraw(counts, 7)); CHECK(!review_approval_can_withdraw(counts, 8));
    // Without the token's user every approval recorded here is offered; the server refuses another person's.
    CHECK(review_approval_can_withdraw(moved, 0));
    CHECK(!review_approval_can_withdraw(withdrawn, 7)); CHECK(!review_approval_can_withdraw(github, 0));
    CHECK(review_approval_withdraw_consequence() != NULL);
    review_approvals_free(&a);
}
static void the_line_and_tone_say_when_nothing_is_needed(void) {
    // A push lowered the tier: nothing is asked, but the approvals on record keep the block shown.
    ReviewApprovals a; parse_requirement("{\"enabled\":true,\"result\":\"not_required\",\"tier\":\"low\",\"required\":0,\"reviewCurrent\":true,\"headSha\":\"abc\","
        "\"approvals\":[{\"id\":2,\"source\":\"briareus\",\"by\":{\"userId\":7},\"headSha\":\"old\",\"counted\":false,\"reason\":\"head_moved\"}]}", &a);
    CHECK(review_approvals_shown(&a)); CHECK_OWNED_STR(review_approvals_line(&a), "None needed \xC2\xB7 low risk");
    CHECK_INT(review_approvals_tone(&a), REVIEW_TONE_MUTED); CHECK(!review_approvals_can_approve(&a, 7));
    review_approvals_free(&a);
    parse_requirement("{\"enabled\":true,\"result\":\"not_required\",\"approvals\":[]}", &a);
    CHECK(!review_approvals_shown(&a)); CHECK_OWNED_STR(review_approvals_line(&a), "None needed"); review_approvals_free(&a);
    parse_requirement("{\"enabled\":true}", &a); CHECK(!a.present); CHECK(!review_approvals_shown(&a)); review_approvals_free(&a);
    parse_requirement("[]", &a); CHECK(!a.present); review_approvals_free(&a);
}
static void the_write_bodies_trim_the_note_and_name_the_head(void) {
    Json *body = review_approval_approve_args("o/r", 12, "abcdef1234567", "  Looked at it \n");
    char *text = json_serialize(body, false);
    CHECK_STR(text, "{\"repo\":\"o/r\",\"pr\":12,\"note\":\"Looked at it\",\"headSha\":\"abcdef1234567\"}"); free(text); json_free(body);
    body = review_approval_withdraw_args("o/r", 12, 5, " \t");
    text = json_serialize(body, false); CHECK_STR(text, "{\"repo\":\"o/r\",\"pr\":12,\"approvalId\":5}"); free(text); json_free(body);
    body = review_approval_withdraw_args("o/r", 12, 5, NULL);
    CHECK_INT(json_count(body), 3); json_free(body);
    CHECK(review_approval_rereads("head_moved")); CHECK(review_approval_rereads("review_not_current")); CHECK(review_approval_rereads("review_running"));
    CHECK(!review_approval_rereads("own_change")); CHECK(!review_approval_rereads(NULL));
}
static Json *state_of(const char *answer, ReviewOutcomeSource source, const Json *previous) {
    Json *j = json_parsez(answer), *state = review_outcome_state(j, source, previous);
    json_free(j); return state;
}
static void an_approval_answer_replaces_the_outcome_or_only_its_approvals(void) {
    Json *before = state_of("{\"reviewOutcome\":{\"outcome\":\"passed\",\"approvals\":{\"enabled\":true,\"result\":\"pending\",\"required\":1}},"
        "\"reviewRounds\":{\"used\":2,\"limit\":3}}", REVIEW_OUTCOME_FROM_READ, NULL);
    // With an outcome, it replaces the kept one; the rounds stay.
    Json *after = state_of("{\"approval\":{\"id\":9},\"reviewOutcome\":{\"outcome\":\"passed\",\"ready\":true,\"approvals\":{\"enabled\":true,\"result\":\"satisfied\",\"required\":1,\"valid\":1}}}",
        REVIEW_OUTCOME_FROM_APPROVAL, before);
    ReviewOutcome o; CHECK(review_outcome_parse(after, &o));
    CHECK(o.ready); CHECK_STR(o.approvals.result, "satisfied"); CHECK_INT(o.rounds.used, 2);
    review_outcome_free(&o); json_free(after);
    // Without one, only its requirement replaces the kept outcome's approvals.
    after = state_of("{\"approval\":{\"id\":9},\"approvals\":{\"enabled\":true,\"result\":\"satisfied\",\"required\":1,\"valid\":1}}", REVIEW_OUTCOME_FROM_APPROVAL, before);
    CHECK(review_outcome_parse(after, &o));
    CHECK(!o.ready); CHECK_INT(o.outcome, REVIEW_OUTCOME_PASSED); CHECK_STR(o.approvals.result, "satisfied"); CHECK_INT(o.approvals.valid, 1); CHECK_INT(o.rounds.limit, 3);
    review_outcome_free(&o); json_free(after);
    // With neither, everything is kept; nothing kept and nothing answered leaves nothing.
    after = state_of("{\"approval\":{\"id\":9}}", REVIEW_OUTCOME_FROM_APPROVAL, before);
    CHECK(json_equal(after, before)); json_free(after);
    CHECK(state_of("{\"approvals\":{\"result\":\"satisfied\"}}", REVIEW_OUTCOME_FROM_APPROVAL, NULL) == NULL);
    json_free(before);
}
void core_review_approvals_tests(void) {
    test_run("an approval requirement reads its risk, reasons and every approval on record", a_requirement_reads_its_risk_reasons_and_every_approval);
    test_run("approve waits for a reviewed head and is offered until the person's approval counts", approve_waits_for_a_reviewed_head_and_offers_itself_once);
    test_run("only an approver withdraws their own approval recorded in Briareus", only_an_approver_withdraws_their_own_recorded_approval);
    test_run("the approvals line and tone say when a lower tier needs none", the_line_and_tone_say_when_nothing_is_needed);
    test_run("approval write bodies trim the note and name the reviewed head", the_write_bodies_trim_the_note_and_name_the_head);
    test_run("an approval answer replaces the outcome, or only its approvals, and keeps the rounds", an_approval_answer_replaces_the_outcome_or_only_its_approvals);
}
