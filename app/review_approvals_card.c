#include "review_approvals_card.h"
#include "review_outcome_card.h"
#include "str.h"
#include <commctrl.h>
#include <stdlib.h>
#include <string.h>

void approval_form_init(ApprovalForm *f, const ApprovalFormHost *host, void *owner, Screen *screen, const char *repo, int number,
    int action_base, bool risk_open) {
    memset(f, 0, sizeof *f);
    f->host = host; f->owner = owner; f->screen = screen;
    f->repo = xstrdup(repo); f->number = number; f->action_base = action_base; f->risk_open = risk_open;
}
void approval_form_clear(ApprovalForm *f) {
    request_cancel(&f->req);
    if (f->box) DestroyWindow(f->box);
    free(f->repo); free(f->note); free(f->error);
    memset(f, 0, sizeof *f);
}
bool approval_form_writing(const ApprovalForm *f) { return f->req != NULL; }
void approval_form_layout_begin(ApprovalForm *f) { f->box_laid = false; }
static bool busy(const ApprovalForm *f) { return f->req || (f->host && f->host->busy && f->host->busy(f->owner)); }
static void relayout(ApprovalForm *f) { if (f->screen) pane_relayout(f->screen->pane); }
static const ReviewApprovals *shown_approvals(ApprovalForm *f) { return f->host && f->host->approvals ? f->host->approvals(f->owner) : NULL; }
static bool can_approve(const ReviewApprovals *a) { return a && store_supports("approve_pull") && review_approvals_can_approve(a, g_store.device.user_id); }
static bool can_withdraw(const ReviewApproval *a) { return store_supports("withdraw_pull_approval") && review_approval_can_withdraw(a, g_store.device.user_id); }
static const ReviewApproval *approval_of(const ReviewApprovals *a, int64_t id) {
    for (size_t i = 0; a && i < a->count; i++) if (a->items[i].id == id) return &a->items[i];
    return NULL;
}

// MARK: Drawing

