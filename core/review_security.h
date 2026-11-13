// A pull request's security pass in full (`reviewOutcome.security`, the server's `SecurityReview`): beyond the result
// and policy the outcome card names, the risk tier that made the pass required, the stage and head it covered, what it
// examined per risk category, the findings it recorded and how the security findings stand. The Code reviews tab draws
// it as its own block under the outcome. No UI here.
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
// A category's name in words ("Untrusted input"), its id with spaces for one this client does not know; owned.
char *review_security_category_title(const SecurityCategory *category);
// "Applies · 1 finding", "Applies", "Does not apply"; owned.
char *review_security_category_line(const SecurityCategory *category);
ReviewTone review_security_category_tone(const SecurityCategory *category);
// Where a recorded finding is, "app/x.c:12", or the file alone; owned, NULL without a file.
char *review_security_entry_location(const SecurityEntry *entry);
#endif
