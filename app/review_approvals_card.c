#include "review_approvals_card.h"
#include "review_outcome_card.h"
#include "str.h"
#include <commctrl.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

void approval_form_init(ApprovalForm *f, const ApprovalFormHost *host, void *owner, Screen *screen, const char *repo, int number,
    int action_base, bool risk_open) {
    memset(f, 0, sizeof *f);
    f->host = host; f->owner = owner; f->screen = screen;
    f->repo = xstrdup(repo); f->number = number; f->action_base = action_base; f->risk_open = risk_open;
}
void approval_form_clear(ApprovalForm *f) {
    request_cancel(&f->req);
    if (f->box) DestroyWindow(f->box);
    free(f->repo); free(f->error);
    memset(f, 0, sizeof *f);
}
bool approval_form_writing(const ApprovalForm *f) { return f->req != NULL; }
void approval_form_layout_begin(ApprovalForm *f) { f->box_laid = false; }
static bool busy(const ApprovalForm *f) { return f->req || (f->host && f->host->busy && f->host->busy(f->owner)); }
static void relayout(ApprovalForm *f) { if (f->screen) pane_relayout(f->screen->pane); }
static const ReviewApprovals *shown_approvals(const ApprovalForm *f) { return f->host && f->host->approvals ? f->host->approvals(f->owner) : NULL; }
static bool can_approve(const ReviewApprovals *a) { return a && store_supports("approve_pull") && review_approvals_can_approve(a, g_store.device.user_id); }
static bool can_withdraw(const ReviewApproval *a) { return a && store_supports("withdraw_pull_approval") && review_approval_can_withdraw(a, g_store.device.user_id); }
// The form is open on something it may still send: Approve while offered, or an approval its person may withdraw.
static bool form_allowed(const ApprovalForm *f, long long which) {
    const ReviewApprovals *a = shown_approvals(f);
    if (which == APPROVAL_FORM_APPROVE) return can_approve(a);
    return which && can_withdraw(review_approvals_find(a, which));
}

// MARK: Drawing

