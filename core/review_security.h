// A security pass in full (the server's `SecurityReview`, the pull request's in `reviewOutcome.security` and each stored
// review's own in its `security`): beyond the result and policy the outcome card names, the risk tier that made the pass
// required, the stage and head it covered, what it examined per risk category, the findings it recorded, the earlier
// claims it checked and how the security findings stand. The Code reviews tab draws the pull request's as its own block
// under the outcome and a review's on its opened page. No UI here.
#ifndef BRIAREUS_REVIEW_SECURITY_H
#define BRIAREUS_REVIEW_SECURITY_H
#include "review_outcome.h"

// Reads `security` into `out`, which stays empty (`present` false) for anything but a result; free with
// review_security_free. A pass without a policy is held to `off`.
void review_security_parse(const Json *security, ReviewSecurity *out);
void review_security_free(ReviewSecurity *security);

// "Required by this change's high risk tier." when the project's own policy would not require it; owned, NULL otherwise.
char *review_security_risk_note(const ReviewSecurity *security);
// The pass's stage and the head it covered: "Pass completed on 1a2b3c4", "No security pass ran"; with "· out of date"
// once the pull request moved on. Owned, NULL when the server says nothing of either.
char *review_security_scope_line(const ReviewSecurity *security);
// The security findings and how they stand, "3 findings · 1 to fix · 1 optional · 1 dismissed", or "No security
// findings" once a pass completed with none; owned, NULL before a pass completed and with nothing counted.
char *review_security_findings_line(const ReviewSecurity *security);
// "Earlier claims: 2 fixed · 1 still stands"; owned, NULL when the pass checked none.
char *review_security_claims_line(const ReviewSecurity *security);
// One claim checked, "f-12: fixed · Guard added"; owned. A claim that still stands reads as a warning.
char *review_security_claim_line(const SecurityClaim *claim);
ReviewTone review_security_claim_tone(const SecurityClaim *claim);
// A stored code review's own security pass (its `security`, the same shape as the pull request's). False, with `out`
// empty, when there is nothing of its own to show: no result (an older server), or a policy that ran no pass in it.
bool review_security_of_review(const Json *review, ReviewSecurity *out);
// A category's name in words ("Untrusted input"), its id with spaces for one this client does not know; owned.
char *review_security_category_title(const SecurityCategory *category);
// "Applies · 1 finding", "Applies", "Does not apply"; owned.
char *review_security_category_line(const SecurityCategory *category);
ReviewTone review_security_category_tone(const SecurityCategory *category);
// Where a recorded finding is, "app/x.c:12", or the file alone; owned, NULL without a file.
char *review_security_entry_location(const SecurityEntry *entry);

// Where a pull request's security result stands across pull requests, worst first: what blocks, what could not be read, what
// is owed, what is under way, what passed, and a pass the project does not ask for.
typedef enum {
    SECURITY_RANK_FAILED, SECURITY_RANK_UNREAD, SECURITY_RANK_INCOMPLETE, SECURITY_RANK_NOT_RUN, SECURITY_RANK_PENDING, SECURITY_RANK_PASSED,
    SECURITY_RANK_OFF, SECURITY_RANKS
} SecurityRank;
// NULL or an absent security result ranks as unread, distinct from an explicit not_required result.
SecurityRank review_security_rank(const ReviewSecurity *security);
// "Failed", "Could not be read", "Incomplete", "Not run", "Pending", "Passed", "Not asked"; static.
const char *review_security_rank_title(SecurityRank rank);
// The pull requests counted per rank, "2 failed · 1 not run · 10 passed", the ones not asked for last; "No open pull
// requests" when every count is 0. Owned.
char *review_security_summary(const size_t counts[SECURITY_RANKS]);
#endif
