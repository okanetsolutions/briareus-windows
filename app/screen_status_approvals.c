#include "screen_status_internal.h"
#include <math.h>

// ◉ Status's pull requests waiting on a person's approval (the Mac's StatusReviewApproval): the `attention` inbox names
// them, each reads its own review outcome, and the approvals card under it approves or withdraws in place, with what set
// the risk shown. Once a write lands the inbox is read again, so a pull request with all its approvals leaves the list.

static const ReviewApprovals *approvals_of(void *owner) {
    StatusApproval *a = owner;
    return a->outcome ? &a->outcome->approvals : NULL;
}
static bool no_other_write(void *owner) { (void)owner; return false; }
static void before_write(void *owner) { request_cancel(&((StatusApproval *)owner)->req); }
static void approval_done(void *owner, Request *req);
static const ApprovalFormHost host = {approvals_of, no_other_write, before_write, approval_done};

static void take(StatusApproval *a, const Json *answer, ReviewOutcomeSource source) {
    Json *state = review_outcome_state(answer, source, a->outcome_state);
    json_free(a->outcome_state); a->outcome_state = state;
    review_outcome_free(a->outcome); free(a->outcome); a->outcome = xcalloc(1, sizeof *a->outcome);
    if (!review_outcome_parse(state, a->outcome)) { free(a->outcome); a->outcome = NULL; }
}
static void read_done(void *owner, Request *req) {
    StatusApproval *a = owner; StatusScreen *s = a->screen;
    if (!status_current(s) || req->client != s->client) return;
    a->read = true;
    // A failed read stays owed and is tried again with the next inbox read.
    if (req->ok) { take(a, req->result, REVIEW_OUTCOME_FROM_READ); set_string(&a->error, NULL); a->stale = false; }
    else request_error_into(&a->error, req);
    pane_relayout(s->base.pane);
}
// Its review outcome, unless a read or its write is under way; a write's answer is not overwritten by an older read. It
// stays owed (`stale`) until a read lands, so one cancelled by hiding Status or by a write is read again.
static void read_review(StatusApproval *a) {
    if (a->req || approval_form_writing(&a->form)) return;
    if (!store_supports("code_reviews")) {
        a->read = true; set_string(&a->error, "This server or token cannot read the pull request's code review."); return;
    }
    Json *args = json_object(); json_set_str(args, "repo", a->pull.repo); json_set_num(args, "pr", a->pull.number);
    store_call("code_reviews", args, 0, a, read_done, 0, &a->req);
}
static void approval_done(void *owner, Request *req) {
    StatusApproval *a = owner; StatusScreen *s = a->screen;
    if (!status_current(s) || req->client != s->client) return;
    switch (approval_form_finish(&a->form, req)) {
    case APPROVAL_LANDED:
        take(a, req->result, review_outcome_source_after(req->operation));
        // An inbox read under way may predate the write: the inbox is read again after it.
        request_cancel(&s->attention_read); s->attention_retry_at = 0; status_approvals_load(s, false);
        break;
    case APPROVAL_STALE: read_review(a); break;
    case APPROVAL_FAILED: if (a->stale) read_review(a); break;
    }
    pane_relayout(s->base.pane);
}

static void item_free(StatusApproval *a) {
    request_cancel(&a->req); approval_form_clear(&a->form);
    json_free(a->outcome_state); review_outcome_free(a->outcome); free(a->outcome); free(a->error);
    free(a->pull.id); free(a->pull.repo); free(a->pull.title); free(a->pull.summary); free(a->pull.at);
    free(a);
}
static void copy_pull(AttentionPull *into, const AttentionPull *from) {
    free(into->id); free(into->repo); free(into->title); free(into->summary); free(into->at);
    into->id = xstrdup(from->id); into->repo = xstrdup(from->repo); into->title = xstrdup(from->title);
    into->summary = xstrdup(from->summary); into->at = xstrdup(from->at); into->number = from->number;
}
static StatusApproval *take_item(StatusScreen *s, const AttentionPull *pull) {
    for (size_t i = 0; i < s->approval_count; i++) {
        StatusApproval *a = s->approvals[i];
        if (!a || !attention_pull_same(&a->pull, pull)) continue;
        s->approvals[i] = NULL;
        // A changed summary says the requirement moved: its review is read again.
        if (!str_eq(a->pull.summary, pull->summary)) a->stale = true;
        copy_pull(&a->pull, pull);
        return a;
    }
    StatusApproval *a = xcalloc(1, sizeof *a);
    a->screen = s; a->stale = true; copy_pull(&a->pull, pull);
    approval_form_init(&a->form, &host, a, &s->base, pull->repo, pull->number, STATUS_APPROVALS_BASE, true);
    return a;
}
// The inbox's pull requests in its order, keeping each one's state; one gone from it stays while its write is under way.
static void merge(StatusScreen *s, const AttentionPull *pulls, size_t count) {
    StatusApproval **next = xcalloc(count + s->approval_count + 1, sizeof *next);
    size_t n = 0;
    for (size_t i = 0; i < count; i++) { next[n] = take_item(s, &pulls[i]); next[n++]->present = true; }
    for (size_t i = 0; i < s->approval_count; i++) {
        StatusApproval *a = s->approvals[i];
        if (!a) continue;
        if (approval_form_writing(&a->form)) { a->present = false; next[n++] = a; }
        else item_free(a);
    }
    free(s->approvals); s->approvals = next; s->approval_count = n;
    for (size_t i = 0; i < n; i++) if (next[i]->stale) read_review(next[i]);
}
static void attention_done(void *owner, Request *req) {
    StatusScreen *s = owner;
    if (!status_current(s)) return;
    if (req->ok) {
        size_t count; AttentionPull *pulls = attention_approval_pulls(req->result, &count);
        merge(s, pulls, count); attention_pulls_free(pulls, count);
        set_string(&s->attention_error, NULL); s->attention_retry_at = 0;
    } else {
        request_error_into(&s->attention_error, req);
        if (req->error.retry_after > 0) s->attention_retry_at = time(NULL) + (time_t)ceil(req->error.retry_after);
    }
    pane_relayout(s->base.pane);
}
void status_approvals_load(StatusScreen *s, bool fresh) {
    if (!status_current(s) || !store_supports("attention")) return;
    if (fresh) for (size_t i = 0; i < s->approval_count; i++) s->approvals[i]->stale = true;
    if (s->attention_read || (!fresh && time(NULL) < s->attention_retry_at)) return;
    store_call("attention", json_object(), 0, s, attention_done, 0, &s->attention_read);
}
// Reads stop with the screen; a write under way finishes, since its outcome is the person's to know.
void status_approvals_cancel(StatusScreen *s) {
    request_cancel(&s->attention_read);
    for (size_t i = 0; i < s->approval_count; i++) request_cancel(&s->approvals[i]->req);
    status_approvals_place(s, NULL, 0);
}
void status_approvals_free(StatusScreen *s) {
    request_cancel(&s->attention_read);
    for (size_t i = 0; i < s->approval_count; i++) item_free(s->approvals[i]);
    free(s->approvals); s->approvals = NULL; s->approval_count = 0; free(s->attention_error); s->attention_error = NULL;
}