// "What set the risk", folded; unfolded, each reason with the tier it names.
static void risk(ApprovalForm *f, Doc *doc, int x, int w, const ReviewApprovals *a) {
    if (!a->risk_count) return;
    doc_space(doc, px(8));
    Item *toggle = doc_item(doc, doc_label(doc, x, w, f->risk_open ? 0xE70D : 0xE76C, "What set the risk", FONT_FOOTNOTE, theme.ink));
    toggle->action = f->action_base + APPROVAL_ACT_RISK; toggle->hand = true;
    if (!f->risk_open) return;
    for (size_t i = 0; i < a->risk_count; i++) {
        const RiskReason *reason = &a->risk[i];
        char *text = reason->tier ? xstrfmt("%s \xC2\xB7 %s", reason->tier, reason->message) : xstrdup(reason->message);
        doc_space(doc, px(3));
        review_line_draw(doc, x + px(22), w - px(22), text, FONT_FOOTNOTE, theme.ink);
        free(text);
    }
}
// The note, what sending it means, and the button that sends it; the box itself is placed over `box_rc`.
static void note_form(ApprovalForm *f, Doc *doc, int x, int w, const char *prompt, const char *consequence, const char *send, bool destructive) {
    bool held = busy(f);
    doc_space(doc, px(10));
    doc_text(doc, x, w, prompt, FONT_CAPTION_SEMIBOLD, theme.secondary, DT_SINGLELINE);
    doc_space(doc, px(4));
    int field = doc_box_begin(doc, x, w, 0, theme.field, theme.line, px(6));
    f->box_rc = (RECT){x + px(8), doc->y + px(6), x + w - px(8), doc->y + px(58)};
    f->box_laid = true;
    doc_space(doc, px(64)); doc_box_end(doc, field, 0);
    doc_item(doc, field)->hover_fill = false;
    doc_space(doc, px(6));
    review_line_draw(doc, x, w, consequence, FONT_FOOTNOTE, theme.secondary);
    doc_space(doc, px(8));
    ButtonSpec buttons[] = {
        {0, f->req && f->saving == f->open ? "Saving\xE2\x80\xA6" : send, destructive ? BUTTON_DESTRUCTIVE : BUTTON_PROMINENT, f->action_base + APPROVAL_ACT_SEND, 0, !held},
        {0, "Cancel", BUTTON_BORDERED, f->action_base + APPROVAL_ACT_CANCEL, 0, !held},
    };
    Item *first = doc_item(doc, doc_button_row(doc, x, w, buttons, 2));
    first->tip = xstrfmt("%s (Ctrl+Enter)", send);
}
// When it was given, in local time; owned, NULL when the server sent none.
static char *given(const ReviewApproval *a) {
    time_t when;
    return a->at && board_date_parse(a->at, &when) ? format_event_time(when) : NULL;
}
// One approval on record: who and whether it counts, where and for which head, when, its notes, and Withdraw.
static void approval_line(ApprovalForm *f, Doc *doc, int x, int w, const ReviewApproval *a, bool form_open) {
    COLORREF tone = review_tone_color(review_approval_tone(a));
    wchar_t glyph = a->counted ? 0xE73E : a->withdrawn ? 0xE7A7 : 0xE738;
    char *who = review_approval_who(a), *state = review_approval_state(a), *detail = review_approval_detail(a), *at = given(a);
    char *heading = xstrfmt("%s \xC2\xB7 %s", who, state);
    Item *it = doc_item(doc, doc_label(doc, x, w, glyph, heading, FONT_FOOTNOTE_SEMIBOLD, tone));
    it->tip = xstrdup(heading);
    int ix = x + px(22), iw = w - px(22);
    char *meta = *detail && at ? xstrfmt("%s \xC2\xB7 %s", detail, at) : xstrdup(*detail ? detail : at ? at : "");
    review_line_draw(doc, ix, iw, meta, FONT_CAPTION, theme.muted);
    review_line_draw(doc, ix, iw, a->note, FONT_FOOTNOTE, theme.ink);
    if (a->withdraw_note) {
        char *why = xstrfmt("Withdrawn: %s", a->withdraw_note);
        review_line_draw(doc, ix, iw, why, FONT_FOOTNOTE, theme.secondary); free(why);
    }
    free(who); free(state); free(detail); free(at); free(heading); free(meta);
    if (form_open && f->open == a->id) { note_form(f, doc, ix, iw, "Why (optional)", review_approval_withdraw_consequence(), "Withdraw approval", true); return; }
    if (!can_withdraw(a)) return;
    ButtonSpec withdraw = {0xE7A7, "Withdraw", BUTTON_BORDERED, f->action_base + APPROVAL_ACT_WITHDRAW, (intptr_t)a->id, !busy(f)};
    doc_space(doc, px(4));
    doc_button_row(doc, ix, iw, &withdraw, 1);
}
void approval_form_layout(ApprovalForm *f, Doc *doc, int x, int w, const ReviewApprovals *a) {
    if (!a || !review_approvals_shown(a)) return;
    bool form_open = form_allowed(f, f->open);
    int pad = px(12);
    int box = doc_box_begin(doc, x, w, pad, theme.canvas, theme.line, px(8));
    int ix = x + pad, iw = w - 2 * pad;
    review_fact_draw(doc, ix, iw, "Approvals", review_approvals_line(a), review_approvals_tone(a));
    if (a->explanation) { doc_space(doc, px(6)); review_line_draw(doc, ix, iw, a->explanation, FONT_FOOTNOTE, theme.secondary); }
    review_reasons_draw(doc, ix, iw, &a->reasons, px(4));
    risk(f, doc, ix, iw, a);
    if (form_open && f->open == APPROVAL_FORM_APPROVE) {
        char *consequence = review_approvals_approve_consequence(a);
        note_form(f, doc, ix, iw, "Note (optional)", consequence, "Approve this revision", false);
        free(consequence);
    } else if (can_approve(a)) {
        ButtonSpec approve = {0xE8FB, "Approve", BUTTON_BORDERED, f->action_base + APPROVAL_ACT_APPROVE, 0, !busy(f)};
        doc_space(doc, px(10));
        doc_button_row(doc, ix, iw, &approve, 1);
    }
    if (a->count) { doc_space(doc, px(10)); doc_rule(doc, ix, iw); }
    for (size_t i = 0; i < a->count; i++) { doc_space(doc, px(8)); approval_line(f, doc, ix, iw, &a->items[i], form_open); }
    if (f->error) { doc_space(doc, px(8)); doc_notice(doc, ix, iw, f->error); }
    doc_box_end(doc, box, pad);
    doc_item(doc, box)->hover_fill = false;
    doc_space(doc, px(14));
}

// MARK: Writing

