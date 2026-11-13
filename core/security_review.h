// A code review's own security pass (`security` on a stored review, the server's `SecurityReview`): its result under the
// project's policy, the step's status, how the review's security findings stand, and the coverage record the pass left:
// each risk category with what was examined, the findings it recorded and the earlier security claims it checked. The
// Code reviews tab draws it on an opened review. No UI here.
#ifndef BRIAREUS_SECURITY_REVIEW_H
#define BRIAREUS_SECURITY_REVIEW_H
#include "review_outcome.h"

// One risk category of the coverage record: whether the change touches it, what was concluded, and its findings.
typedef struct { char *id, *assessment; bool applicable; int findings; } SecurityCategory;
// One finding the pass recorded; `start_line` 0 when not given.
typedef struct { char *id, *category, *severity, *title, *file_path, *claim; int start_line; } SecurityEntry;
// An earlier security claim the pass checked against the current code: `fixed` or `stands`, with its evidence.
typedef struct { char *finding_ref, *status, *evidence; } SecurityClaim;
// The review's findings marked security, and how they and the claims earlier rounds left stand.
typedef struct { bool present; int findings, fix, optional, dismissed, suppressed, unverified; } SecurityCounts;
typedef struct {
    ReviewSecurity result;          // policy, result, blocking and reasons, as the outcome's security reads
    char *stage;                    // the pass's step: pending, running, completed or failed; NULL when it ran none
    char *required_by_risk;         // the risk tier that made the pass required; NULL otherwise
    char *widened;                  // what an incremental pass widened to, when it says so in words
    bool stale, has_coverage;
    int recorded;                   // how many findings the pass recorded
    SecurityCategory *categories; size_t category_count;
    SecurityEntry *entries; size_t entry_count;
    SecurityClaim *claims; size_t claim_count;
    SecurityCounts counts;
} SecurityReview;

// Reads a stored review's `security`. False, with `out` empty, when there is nothing to show: no security result (an
// older server), or a policy that ran no pass and has no stage. Free with security_review_free.
bool security_review_parse(const Json *review, SecurityReview *out);
void security_review_free(SecurityReview *security);
// "Permissions", "Untrusted input" and the like; an unknown id in words. Owned.
char *security_category_title(const char *id);
// "Security pass completed", "Security pass failed", …; static.
const char *security_review_stage_line(const SecurityReview *security);
// "3 security findings · 1 to fix · 1 optional · 1 dismissed · 1 unverified", leaving out what is zero; owned, NULL
// without counts.
char *security_review_counts_line(const SecurityReview *security);
// "Required by the high risk tier", when risk made the pass required; owned, NULL otherwise.
char *security_review_risk_line(const SecurityReview *security);
// "high · Token logged · app/x.c:12 · secrets"; owned.
char *security_entry_line(const SecurityEntry *entry);
// "f-12: fixed · <evidence>"; owned.
char *security_claim_line(const SecurityClaim *claim);
#endif