static bool has_project(StatusScreen *s, const char *repo) {
    for (size_t i = 0; i < s->project_count; i++) if (str_eq(s->projects[i].repo, repo)) return true;
    return false;
}
static void pull_heading(StatusScreen *s, Doc *doc, int x, int w, size_t index) {
    const AttentionPull *p = &s->approvals[index]->pull;
    char *title = p->title ? xstrfmt("%s #%d \xC2\xB7 %s", p->repo, p->number, p->title) : xstrfmt("%s #%d", p->repo, p->number);
    int i = doc_text(doc, x, w, title, FONT_SUBHEADLINE, theme.ink, DT_WORDBREAK); free(title);
    if (has_project(s, p->repo)) { Item *it = doc_item(doc, i); it->action = STATUS_APPROVAL_PULL; it->arg = (intptr_t)index; it->hand = true; }
    if (p->summary) doc_text(doc, x, w, p->summary, FONT_FOOTNOTE, theme.secondary, DT_WORDBREAK);
}
void status_approvals_layout(StatusScreen *s, Doc *doc, int x, int w) {
    if (!s->approval_count && !s->attention_error) return;
    doc_section(doc, x, w, "Approval needed");
    if (s->attention_error) doc_notice(doc, x, w, s->attention_error);
    for (size_t i = 0; i < s->approval_count; i++) {
        StatusApproval *a = s->approvals[i];
        a->form.action_base = STATUS_APPROVALS_BASE + (int)i * APPROVAL_ACTIONS;
        approval_form_layout_begin(&a->form);
        pull_heading(s, doc, x, w, i);
        doc_space(doc, px(10));
        if (a->error) doc_notice(doc, x, w, a->error);
        if (!a->read && !a->error) doc_loading(doc, x, w, "Loading the review\xE2\x80\xA6");
        if (a->outcome) {
            review_outcome_card(doc, x, w, a->outcome);
            approval_form_layout(&a->form, doc, x, w, &a->outcome->approvals);
        }
        doc_space(doc, px(10));
    }
    doc_space(doc, px(10));
}
void status_approvals_place(StatusScreen *s, const RECT *content, int scroll_y) {
    bool shown = content && s->shown && status_current(s);
    for (size_t i = 0; i < s->approval_count; i++) approval_form_place(&s->approvals[i]->form, content, scroll_y, shown);
}
void status_approvals_command(StatusScreen *s, HWND control, int code) {
    for (size_t i = 0; i < s->approval_count; i++) approval_form_command(&s->approvals[i]->form, control, code);
}
bool status_approvals_action(StatusScreen *s, int action, intptr_t arg) {
    if (action == STATUS_APPROVAL_PULL) {
        if (arg < 0 || (size_t)arg >= s->approval_count) return true;
        const AttentionPull *p = &s->approvals[arg]->pull;
        for (size_t i = 0; i < s->project_count; i++) if (str_eq(s->projects[i].repo, p->repo)) {
            app_push_from(&s->base, pull_detail_screen_new(&s->projects[i], p->number, NULL, NULL)); break;
        }
        return true;
    }
    for (size_t i = 0; i < s->approval_count; i++) if (approval_form_action(&s->approvals[i]->form, action, arg)) return true;
    return false;
}
