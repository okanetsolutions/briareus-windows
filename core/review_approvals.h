// The human approvals a pull request's risk asks for, beyond the counts the review outcome shows (`reviewOutcome.approvals`,
// the server's `ApprovalRequirement`): what set the risk tier, every approval on record with whether it counts and why not,
// and the two writes a person makes, recording their approval of the reviewed head (`approve_pull`) and withdrawing one of
// their own (`withdraw_pull_approval`). The Code reviews tab draws it under the outcome. No UI here.
#ifndef BRIAREUS_REVIEW_APPROVALS_H
#define BRIAREUS_REVIEW_APPROVALS_H
#include "review_outcome.h"

/// The notes kept with an approval or a withdrawal are held to the server's 2000 characters.
enum { REVIEW_APPROVAL_NOTE_LIMIT = 2000 };

// Reads `approvals` into `out`, which stays empty (`present` false) for anything but a requirement with a result; free
// with review_approvals_free.
void review_approvals_parse(const Json *approvals, ReviewApprovals *out);
void review_approvals_free(ReviewApprovals *approvals);

// Shown while a policy is on and the change's tier asks for approvals, or approvals are on record: a push that lowered the
// tier leaves the earlier ones listed. "1 of 2 approvals · high risk", or "None needed · low risk"; owned.
bool review_approvals_shown(const ReviewApprovals *approvals);
char *review_approvals_line(const ReviewApprovals *approvals);
ReviewTone review_approvals_tone(const ReviewApprovals *approvals);
// Approve is offered while the requirement asks for people, a review that has ended covers the head it names, and no
// approval of user `me` already counts for that head. `me` 0 (a server that does not say who the token is) offers it.
bool review_approvals_can_approve(const ReviewApprovals *approvals, long long me);
// What approving means, said before it is sent: it counts for the reviewed head alone; owned.
char *review_approvals_approve_consequence(const ReviewApprovals *approvals);
// The approval with `id`, borrowed; NULL when none is on record.
const ReviewApproval *review_approvals_find(const ReviewApprovals *approvals, long long id);

// Who gave it: the person's name, else the GitHub account as `@login`; owned.
char *review_approval_who(const ReviewApproval *approval);
// "Counts", "Withdrawn", or why it does not count in the server's words; owned.
char *review_approval_state(const ReviewApproval *approval);
ReviewTone review_approval_tone(const ReviewApproval *approval);
// "on GitHub · 1a2b3c4" for one read from GitHub, the head it approves in short; owned, empty when there is neither.
char *review_approval_detail(const ReviewApproval *approval);
// Only its approver withdraws one recorded in Briareus; one read from GitHub is dismissed there. With `me` 0 every
// approval recorded here is offered, and the server refuses another person's.
bool review_approval_can_withdraw(const ReviewApproval *approval, long long me);
// What withdrawing means, said before it is sent; static.
const char *review_approval_withdraw_consequence(void);

// The write bodies, with the note trimmed and left out when empty; owned. Approving names exactly `head_sha`, the head the
// requirement was computed for, which the server refuses once the pull request has moved on.
Json *review_approval_approve_args(const char *repo, int number, const char *head_sha, const char *note);
Json *review_approval_withdraw_args(const char *repo, int number, long long approval_id, const char *note);
// A refusal saying the kept requirement is out of date (`head_moved`, `review_not_current`, `review_running`): the
// reviews are read again so Approve follows the head the server has.
bool review_approval_rereads(const char *code);
#endif
