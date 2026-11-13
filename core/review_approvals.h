// The human approvals a pull request's risk asks for, beyond the counts the review outcome shows (review_outcome.h): what
// produced the risk tier, every approval on record with whether it counts and why not, and the two writes a person makes,
// recording their approval of the reviewed head (`approve_pull`) and withdrawing one of their own
// (`withdraw_pull_approval`). The Code reviews tab and ◉ Status draw it under the outcome. No UI here.
#ifndef BRIAREUS_REVIEW_APPROVALS_H
#define BRIAREUS_REVIEW_APPROVALS_H
#include "review_outcome.h"

// Reads an `ApprovalRequirement`; `out` stays empty (not `present`) for anything else. Free with review_approvals_free.
void review_approvals_parse(const Json *requirement, ReviewApprovals *out);
void review_approvals_free(ReviewApprovals *approvals);

// Who gave it: the person's name, else the GitHub account as "@login", else "Someone"; owned.
char *review_approval_who(const ReviewApproval *approval);
// "Counts", "Withdrawn", or why it does not count in the server's words (the reason code in words without them); owned.
char *review_approval_state(const ReviewApproval *approval);
ReviewTone review_approval_tone(const ReviewApproval *approval);
// "on GitHub" for one read from there, and the head it approves in short, joined by " · "; owned, possibly empty.
char *review_approval_detail(const ReviewApproval *approval);
// "Withdrawn: <note>" when its approver said why; owned, NULL otherwise.
char *review_approval_withdraw_line(const ReviewApproval *approval);
// Only its approver withdraws one recorded here, and only while it stands; one read from GitHub is dismissed there.
// `me` is the token's user (Device.user_id); without it (0, an older server) every approval recorded here is offered,
// and the server refuses another person's.
bool review_approval_can_withdraw(const ReviewApproval *approval, int64_t me);
// Approve is offered while the requirement asks for people, a review that has ended covers the head it names, and no
// approval of `me` already counts for that head.
bool review_approvals_can_approve(const ReviewApprovals *approvals, int64_t me);
// What approving means, naming the reviewed head in short; owned.
char *review_approvals_approve_consequence(const ReviewApprovals *approvals);
// What withdrawing means; static.
const char *review_approval_withdraw_consequence(void);

// The notes are kept to the server's 2000 characters.
enum { REVIEW_APPROVAL_NOTE_LIMIT = 2000 };
// Owned `approve_pull` arguments: exactly `head_sha`, the head the requirement was computed for, which the server refuses
// once the pull request has moved on. The note goes trimmed, and only when it says anything.
Json *review_approval_approve_args(const char *repo, int number, const char *head_sha, const char *note);
// Owned `withdraw_pull_approval` arguments naming the approval, with the trimmed note when given.
Json *review_approval_withdraw_args(const char *repo, int number, int64_t approval_id, const char *note);
// A refusal code saying the kept requirement is out of date (`head_moved`, `review_not_current`, `review_running`):
// the outcome is read again so Approve follows the head the server has instead of offering the old one again.
bool review_approval_rereads(const char *code);
#endif
