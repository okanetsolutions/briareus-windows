#include "screen_pulls_internal.h"

// What the pull request's code review amounts to, at the top of the Code reviews tab (review_outcome_card.c), and the
// approvals its risk asks for under it (review_approvals_card.c), whose Approve and Withdraw are the tab's one write while
// they are under way.

// Keeps the outcome and rounds `answer` carries, as `source` decides, beside the reviews.
void screen_pulls_review_outcome_take(PullReviews *r, const Json *answer, ReviewOutcomeSource source) {
    Json *state = review_outcome_state(answer, source, r->outcome_state);
    json_free(r->outcome_state); r->outcome_state = state;
    review_outcome_free(r->outcome); free(r->outcome); r->outcome = xcalloc(1, sizeof *r->outcome);
    if (!review_outcome_parse(state, r->outcome)) { free(r->outcome); r->outcome = NULL; }
}
void screen_pulls_review_outcome_clear(PullReviews *r) {
    json_free(r->outcome_state); r->outcome_state = NULL;
    review_outcome_free(r->outcome); free(r->outcome); r->outcome = NULL;
}

static const ReviewApprovals *approvals_shown(void *owner) {
    PullScreen *s = owner;
    return s->reviews.outcome ? &s->reviews.outcome->approvals : NULL;
}
static bool other_write(void *owner) { return ((PullScreen *)owner)->reviews.req_write != NULL; }
// A read under way may predate the approval and would put back what it changes; another write's refusal is now old.
static void before_approval(void *owner) {
    PullReviews *r = &((PullScreen *)owner)->reviews;
    request_cancel(&r->req); set_string(&r->save_error, NULL); set_string(&r->edit_error, NULL);
}
static void approval_done(void *owner, Request *req) {
    PullScreen *s = owner; PullReviews *r = &s->reviews;
    if (!screen_pulls_reviews_current(s, req)) { screen_pulls_reviews_sync(s); pane_relayout(s->base.pane); return; }
    switch (approval_form_finish(&r->approvals, req)) {
    case APPROVAL_LANDED: screen_pulls_review_outcome_take(r, req->result, review_outcome_source_after(req->operation)); break;
    // The head moved on or no ended review covers it: the outcome is read again so Approve follows the server's head.
    case APPROVAL_STALE: screen_pulls_reviews_load(s, true); break;
    case APPROVAL_FAILED: break;
    }
    pane_relayout(s->base.pane);
}
static const ApprovalFormHost approval_host = {approvals_shown, other_write, before_approval, approval_done};
void screen_pulls_review_approvals_init(PullScreen *s) {
    approval_form_init(&s->reviews.approvals, &approval_host, s, &s->base, s->project.repo, s->number, ACT_APPROVAL_BASE, false);
}

// The cards, while the server has sent an outcome; nothing from an older server.
void screen_pulls_review_outcome(PullScreen *s, Doc *doc, Col c) {
    const ReviewOutcome *o = s->reviews.outcome;
    if (!o) return;
    review_outcome_card(doc, c.ix, c.iw, o);
    approval_form_layout(&s->reviews.approvals, doc, c.ix, c.iw, &o->approvals);
}
