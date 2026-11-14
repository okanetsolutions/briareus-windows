#include "screen_pulls_internal.h"
#include <commctrl.h>
#include <time.h>

// The human approvals a pull request's risk asks for, under the review outcome on its Code reviews tab (the Mac's
// ReviewApprovalsCard): how many count of how many, why the requirement stands as it does, what set the risk tier, every
// approval on record with whether it counts, and Approve and Withdraw for a token that may record them, each with an
// optional note in a form under it. The note is a native box over the page, as the verdict editor's reason is. The
// words and tones are review_approvals.c's; the writes are screen_pulls_pull_review_writes.c's.

// "What set the risk", folded; unfolded, each reason with the tier it names.
static void risk(PullScreen *s, Doc *doc, Col c, const ReviewApprovals *a) {
    if (!a->risk_count) return;
    bool open = s->reviews.risk_open;
    doc_space(doc, px(8));
    Item *toggle = doc_item(doc, doc_label(doc, c.ix, c.iw, open ? 0xE70D : 0xE76C, "What set the risk", FONT_FOOTNOTE, theme.ink));
    toggle->action = ACT_REVIEW_RISK; toggle->hand = true;
    if (!open) return;
    for (size_t i = 0; i < a->risk_count; i++) {
        const RiskReason *reason = &a->risk[i];
        char *text = reason->tier ? xstrfmt("%s \xC2\xB7 %s", reason->tier, reason->message) : xstrdup(reason->message);
        doc_space(doc, px(3));
        screen_pulls_review_line(doc, c.ix + px(22), c.iw - px(22), text, FONT_FOOTNOTE, theme.ink);
        free(text);
    }
}
// The note, what sending it means, and the button that sends it, for approving or withdrawing.
static void form(PullScreen *s, Doc *doc, Col c, const char *prompt, const char *consequence, const char *action, bool destructive) {
    PullReviews *r = &s->reviews;
    bool busy = r->req_write != NULL;
    doc_space(doc, px(10));
    doc_text(doc, c.ix, c.iw, prompt, FONT_CAPTION_SEMIBOLD, theme.secondary, DT_SINGLELINE);
    doc_space(doc, px(4));
    int field = doc_box_begin(doc, c.ix, c.iw, 0, theme.field, theme.line, px(6));
    r->note_rc = (RECT){c.ix + px(8), doc->y + px(6), c.ix + c.iw - px(8), doc->y + px(58)};
    r->note_laid = true;
    doc_space(doc, px(64)); doc_box_end(doc, field, 0);
    doc_item(doc, field)->hover_fill = false;
    doc_space(doc, px(6));
    screen_pulls_review_line(doc, c.ix, c.iw, consequence, FONT_FOOTNOTE, theme.secondary);
    doc_space(doc, px(8));
    bool saving = busy && r->approval_saving == r->approval_form;
    ButtonSpec buttons[] = {
        {0, saving ? "Saving\xE2\x80\xA6" : action, destructive ? BUTTON_DESTRUCTIVE : BUTTON_PROMINENT, ACT_REVIEW_APPROVAL_SEND, 0, !busy},
        {0, "Cancel", BUTTON_BORDERED, ACT_REVIEW_APPROVAL_CANCEL, 0, !busy},
    };
    Item *send = doc_item(doc, doc_button_row(doc, c.ix, c.iw, buttons, 2));
    send->tip = xstrfmt("%s (Ctrl+Enter)", action);
}
// When it was given, in local time; owned, NULL when the server sent none.
static char *given(const ReviewApproval *a) {
    time_t when;
    return a->at && board_date_parse(a->at, &when) ? format_event_time(when) : NULL;
}
// One approval on record: who and whether it counts, where and for which head, when, its notes, and Withdraw.
static void approval(PullScreen *s, Doc *doc, Col c, const ReviewApproval *a, bool form_open) {
    PullReviews *r = &s->reviews;
    COLORREF tone = screen_pulls_review_tone_color(review_approval_tone(a));
    wchar_t glyph = a->counted ? 0xE73E : a->withdrawn ? 0xE7A7 : 0xE738;
    char *who = review_approval_who(a), *state = review_approval_state(a), *detail = review_approval_detail(a), *at = given(a);
    char *heading = xstrfmt("%s \xC2\xB7 %s", who, state);
    Item *it = doc_item(doc, doc_label(doc, c.ix, c.iw, glyph, heading, FONT_FOOTNOTE_SEMIBOLD, tone));
    it->tip = xstrdup(heading);
    Col in = {c.x, c.w, c.ix + px(22), c.iw - px(22)};
    char *meta = *detail && at ? xstrfmt("%s \xC2\xB7 %s", detail, at) : xstrdup(*detail ? detail : at ? at : "");
    screen_pulls_review_line(doc, in.ix, in.iw, meta, FONT_CAPTION, theme.muted);
    screen_pulls_review_line(doc, in.ix, in.iw, a->note, FONT_FOOTNOTE, theme.ink);
    if (a->withdraw_note) {
        char *why = xstrfmt("Withdrawn: %s", a->withdraw_note);
        screen_pulls_review_line(doc, in.ix, in.iw, why, FONT_FOOTNOTE, theme.secondary); free(why);
    }
    free(who); free(state); free(detail); free(at); free(heading); free(meta);
    bool open = form_open && r->approval_form == a->id;
    if (open) { form(s, doc, in, "Why (optional)", review_approval_withdraw_consequence(), "Withdraw approval", true); return; }
    if (!screen_pulls_review_can_withdraw(a)) return;
    ButtonSpec withdraw = {0xE7A7, "Withdraw", BUTTON_BORDERED, ACT_REVIEW_WITHDRAW, (intptr_t)a->id, !r->req_write};
    doc_space(doc, px(4));
    doc_button_row(doc, in.ix, in.iw, &withdraw, 1);
}
// The block, while a policy asks for approvals or some are on record; nothing from an older server.
void screen_pulls_review_approvals(PullScreen *s, Doc *doc, Col c) {
    PullReviews *r = &s->reviews;
    const ReviewApprovals *a = r->outcome ? &r->outcome->approvals : NULL;
    if (!a || !review_approvals_shown(a)) return;
    bool form_open = screen_pulls_review_approval_form_open(s);
    int pad = px(12);
    int box = doc_box_begin(doc, c.ix, c.iw, pad, theme.canvas, theme.line, px(8));
    Col in = {c.x, c.w, c.ix + pad, c.iw - 2 * pad};
    screen_pulls_review_fact(doc, in, "Approvals", review_approvals_line(a), review_approvals_tone(a));
    if (a->explanation) { doc_space(doc, px(6)); screen_pulls_review_line(doc, in.ix, in.iw, a->explanation, FONT_FOOTNOTE, theme.secondary); }
    screen_pulls_review_reasons(doc, in, &a->reasons, px(4));
    risk(s, doc, in, a);
    if (form_open && r->approval_form == REVIEW_APPROVE_FORM) {
        char *consequence = review_approvals_approve_consequence(a);
        form(s, doc, in, "Note (optional)", consequence, "Approve this revision", false);
        free(consequence);
    } else if (screen_pulls_review_can_approve(s)) {
        ButtonSpec approve = {0xE8FB, "Approve", BUTTON_BORDERED, ACT_REVIEW_APPROVE, 0, !r->req_write};
        doc_space(doc, px(10));
        doc_button_row(doc, in.ix, in.iw, &approve, 1);
    }
    if (a->count) { doc_space(doc, px(10)); doc_rule(doc, in.ix, in.iw); }
    for (size_t i = 0; i < a->count; i++) { doc_space(doc, px(8)); approval(s, doc, in, &a->items[i], form_open); }
    if (r->approval_error) { doc_space(doc, px(8)); doc_notice(doc, in.ix, in.iw, r->approval_error); }
    doc_box_end(doc, box, pad);
    doc_item(doc, box)->hover_fill = false;
    doc_space(doc, px(14));
}