static void action_text(Doc *doc, int item, int action, intptr_t arg) {
    Item *it = doc_item(doc, item); it->action = action; it->arg = arg; it->hand = true;
}
// What set the risk tier, folded under a disclosure line.
static void risk(ApprovalForm *f, Doc *doc, int x, int w, const ReviewApprovals *a) {
    if (!a->risk_count) return;
    doc_space(doc, px(8));
    action_text(doc, doc_label(doc, x, w, f->risk_open ? 0xE70D : 0xE76C, "What set the risk", FONT_FOOTNOTE, theme.ink), f->action_base + APPROVAL_ACT_RISK, 0);
    if (!f->risk_open) return;
    for (size_t i = 0; i < a->risk_count; i++) {
        const RiskReason *r = &a->risk[i];
        char *line = r->tier ? xstrfmt("%s \xC2\xB7 %s", r->tier, r->message) : xstrdup(r->message);
        doc_space(doc, px(4));
        doc_text(doc, x + px(22), w - px(22), line, FONT_FOOTNOTE, theme.ink, DT_WORDBREAK);
        free(line);
    }
}
// The optional note, what sending it means, and the button that sends it; the box itself is placed over `box_rc`.
static void note_form(ApprovalForm *f, Doc *doc, int x, int w, const char *prompt, const char *consequence, const char *send, bool destructive) {
    doc_space(doc, px(10));
    doc_text(doc, x, w, prompt, FONT_CAPTION_SEMIBOLD, theme.secondary, DT_SINGLELINE);
    doc_space(doc, px(4));
    int field = doc_box_begin(doc, x, w, 0, theme.field, theme.line, px(6));
    f->box_rc = (RECT){x + px(8), doc->y + px(6), x + w - px(8), doc->y + px(58)};
    f->box_laid = true;
    doc_space(doc, px(64)); doc_box_end(doc, field, 0);
    doc_item(doc, field)->hover_fill = false;
    doc_space(doc, px(6));
    doc_text(doc, x, w, consequence, FONT_FOOTNOTE, theme.secondary, DT_WORDBREAK);
    doc_space(doc, px(8));
    bool held = busy(f);
    ButtonSpec buttons[] = {
        {0, f->req ? "Saving\xE2\x80\xA6" : send, destructive ? BUTTON_DESTRUCTIVE : BUTTON_PROMINENT, f->action_base + APPROVAL_ACT_SEND, 0, !held},
        {0, "Cancel", BUTTON_BORDERED, f->action_base + APPROVAL_ACT_CANCEL, 0, !held},
    };
    Item *first = doc_item(doc, doc_button_row(doc, x, w, buttons, 2));
    first->tip = xstrfmt("%s (Ctrl+Enter)", send);
}
static wchar_t approval_glyph(const ReviewApproval *a) { return a->counted ? 0xE73E : a->withdrawn ? 0xE7A7 : 0xE738; }
// One approval on record: who and whether it counts (or why not), where and for which head, when, its note, why it was
// withdrawn, and Withdraw for its approver.
static void approval_line(ApprovalForm *f, Doc *doc, int x, int w, const ReviewApproval *a) {
    int ix = x + px(22), iw = w - px(22);
    char *who = review_approval_who(a), *state = review_approval_state(a);
    char *line = xstrfmt("%s \xC2\xB7 %s", who, state);
    COLORREF tone = review_tone_color(review_approval_tone(a));
    int top = doc->y;
    wchar_t glyph[2] = {approval_glyph(a), 0};
    char *g = wide_to_utf8(glyph);
    RECT mark = {x, top, x + px(16), top + font_height(doc->cv, FONT_FOOTNOTE)};
    doc_text_at(doc, &mark, g, FONT_ICON_SMALL, tone, DT_SINGLELINE); free(g);
    doc->y = top;
    doc_text(doc, ix, iw, line, FONT_FOOTNOTE_SEMIBOLD, a->counted ? theme.ink : theme.secondary, DT_WORDBREAK);
    free(who); free(state); free(line);
    char *detail = review_approval_detail(a), *when = NULL;
    time_t at;
    if (a->at && board_date_parse(a->at, &at)) when = format_event_time(at);
    char *meta = *detail && when ? xstrfmt("%s \xC2\xB7 %s", detail, when) : xstrdup(*detail ? detail : when ? when : "");
    if (*meta) doc_text(doc, ix, iw, meta, FONT_CAPTION, theme.secondary, DT_WORDBREAK);
    free(detail); free(when); free(meta);
    if (a->note) { doc_space(doc, px(2)); doc_text(doc, ix, iw, a->note, FONT_FOOTNOTE, theme.ink, DT_WORDBREAK); }
    char *withdrawn = review_approval_withdraw_line(a);
    if (withdrawn) { doc_space(doc, px(2)); doc_text(doc, ix, iw, withdrawn, FONT_FOOTNOTE, theme.secondary, DT_WORDBREAK); free(withdrawn); }
    if (!can_withdraw(a)) return;
    if (f->open == a->id) {
        note_form(f, doc, ix, iw, "Why (optional)", review_approval_withdraw_consequence(), "Withdraw approval", true);
        return;
    }
    doc_space(doc, px(6));
    ButtonSpec button = {0xE7A7, "Withdraw", BUTTON_BORDERED, f->action_base + APPROVAL_ACT_WITHDRAW, (intptr_t)a->id, !busy(f)};
    doc_button_row(doc, ix, iw, &button, 1);
}
void approval_form_layout(ApprovalForm *f, Doc *doc, int x, int w, const ReviewApprovals *a) {
    if (!a || !review_approvals_shown(a)) return;
    int pad = px(12);
    int box = doc_box_begin(doc, x, w, pad, theme.canvas, theme.line, px(8));
    int ix = x + pad, iw = w - 2 * pad;
    review_fact_draw(doc, ix, iw, "Approvals", review_approvals_line(a), review_approvals_tone(a));
    if (a->explanation) { doc_space(doc, px(6)); doc_text(doc, ix, iw, a->explanation, FONT_FOOTNOTE, theme.secondary, DT_WORDBREAK); }
    review_reasons_draw(doc, ix, iw, &a->reasons, px(4));
    risk(f, doc, ix, iw, a);
    if (can_approve(a)) {
        if (f->open == APPROVAL_FORM_APPROVE) {
            char *consequence = review_approvals_approve_consequence(a);
            note_form(f, doc, ix, iw, "Note (optional)", consequence, "Approve this revision", false);
            free(consequence);
        } else {
            doc_space(doc, px(10));
            ButtonSpec button = {0xE8FB, "Approve", BUTTON_BORDERED, f->action_base + APPROVAL_ACT_APPROVE, 0, !busy(f)};
            doc_button_row(doc, ix, iw, &button, 1);
        }
    }
    if (a->count) {
        doc_space(doc, px(10)); doc_rule(doc, ix, iw); doc_space(doc, px(10));
        for (size_t i = 0; i < a->count; i++) {
            if (i) doc_space(doc, px(8));
            approval_line(f, doc, ix, iw, &a->items[i]);
        }
    }
    if (f->error) { doc_space(doc, px(8)); doc_notice(doc, ix, iw, f->error); }
    doc_box_end(doc, box, pad);
    doc_item(doc, box)->hover_fill = false;
    doc_space(doc, px(14));
}

// MARK: Writing

