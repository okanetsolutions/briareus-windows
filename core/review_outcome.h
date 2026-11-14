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
// One risk category the security pass assessed (`coverage.categories`): whether the change touches it, what was
// examined and concluded, and how many of its findings fall under it.
typedef struct { char *id, *assessment; bool applicable; int findings; } SecurityCategory;
// One finding the pass recorded (`coverage.entries`); `start_line` 0 when it names none, `claim` the earlier security
// claim it raises again, NULL for a new one.
typedef struct { char *category, *severity, *title, *file_path, *claim; int start_line; } SecurityEntry;
// An earlier security claim the pass checked against the current code (`coverage.claims`): `fixed` or `stands`, with its
// evidence (NULL when it gave none).
typedef struct { char *finding_ref, *status, *evidence; } SecurityClaim;
// The review's findings marked security, and how they and earlier rounds' security claims stand (`counts`).
typedef struct { int findings, fix, optional, dismissed, suppressed, unverified; } SecurityCounts;
// The security pass under the project's policy (`off`, `advisory`, `required`), its result and its own reasons; the
// rest is its overview (review_security.h): the tier that made it required, the stage and head it covered, and the
// coverage record it left (`covered` false until a pass completed with one): how many findings it recorded, and what an
// incremental pass widened to (NULL when it did not).
typedef struct {
    bool present, blocking, stale, covered;
    char *policy, *result, *required_by_risk, *stage, *head_sha, *widened;   // the last four NULL when absent
    ReviewReasons reasons;
    SecurityCounts counts;
    SecurityCategory *categories; size_t category_count;
    SecurityEntry *entries; size_t entry_count;
    SecurityClaim *claims; size_t claim_count;
    int recorded;
} ReviewSecurity;
// One approval on record (`Approval`): recorded by a person through Briareus, or read from GitHub (`from_github`, with
// `user_id` 0). `counted` says whether it counts now; `reason` and `message` say why not.
typedef struct {
    long long id, user_id;
    bool from_github, withdrawn, counted;
    char *name, *github_login, *head_sha, *at, *note, *withdraw_note, *reason, *message;   // NULL when absent
} ReviewApproval;
// One thing that set the change's risk tier (`risk.reasons`): a sensitive path rule, a size threshold, an unreadable list.
typedef struct { char *code, *tier, *message; } RiskReason;
// Whether the people the change's risk calls for approved the reviewed revision; a result apart from the outcome.
// `head_sha` is the head an approval names, and `review_current` whether a review that has ended covers it.
typedef struct {
    bool present, enabled, review_current;
    char *result, *tier, *explanation, *head_sha;
    int required, valid, missing;
    ReviewReasons reasons;
    RiskReason *risk; size_t risk_count;
    ReviewApproval *items; size_t count;   // oldest first
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
// The `{ reviewOutcome, reviewRounds }` to keep after `answer`, over `previous` (either may be NULL); owned, NULL for none.
Json *review_outcome_state(const Json *answer, ReviewOutcomeSource source, const Json *previous);
// Reads what review_outcome_state kept; false, with `out` empty, while it holds no outcome this client knows (an older
// server, or nothing read yet). Free with review_outcome_free.
bool review_outcome_parse(const Json *state, ReviewOutcome *out);
void review_outcome_free(ReviewOutcome *outcome);
// What a screen keeps of a pull request's review: `*state` as review_outcome_state leaves it after `answer`, and
// `*outcome` read from it (owned, NULL while it holds none this client knows). Drop frees both and leaves them NULL.
void review_outcome_keep(Json **state, ReviewOutcome **outcome, const Json *answer, ReviewOutcomeSource source);
void review_outcome_drop(Json **state, ReviewOutcome **outcome);

// The server's `reasons` list read into `out`, every entry with a code; free with review_reasons_free.
void review_reasons_parse(const Json *list, ReviewReasons *out);
void review_reasons_free(ReviewReasons *reasons);

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
// "2 of 3 rounds", "4 rounds" without a limit, with any rounds granted past it; owned.
char *review_rounds_line(const ReviewRounds *rounds);
ReviewTone review_rounds_tone(const ReviewRounds *rounds);
#endif