static void close_form(ApprovalForm *f) {
    bool focused = f->box && GetFocus() == f->box;
    // The box starts empty for whichever form opens next, so what it shows is what is sent.
    f->open = 0; f->box_for = 0;
    if (f->box) SetWindowTextW(f->box, L"");
    if (focused && f->screen && f->screen->pane) SetFocus(pane_hwnd(f->screen->pane));
}
static void open_form(ApprovalForm *f, long long which) {
    if (busy(f) || !form_allowed(f, which)) return;
    if (f->open != which) close_form(f);
    f->open = which; f->focus_box = true;
    set_string(&f->error, NULL);
}
// The note as typed in the box, plain line feeds; owned.
static char *note_text(const ApprovalForm *f) {
    if (!f->box) return xstrdup("");
    int n = GetWindowTextLengthW(f->box);
    wchar_t *wide = xcalloc((size_t)n + 1, sizeof *wide); GetWindowTextW(f->box, wide, n + 1);
    char *text = wide_to_utf8(wide), *plain = str_replace(text, "\r\n", "\n");
    free(wide); free(text); return plain;
}
static void send_write(ApprovalForm *f) {
    if (busy(f) || !form_allowed(f, f->open) || !f->host || !f->host->done) return;
    const ReviewApprovals *a = shown_approvals(f);
    char *note = note_text(f);
    bool approve = f->open == APPROVAL_FORM_APPROVE;
    Json *args = approve ? review_approval_approve_args(f->repo, f->number, a->head_sha, note)
                         : review_approval_withdraw_args(f->repo, f->number, f->open, note);
    free(note);
    if (f->host->sending) f->host->sending(f->owner);
    f->saving = f->open; set_string(&f->error, NULL);
    store_call(approve ? "approve_pull" : "withdraw_pull_approval", args, 0, f->owner, f->host->done, 0, &f->req);
}
bool approval_form_action(ApprovalForm *f, int action, intptr_t arg) {
    int which = action - f->action_base;
    if (which < 0 || which >= APPROVAL_ACTIONS) return false;
    switch (which) {
    case APPROVAL_ACT_APPROVE: open_form(f, APPROVAL_FORM_APPROVE); break;
    case APPROVAL_ACT_WITHDRAW: if (arg > 0) open_form(f, (long long)arg); break;
    case APPROVAL_ACT_SEND: send_write(f); break;
    case APPROVAL_ACT_CANCEL:
        if (busy(f) || !f->open) break;
        close_form(f); set_string(&f->error, NULL);
        if (f->screen && f->screen->pane) SetFocus(pane_hwnd(f->screen->pane));
        break;
    case APPROVAL_ACT_RISK: f->risk_open = !f->risk_open; break;
    }
    relayout(f);
    return true;
}
// A failed write's message: a refusal as said, anything else noting that it may have been saved all the same.
static void failure(ApprovalForm *f, const Request *req) {
    char *text = request_error_text(req);
    if (request_outcome_unknown(req)) {
        char *said = xstrfmt("%s It may still have been saved; refresh before trying again.", text);
        free(text); text = said;
    }
    free(f->error); f->error = text;
}
ApprovalSent approval_form_finish(ApprovalForm *f, const Request *req) {
    long long sent = f->saving;
    f->saving = 0;
    if (req->ok) { if (f->open == sent) close_form(f); set_string(&f->error, NULL); relayout(f); return APPROVAL_LANDED; }
    failure(f, req);
    relayout(f);
    return review_approval_rereads(req->error.code) ? APPROVAL_STALE : APPROVAL_FAILED;
}

// MARK: The note box

// Ctrl+Enter sends and Escape cancels from inside the box; Enter starts a new line.
static LRESULT CALLBACK box_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref) {
    ApprovalForm *f = (ApprovalForm *)ref;
    bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    if (msg == WM_KEYDOWN && wp == VK_RETURN && ctrl) { approval_form_action(f, f->action_base + APPROVAL_ACT_SEND, 0); return 0; }
    if (msg == WM_KEYDOWN && wp == VK_ESCAPE) { approval_form_action(f, f->action_base + APPROVAL_ACT_CANCEL, 0); return 0; }
    if (msg == WM_CHAR && (wp == '\n' || wp == VK_ESCAPE)) return 0;   // Ctrl+Enter arrives as a line feed
    if (msg == WM_NCDESTROY) RemoveWindowSubclass(hwnd, box_proc, id);
    return DefSubclassProc(hwnd, msg, wp, lp);
}
void approval_form_place(ApprovalForm *f, const RECT *content, int scroll_y, bool shown) {
    Pane *pane = f->screen ? f->screen->pane : NULL;
    if (!shown || !content || !pane || !f->open || !f->box_laid) { if (f->box) ShowWindow(f->box, SW_HIDE); return; }
    if (!f->box) {
        f->box = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_TABSTOP | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN,
            0, 0, 10, 10, pane_hwnd(pane), NULL, GetModuleHandleW(NULL), NULL);
        SetWindowSubclass(f->box, box_proc, 0, (DWORD_PTR)f);
        SendMessageW(f->box, EM_SETLIMITTEXT, REVIEW_APPROVAL_NOTE_LIMIT, 0);
        theme_apply_control(f->box);
        f->box_for = 0;
    }
    SendMessageW(f->box, WM_SETFONT, (WPARAM)font(FONT_FOOTNOTE), TRUE);
    // Another form starts empty; the same one keeps what was typed across layouts.
    if (f->box_for != f->open) { f->box_for = f->open; SetWindowTextW(f->box, L""); }
    EnableWindow(f->box, !f->req);
    if (!pane_place_clipped(pane, f->box, &f->box_rc, content, scroll_y)) return;
    if (f->focus_box) { f->focus_box = false; SetFocus(f->box); }
}