static void close_form(ApprovalForm *f) {
    bool focused = f->box && GetFocus() == f->box;
    f->open = 0; set_string(&f->note, NULL);
    if (focused && f->screen && f->screen->pane) SetFocus(pane_hwnd(f->screen->pane));
}
static void open_form(ApprovalForm *f, int64_t which) {
    if (busy(f)) return;
    f->open = which; set_string(&f->note, NULL); set_string(&f->error, NULL);
    f->focus_box = true;
}
static void send_write(ApprovalForm *f) {
    const ReviewApprovals *a = shown_approvals(f);
    if (busy(f) || !f->open || !f->host || !f->host->done) return;
    Json *args = NULL; const char *call = NULL;
    if (f->open == APPROVAL_FORM_APPROVE && can_approve(a)) {
        call = "approve_pull"; args = review_approval_approve_args(f->repo, f->number, a->head_sha, f->note);
    } else {
        const ReviewApproval *approval = approval_of(a, f->open);
        if (!approval || !can_withdraw(approval)) return;
        call = "withdraw_pull_approval"; args = review_approval_withdraw_args(f->repo, f->number, approval->id, f->note);
    }
    if (f->host->sending) f->host->sending(f->owner);
    f->saving = f->open; set_string(&f->error, NULL);
    store_call(call, args, 0, f->owner, f->host->done, 0, &f->req);
}
bool approval_form_action(ApprovalForm *f, int action, intptr_t arg) {
    int which = action - f->action_base;
    if (which < 0 || which >= APPROVAL_ACTIONS) return false;
    switch (which) {
    case APPROVAL_ACT_APPROVE: open_form(f, APPROVAL_FORM_APPROVE); break;
    case APPROVAL_ACT_WITHDRAW: if (arg > 0) open_form(f, (int64_t)arg); break;
    case APPROVAL_ACT_SEND: send_write(f); break;
    case APPROVAL_ACT_CANCEL: if (!busy(f)) close_form(f); break;
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
    f->saving = 0;
    if (req->ok) { close_form(f); set_string(&f->error, NULL); relayout(f); return APPROVAL_LANDED; }
    failure(f, req);
    relayout(f);
    return review_approval_rereads(req->error.code) ? APPROVAL_STALE : APPROVAL_FAILED;
}

// MARK: The note box

// Ctrl+Enter sends and Escape cancels from inside the box; Enter starts a new line.
static LRESULT CALLBACK box_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref) {
    ApprovalForm *f = (ApprovalForm *)ref;
    bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    if (msg == WM_KEYDOWN && wp == VK_RETURN && ctrl) { send_write(f); relayout(f); return 0; }
    if (msg == WM_KEYDOWN && wp == VK_ESCAPE) { if (!busy(f)) close_form(f); relayout(f); return 0; }
    if (msg == WM_CHAR && (wp == '\n' || wp == VK_ESCAPE)) return 0;   // Ctrl+Enter arrives as a line feed
    if (msg == WM_NCDESTROY) RemoveWindowSubclass(hwnd, box_proc, id);
    return DefSubclassProc(hwnd, msg, wp, lp);
}
void approval_form_place(ApprovalForm *f, const RECT *content, int scroll_y, bool shown) {
    Pane *pane = f->screen ? f->screen->pane : NULL;
    if (!shown || !content || !pane || !f->open || !f->box_laid) { if (f->box) ShowWindow(f->box, SW_HIDE); return; }
    HWND parent = pane_hwnd(pane);
    if (!f->box) {
        f->box = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_TABSTOP | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN,
            0, 0, 10, 10, parent, NULL, GetModuleHandleW(NULL), NULL);
        SetWindowSubclass(f->box, box_proc, 0, (DWORD_PTR)f);
        SendMessageW(f->box, EM_SETLIMITTEXT, REVIEW_APPROVAL_NOTE_LIMIT, 0);
        theme_apply_control(f->box);
        f->box_for = 0;
    }
    if (GetParent(f->box) != parent) SetParent(f->box, parent);
    SendMessageW(f->box, WM_SETFONT, (WPARAM)font(FONT_FOOTNOTE), TRUE);
    // Another form starts empty; the same one keeps what was typed across layouts.
    if (f->box_for != f->open) { f->box_for = f->open; SetWindowTextW(f->box, L""); }
    EnableWindow(f->box, !f->req);
    RECT client; GetClientRect(parent, &client);
    int margin = (client.right - pane_content_width(pane)) / 2;
    RECT rc = f->box_rc, view = *content, visible;
    OffsetRect(&rc, content->left + margin, content->top - scroll_y);
    if (pane_doc(pane)->pin_last) view.top += pane_doc(pane)->pin_bottom;
    if (!IntersectRect(&visible, &rc, &view)) { ShowWindow(f->box, SW_HIDE); return; }
    MoveWindow(f->box, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top, TRUE);
    SetWindowRgn(f->box, CreateRectRgn(visible.left - rc.left, visible.top - rc.top, visible.right - rc.left, visible.bottom - rc.top), TRUE);
    ShowWindow(f->box, SW_SHOWNA);
    if (f->focus_box) { f->focus_box = false; SetFocus(f->box); }
}
void approval_form_command(ApprovalForm *f, HWND control, int code) {
    if (code != EN_CHANGE || !control || control != f->box || !f->open) return;
    int n = GetWindowTextLengthW(control);
    wchar_t *wide = xcalloc((size_t)n + 1, sizeof *wide); GetWindowTextW(control, wide, n + 1);
    char *text = wide_to_utf8(wide), *plain = str_replace(text, "\r\n", "\n"); free(wide); free(text);
    free(f->note); f->note = plain;
}
