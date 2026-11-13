#ifndef BRIAREUS_CODE_REVIEWS_H
#define BRIAREUS_CODE_REVIEWS_H
#include "json.h"
#include <stdint.h>

// Owns a clone of the public codeReviews array; malformed data returns NULL, never an empty success.
Json *code_reviews_parse(const Json *response);
int64_t code_review_id(const Json *record);
// Borrowed records, independent of the lifetime of review sessions.
const Json *code_review_find(const Json *reviews, int64_t id);
const Json *code_review_finding(const Json *review, int64_t id);
size_t code_reviews_findings(const Json *reviews);
bool code_reviews_running(const Json *reviews);
// "current head", "older head" or "head unknown"; never equates two unknown heads.
const char *code_review_head(const Json *review, const char *current_head);
// Null verdicts await work only while their parent review is running.
const char *code_review_verdict(const Json *review, const Json *finding);
const char *code_review_verification(const Json *review, const Json *finding);
// Whether the finding counts towards the review's security result: one of its security pass, or one the server marked so.
bool code_review_finding_is_security(const Json *finding);
// Owned display strings; null usage remains visibly unknown.
char *code_review_location(const Json *finding);
char *code_review_usage(const Json *review);
char *code_review_counts(const Json *review);
char *code_reviews_counts(const Json *reviews);
// Values are sums of known nonnegative observations; missing values never become zero.
typedef struct { double value; size_t known, unknown; } CodeReviewAmount;
typedef struct {
    size_t reviews, findings, unpriced_turns;
    CodeReviewAmount input, output, cost;
    bool estimated;
} CodeReviewTotals;
CodeReviewTotals code_review_totals(const Json *review);
CodeReviewTotals code_reviews_totals(const Json *reviews);
char *code_review_counts_compact(const Json *review);

// A person marks a stopped review handled or reopens it (`update_code_review`), and overrules a finding's verdict with
// its evidence and reason (`update_code_review_finding`); each answer is folded back into the list as read.

// "handled" or "pending"; NULL from a server that does not track it, which cannot change it either.
const char *code_review_handling(const Json *review);
typedef enum { CODE_REVIEW_HANDLING_NONE, CODE_REVIEW_MARK_HANDLED, CODE_REVIEW_REOPEN } CodeReviewHandlingAction;
// What can be done to a stopped review's handling; none while it runs or without `handlingStatus`.
CodeReviewHandlingAction code_review_handling_action(const Json *review);
const char *code_review_handling_title(CodeReviewHandlingAction action);
// Owned `update_code_review` arguments setting the handling `action` leads to.
Json *code_review_handling_args(const char *repo, int number, int64_t review_id, CodeReviewHandlingAction action);
// A review's verdicts can be overruled once it stopped running: a rerun rewrites its findings.
bool code_review_can_edit_findings(const Json *review);
// Folds an answer into `reviews` in place: the review with its id, or that review's finding with its id. False, changing
// nothing, when the answer is malformed or the list does not hold what it replaces.
bool code_reviews_replace_review(Json *reviews, const Json *review);
bool code_reviews_replace_finding(Json *reviews, int64_t review_id, const Json *finding);

// The editor's choices in order; the last, NULL, leaves the verdict or the evidence unstated.
enum { CODE_REVIEW_VERDICT_CHOICES = 4, CODE_REVIEW_VERIFICATION_CHOICES = 3 };
extern const char *const code_review_verdict_choices[CODE_REVIEW_VERDICT_CHOICES];
extern const char *const code_review_verification_choices[CODE_REVIEW_VERIFICATION_CHOICES];
// "Fix", "Optional", "Dismissed" or "None"; "Confirmed", "Plausible" or "None".
const char *code_review_verdict_title(const char *verdict);
const char *code_review_verification_title(const char *verification);
// One finding's verdict being overruled. `verdict` and `verification` point at the choices above; `reason` is owned and
// kept as typed. Not fixing a finding needs a reason and the evidence the server's review outcome asks; only what
// changed is sent. Free with code_review_edit_free.
typedef struct {
    int64_t review_id, finding_id;
    const char *verdict, *verification, *saved_verdict, *saved_verification;
    char *reason, *saved_reason;
} CodeReviewFindingEdit;
void code_review_edit_init(CodeReviewFindingEdit *edit, const Json *review, const Json *finding);
void code_review_edit_free(CodeReviewFindingEdit *edit);
// Picks a choice by its index; out of range changes nothing.
void code_review_edit_choose_verdict(CodeReviewFindingEdit *edit, size_t choice);
void code_review_edit_choose_verification(CodeReviewFindingEdit *edit, size_t choice);
void code_review_edit_set_reason(CodeReviewFindingEdit *edit, const char *reason);
bool code_review_edit_dirty(const CodeReviewFindingEdit *edit);
bool code_review_edit_needs_reason(const CodeReviewFindingEdit *edit);
// A reason is given, and one of its own once the verdict changed: the saved one argued for the old verdict.
bool code_review_edit_has_reason(const CodeReviewFindingEdit *edit);
// Any evidence for Optional, Confirmed for Dismissed, none for Fix or no verdict.
bool code_review_edit_has_verification(const CodeReviewFindingEdit *edit);
// Changed, with the reason and evidence it needs. Rewording the reason of a ruling stored before evidence was asked
// leaves that ruling as it stands, so it is not held back.
bool code_review_edit_can_save(const CodeReviewFindingEdit *edit);
const char *code_review_edit_reason_prompt(const CodeReviewFindingEdit *edit);
// The lines under the reason box and the evidence picker; NULL when nothing is missing.
const char *code_review_edit_reason_hint(const CodeReviewFindingEdit *edit);
const char *code_review_edit_verification_hint(const CodeReviewFindingEdit *edit);
// Owned `update_code_review_finding` arguments: what changed, null clearing it.
Json *code_review_edit_args(const CodeReviewFindingEdit *edit, const char *repo, int number);
// The edit can be saved and its finding is still in a review of `reviews` that has stopped running.
bool code_reviews_allow_saving(const Json *reviews, const CodeReviewFindingEdit *edit);
#endif
