#include "screen_pulls_internal.h"

// The Code reviews tab's two writes: Mark handled or Reopen a stopped review, and a finding's verdict overruled in the
// editor. Each answer is folded into the list as read (code_reviews.c); neither is retried.

CodeReviewHandlingAction screen_pulls_review_handling_action(const Json *review) {
    return store_supports("update_code_review") ? code_review_handling_action(review) : CODE_REVIEW_HANDLING_NONE;
}
bool screen_pulls_review_can_edit(const Json *review) {
    return store_supports("update_code_review_finding") && code_review_can_edit_findings(review);
}
// A failed write's message: a refusal as said, anything else noting that it may have been saved all the same.
static void failure(char **slot, const Request *req) {
    char *text = request_error_text(req);
    if (request_outcome_unknown(req)) {
        char *said = xstrfmt("%s It may still have been saved; refresh before trying again.", text);
        free(text); text = said;
    }
    free(*slot); *slot = text;
}
// The answer is for the reviews shown; otherwise they are read again for the connection that is current now.
static bool answered(PullScreen *s, const Request *req) {
    if (screen_pulls_reviews_current(s, req)) return true;
    screen_pulls_reviews_sync(s); pane_relayout(s->base.pane);
    return false;
}
static void handling_done(void *owner, Request *req) {
    PullScreen *s = owner; PullReviews *r = &s->reviews;
    r->saving_review = 0;
    if (!answered(s, req)) return;
    if (!req->ok) failure(&r->save_error, req);
    // An answer that cannot be folded in is read again in full.
    else if (!code_reviews_replace_review(r->items, json_get(req->result, "review"))) screen_pulls_reviews_load(s, true);
    pane_relayout(s->base.pane);
}
static void verdict_done(void *owner, Request *req) {
    PullScreen *s = owner; PullReviews *r = &s->reviews;
    if (!answered(s, req)) return;
    if (!req->ok) { failure(&r->edit_error, req); pane_relayout(s->base.pane); return; }
    int64_t review = (int64_t)json_num_or(json_get(req->args, "reviewId"), 0), finding = (int64_t)json_num_or(json_get(req->args, "findingId"), 0);
    if (!code_reviews_replace_finding(r->items, review, json_get(req->result, "finding"))) screen_pulls_reviews_load(s, true);
    // A changed verdict or evidence changes the review outcome at once; the rounds stay as read.
    screen_pulls_review_outcome_take(r, req->result, REVIEW_OUTCOME_FROM_VERDICT);
    // The editor closes once the server took it; one opened on another finding meanwhile stays.
    if (r->editing && r->editing->finding_id == finding) { code_review_edit_free(r->editing); free(r->editing); r->editing = NULL; }
    pane_relayout(s->base.pane);
}
// A read under way may predate the write and would put back what it changes, so it gives way.
static void send_write(PullScreen *s, const char *call, Json *args, RequestDone done) {
    PullReviews *r = &s->reviews;
    request_cancel(&r->req);
    set_string(&r->save_error, NULL); set_string(&r->edit_error, NULL);
    store_call(call, args, 0, s, done, 0, &r->req_write);
    pane_relayout(s->base.pane);
}
void screen_pulls_review_set_handled(PullScreen *s, int64_t review_id) {
    PullReviews *r = &s->reviews;
    CodeReviewHandlingAction action = screen_pulls_review_handling_action(code_review_find(r->items, review_id));
    if (r->req_write || action == CODE_REVIEW_HANDLING_NONE) return;
    r->saving_review = review_id;
    send_write(s, "update_code_review", code_review_handling_args(s->project.repo, s->number, review_id, action), handling_done);
}
void screen_pulls_review_edit(PullScreen *s, int64_t finding_id) {
    PullReviews *r = &s->reviews;
    const Json *review = code_review_find(r->items, r->review_id), *finding = code_review_finding(review, finding_id);
    if (r->req_write || !finding || !screen_pulls_review_can_edit(review)) return;
    if (!r->editing) r->editing = xcalloc(1, sizeof *r->editing);
    else code_review_edit_free(r->editing);
    code_review_edit_init(r->editing, review, finding);
    set_string(&r->edit_error, NULL);
    r->focus_reason = true; r->reason_for = 0;
    pane_relayout(s->base.pane);
}
void screen_pulls_review_edit_cancel(PullScreen *s) {
    PullReviews *r = &s->reviews;
    if (r->req_write || !r->editing) return;
    code_review_edit_free(r->editing); free(r->editing); r->editing = NULL;
    set_string(&r->edit_error, NULL);
    if (s->base.pane) SetFocus(pane_hwnd(s->base.pane));
    pane_relayout(s->base.pane);
}
void screen_pulls_review_edit_save(PullScreen *s) {
    PullReviews *r = &s->reviews;
    if (r->req_write || !r->editing || !store_supports("update_code_review_finding")) return;
    if (!code_reviews_allow_saving(r->items, r->editing)) return;
    send_write(s, "update_code_review_finding", code_review_edit_args(r->editing, s->project.repo, s->number), verdict_done);
}
