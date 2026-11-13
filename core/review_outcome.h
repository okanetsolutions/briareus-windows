// What a pull request's code review amounts to, as the server computes it (`reviewOutcome` and `reviewRounds` in the
// `code_reviews` answer, and `reviewOutcome` again in a verdict save's answer): the outcome and why, whether it is stale
// or ready, the rounds used against the project's limit, the security pass and the approvals the change's risk asks for.
// The Code reviews tab draws it above the reviews. No UI here.
#ifndef BRIAREUS_REVIEW_OUTCOME_H
#define BRIAREUS_REVIEW_OUTCOME_H
#include "json.h"

typedef enum { REVIEW_TONE_MUTED, REVIEW_TONE_RUNNING, REVIEW_TONE_OK, REVIEW_TONE_WARN, REVIEW_TONE_DANGER } ReviewTone;
typedef enum { REVIEW_OUTCOME_PENDING, REVIEW_OUTCOME_INCOMPLETE, REVIEW_OUTCOME_CHANGES_REQUIRED, REVIEW_OUTCOME_PASSED } ReviewOutcomeKind;

// One reason the server gives: `effect` is the outcome it alone leads to, or "note" when it only informs (or says none);
// `message` falls back to the code in words.
typedef struct { char *code, *effect, *message; } ReviewReason;
typedef struct { ReviewReason *items; size_t count; } ReviewReasons;
// The security pass under the project's policy (`off`, `advisory`, `required`), its result and its own reasons.
typedef struct { bool present, blocking; char *policy, *result; ReviewReasons reasons; } ReviewSecurity;
// Whether the people the change's risk calls for approved the reviewed revision; a result apart from the outcome.
typedef struct { bool present, enabled; char *result, *tier, *explanation; int required, valid, missing; } ReviewApprovals;
// The rounds used against the project's `reviewRoundLimit` (0: none), and how many a person granted past it.
typedef struct { bool present, limit_reached; int limit, used, continuations; } ReviewRounds;
typedef struct {
    ReviewOutcomeKind outcome;
    ReviewReasons reasons;
    bool stale, ready, enforced, published, status_retried;
    char *status_error;   // why GitHub refused the commit status the last time; NULL when it did not
    ReviewSecurity security;
    ReviewApprovals approvals;
    ReviewRounds rounds;
} ReviewOutcome;

// Which answer the outcome comes from, and so what it replaces: a read replaces both the outcome and the rounds; a
// verdict save replaces the outcome (and leaves none when it carries none) and keeps the rounds; any other save keeps both.
typedef enum { REVIEW_OUTCOME_FROM_READ, REVIEW_OUTCOME_FROM_VERDICT, REVIEW_OUTCOME_FROM_OTHER } ReviewOutcomeSource;
// The `{ reviewOutcome, reviewRounds }` to keep after `answer`, over `previous` (either may be NULL); owned, NULL for none.
Json *review_outcome_state(const Json *answer, ReviewOutcomeSource source, const Json *previous);
// Reads what review_outcome_state kept; false, with `out` empty, while it holds no outcome this client knows (an older
// server, or nothing read yet). Free with review_outcome_free.
bool review_outcome_parse(const Json *state, ReviewOutcome *out);
void review_outcome_free(ReviewOutcome *outcome);

const char *review_outcome_title(ReviewOutcomeKind kind);
ReviewTone review_outcome_tone(ReviewOutcomeKind kind);
bool review_reason_is_note(const ReviewReason *reason);
ReviewTone review_reason_tone(const ReviewReason *reason);
// Borrowed pointers into `reasons`, those that block or explain first and the notes after, each in the server's order;
// `out` holds `reasons->count`.
void review_reasons_listed(const ReviewReasons *reasons, const ReviewReason **out);
// Why the outcome is out of date, when it is; static, NULL when it is not stale.
const char *review_outcome_stale_note(const ReviewOutcome *outcome);
// An enforced outcome whose commit status GitHub does not carry yet; owned, NULL when there is nothing to say.
char *review_outcome_status_note(const ReviewOutcome *outcome);
// Said once the review passed and the approvals it needs are in; static, NULL otherwise.
const char *review_outcome_ready_note(const ReviewOutcome *outcome);
// Shown only when the project asks for a pass at all; "Passed · required" and the like, owned.
bool review_security_shown(const ReviewSecurity *security);
char *review_security_line(const ReviewSecurity *security);
ReviewTone review_security_tone(const ReviewSecurity *security);
// Shown only while a policy is on and the change's tier asks for approvals; "1 of 2 approvals · high risk", owned.
bool review_approvals_shown(const ReviewApprovals *approvals);
char *review_approvals_line(const ReviewApprovals *approvals);
ReviewTone review_approvals_tone(const ReviewApprovals *approvals);
// "2 of 3 rounds", "4 rounds" without a limit, with any rounds granted past it; owned.
char *review_rounds_line(const ReviewRounds *rounds);
ReviewTone review_rounds_tone(const ReviewRounds *rounds);
#endif
