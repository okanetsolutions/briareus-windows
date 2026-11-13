// What a pull request's code review amounts to, as the server computes it (`reviewOutcome` and `reviewRounds` in the
// `code_reviews` answer, and `reviewOutcome` again in a verdict save's or an approval's answer): the outcome and why,
// whether it is stale or ready, the rounds used against the project's limit, the security pass and the approvals the
// change's risk asks for (with every approval on record, review_approvals.h). The Code reviews tab draws it above the
// reviews. No UI here.
#ifndef BRIAREUS_REVIEW_OUTCOME_H
#define BRIAREUS_REVIEW_OUTCOME_H
#include "json.h"
#include <stdint.h>

typedef enum { REVIEW_TONE_MUTED, REVIEW_TONE_RUNNING, REVIEW_TONE_OK, REVIEW_TONE_WARN, REVIEW_TONE_DANGER } ReviewTone;
typedef enum { REVIEW_OUTCOME_PENDING, REVIEW_OUTCOME_INCOMPLETE, REVIEW_OUTCOME_CHANGES_REQUIRED, REVIEW_OUTCOME_PASSED } ReviewOutcomeKind;

// One reason the server gives: `effect` is the outcome it alone leads to, or "note" when it only informs (or says none);
// `message` falls back to the code in words.
typedef struct { char *code, *effect, *message; } ReviewReason;
typedef struct { ReviewReason *items; size_t count; } ReviewReasons;
// The security pass under the project's policy (`off`, `advisory`, `required`), its result and its own reasons.
typedef struct { bool present, blocking; char *policy, *result; ReviewReasons reasons; } ReviewSecurity;
// One thing that set the change's risk tier: a sensitive path rule, a size threshold, an unreadable file list. `tier`
// may be NULL; `message` falls back to the code in words.
typedef struct { char *code, *tier, *message; } RiskReason;
// One approval the server has on record: recorded by a person here, or read from the pull request's reviews on GitHub.
// `user_id` is the Briareus user who recorded it, 0 for one read from GitHub. Times are ISO strings, NULL when absent.
typedef struct {
    int64_t id, user_id;
    bool from_github, counted, withdrawn;
    char *name, *github_login, *head_sha, *at, *note, *withdrawn_at, *withdraw_note, *reason, *message;
} ReviewApproval;
// Whether the people the change's risk calls for approved the reviewed revision; a result apart from the outcome.
// `head_sha` is the head the requirement is for, which an approval names, and `review_current` whether a review that has
// ended covers it. `risk` is what set the tier, `reasons` why the requirement stands as it does, `items` every approval
// on record, oldest first.
typedef struct {
    bool present, enabled, review_current;
    char *result, *tier, *explanation, *head_sha;
    int required, valid, missing;
    RiskReason *risk; size_t risk_count;
    ReviewReasons reasons;
    ReviewApproval *items; size_t count;
} ReviewApprovals;
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
// verdict save replaces the outcome (and leaves none when it carries none) and keeps the rounds; an approval recorded or
// withdrawn replaces the outcome, or without one only its approvals, and keeps the rounds; any other save keeps both.
typedef enum { REVIEW_OUTCOME_FROM_READ, REVIEW_OUTCOME_FROM_VERDICT, REVIEW_OUTCOME_FROM_APPROVAL, REVIEW_OUTCOME_FROM_OTHER } ReviewOutcomeSource;
// What the answer to the write named `call` replaces of the kept outcome and rounds.
ReviewOutcomeSource review_outcome_source_after(const char *call);
// The `{ reviewOutcome, reviewRounds }` to keep after `answer`, over `previous` (either may be NULL); owned, NULL for none.
Json *review_outcome_state(const Json *answer, ReviewOutcomeSource source, const Json *previous);
// Reads what review_outcome_state kept; false, with `out` empty, while it holds no outcome this client knows (an older
// server, or nothing read yet). Free with review_outcome_free.
bool review_outcome_parse(const Json *state, ReviewOutcome *out);
void review_outcome_free(ReviewOutcome *outcome);
// The server's `{ code, effect, message }` reasons, those without a code left out; free with review_reasons_free.
void review_reasons_parse(const Json *list, ReviewReasons *out);
void review_reasons_free(ReviewReasons *reasons);
// A `SecurityReview`'s policy, result and reasons; not `present` without a result. Free with review_security_free.
void review_security_parse(const Json *security, ReviewSecurity *out);
void review_security_free(ReviewSecurity *security);

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
// Shown while a policy is on and the change's tier asks for approvals, or approvals are on record (a push that lowered
// the tier leaves the earlier ones listed); "1 of 2 approvals · high risk", or "None needed · low risk", owned.
bool review_approvals_shown(const ReviewApprovals *approvals);
char *review_approvals_line(const ReviewApprovals *approvals);
ReviewTone review_approvals_tone(const ReviewApprovals *approvals);
// "2 of 3 rounds", "4 rounds" without a limit, with any rounds granted past it; owned.
char *review_rounds_line(const ReviewRounds *rounds);
ReviewTone review_rounds_tone(const ReviewRounds *rounds);
#endif