// Ctrl+Enter sends and Escape cancels from inside the note box; Enter starts a new line.
static LRESULT CALLBACK note_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref) {
    PullScreen *s = (PullScreen *)ref;
    bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    if (msg == WM_KEYDOWN && wp == VK_RETURN && ctrl) { screen_pulls_review_approval_send(s); return 0; }
    if (msg == WM_KEYDOWN && wp == VK_ESCAPE) { screen_pulls_review_approval_cancel(s); return 0; }
    if (msg == WM_CHAR && (wp == '\n' || wp == VK_ESCAPE)) return 0;   // Ctrl+Enter arrives as a line feed
    if (msg == WM_NCDESTROY) RemoveWindowSubclass(hwnd, note_proc, id);
    return DefSubclassProc(hwnd, msg, wp, lp);
}
static bool note_shown(const PullScreen *s, const RECT *content) {
    const PullReviews *r = &s->reviews;
    if (!content || !s->base.pane || !s->shown || s->tab != PR_TAB_CODE_REVIEWS) return false;
    return !r->open && r->approval_form && r->note_laid;
}
// Shows the note box where the open form laid it out; a form opened on something else starts with an empty note. The
// note is read from the box when it is sent.
void screen_pulls_review_approvals_place(PullScreen *s, const RECT *content, int scroll_y) {
    PullReviews *r = &s->reviews;
    if (!note_shown(s, content)) { if (r->note) ShowWindow(r->note, SW_HIDE); return; }
    if (!r->note) {
        r->note = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_TABSTOP | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN,
            0, 0, 10, 10, pane_hwnd(s->base.pane), NULL, GetModuleHandleW(NULL), NULL);
        SetWindowSubclass(r->note, note_proc, 0, (DWORD_PTR)s);
        SendMessageW(r->note, EM_SETLIMITTEXT, REVIEW_APPROVAL_NOTE_LIMIT, 0);
        theme_apply_control(r->note);
    }
    SendMessageW(r->note, WM_SETFONT, (WPARAM)font(FONT_FOOTNOTE), TRUE);
    if (r->note_for != r->approval_form) { r->note_for = r->approval_form; SetWindowTextW(r->note, L""); }
    EnableWindow(r->note, !r->req_write);
    if (screen_pulls_review_box_place(s, r->note, &r->note_rc, content, scroll_y) && r->focus_note) { r->focus_note = false; SetFocus(r->note); }
}
