#include "screen_status_internal.h"

// Status's pull requests waiting on a person's approval, approved in place (the Mac's StatusReviewApproval): each reads
// its own review outcome, drawn with its security pass, and the approvals card under it approves or withdraws with what
// set the risk shown. Once a write lands attention is read again, so a pull request with all its approvals leaves.

static const ReviewApprovals *approvals_of(void *owner) {
    const StatusApproval *a = owner;
    return a->outcome ? &a->outcome->approvals : NULL;
}
static bool no_other_write(void *owner) { (void)owner; return false; }
static void before_write(void *owner) { request_cancel(&((StatusApproval *)owner)->req); }
static void approval_done(void *owner, Request *req);
static const ApprovalFormHost host = {approvals_of, no_other_write, before_write, approval_done};

static void read_done(void *owner, Request *req) {
    StatusApproval *a = owner; StatusScreen *s = a->screen;
    if (!status_current(s)) return;
    a->read = true;
    // A failed read stays owed and is tried again with the next attention read.
    if (req->ok) { review_outcome_keep(&a->outcome_state, &a->outcome, req->result, REVIEW_OUTCOME_FROM_READ); set_string(&a->error, NULL); a->stale = false; }
    else request_error_into(&a->error, req);
    pane_relayout(s->base.pane);
}
// Its review outcome, unless a read or its write is under way, so a write's answer is not overwritten by an older read.
static void read_review(StatusApproval *a) {
    if (a->req || approval_form_writing(&a->form) || !a->screen->shown) return;
    if (!store_supports("code_reviews")) {
        a->read = true; set_string(&a->error, "This server or token cannot read the pull request's code review."); return;
    }
    Json *args = json_object(); json_set_str(args, "repo", a->repo); json_set_num(args, "pr", a->number);
    store_call("code_reviews", args, 0, a, read_done, 0, &a->req);
}
static void approval_done(void *owner, Request *req) {
    StatusApproval *a = owner; StatusScreen *s = a->screen;
    if (!status_current(s)) return;
    switch (approval_form_finish(&a->form, req)) {
    case APPROVAL_LANDED:
        review_outcome_keep(&a->outcome_state, &a->outcome, req->result, REVIEW_OUTCOME_FROM_APPROVAL);
        // An attention read under way may predate the write: attention is read again after it.
        request_cancel(&s->attention_read); status_attention_load(s, true);
        break;
    case APPROVAL_STALE: read_review(a); break;
    case APPROVAL_FAILED: break;
    }
    pane_relayout(s->base.pane);
}

static void item_free(StatusApproval *a) {
    request_cancel(&a->req); approval_form_clear(&a->form);
    review_outcome_drop(&a->outcome_state, &a->outcome);
    free(a->repo); free(a->summary); free(a->error); free(a);
}
static bool same_pull(const StatusApproval *a, const PullAttention *pull) { return a->number == pull->number && str_eq(a->repo, pull->repo); }
StatusApproval *status_approval_for(StatusScreen *s, const PullAttention *pull) {
    for (size_t i = 0; i < s->approval_count; i++) if (s->approvals[i] && same_pull(s->approvals[i], pull)) return s->approvals[i];
    return NULL;
}
// One kept for `pull`, moved out of the old list, or a new one.
static StatusApproval *take(StatusScreen *s, const PullAttention *pull) {
    for (size_t i = 0; i < s->approval_count; i++) {
        StatusApproval *a = s->approvals[i];
        if (!a || !same_pull(a, pull)) continue;
        s->approvals[i] = NULL;
        // A changed summary says the requirement moved: its review is read again.
        if (!str_eq(a->summary, pull->summary)) { a->stale = true; set_string(&a->summary, pull->summary); }
        return a;
    }
    StatusApproval *a = xcalloc(1, sizeof *a);
    a->screen = s; a->stale = true; a->repo = xstrdup(pull->repo); a->summary = xstrdup(pull->summary); a->number = pull->number;
    approval_form_init(&a->form, &host, a, &s->base, pull->repo, pull->number, STATUS_APPROVALS_BASE, true);
    return a;
}
void status_approvals_merge(StatusScreen *s) {
    StatusApproval **next = xcalloc(s->pull_count + s->approval_count + 1, sizeof *next);
    size_t n = 0;
    for (size_t i = 0; i < s->pull_count; i++) if (s->pulls[i].kind == PULL_ATTENTION_APPROVAL) next[n++] = take(s, &s->pulls[i]);
    // One gone from the list stays while its write is under way; its answer is the person's to see.
    for (size_t i = 0; i < s->approval_count; i++) {
        StatusApproval *a = s->approvals[i];
        if (!a) continue;
        if (approval_form_writing(&a->form)) next[n++] = a;
        else item_free(a);
    }
    free(s->approvals); s->approvals = next; s->approval_count = n;
    for (size_t i = 0; i < n; i++) if (next[i]->stale) read_review(next[i]);
}
void status_approval_layout(StatusScreen *s, Doc *doc, int x, int w, StatusApproval *a) {
    size_t index = 0;
    while (index < s->approval_count && s->approvals[index] != a) index++;
    a->form.action_base = STATUS_APPROVALS_BASE + (int)index * APPROVAL_ACTIONS;
    approval_form_layout_begin(&a->form);
    doc_space(doc, px(8));
    if (a->error) doc_notice(doc, x, w, a->error);
    if (!a->read && !a->error) doc_loading(doc, x, w, "Loading the review\xE2\x80\xA6");
    if (!a->outcome) return;
    review_outcome_card(doc, x, w, a->outcome, true);
    approval_form_layout(&a->form, doc, x, w, &a->outcome->approvals);
}
void status_approvals_place(StatusScreen *s, const RECT *content, int scroll_y) {
    bool shown = content && s->shown && status_current(s);
    for (size_t i = 0; i < s->approval_count; i++) approval_form_place(&s->approvals[i]->form, content, scroll_y, shown);
}
bool status_approvals_action(StatusScreen *s, int action, intptr_t arg) {
    for (size_t i = 0; i < s->approval_count; i++) if (approval_form_action(&s->approvals[i]->form, action, arg)) return true;
    return false;
}
// Reads stop with the screen and are owed again when it shows; a write under way finishes, since its outcome is the
// person's to know.
void status_approvals_cancel(StatusScreen *s) {
    for (size_t i = 0; i < s->approval_count; i++) {
        StatusApproval *a = s->approvals[i];
        if (a->req) { request_cancel(&a->req); a->stale = true; }
    }
    status_approvals_place(s, NULL, 0);
}
void status_approvals_free(StatusScreen *s) {
    for (size_t i = 0; i < s->approval_count; i++) item_free(s->approvals[i]);
    free(s->approvals); s->approvals = NULL; s->approval_count = 0;
}
