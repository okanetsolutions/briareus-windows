#include "screen_pulls_internal.h"
#include <commctrl.h>

// An opened review's handling (Mark handled or Reopen), and the editor that overrules one of its findings' verdict:
// Fix, Optional, Dismissed or none, how well it is established, and why. Save waits for what the review outcome asks
// (code_review_edit_can_save). The reason is a native multiline box over the page, as the Mac's is inline.

// Mark handled or Reopen under the review's identity line, Saving… while it is saved; nothing where it cannot change.
void screen_pulls_review_handling(PullScreen *s, Doc *doc, Col c, const Json *review) {
    CodeReviewHandlingAction action = screen_pulls_review_handling_action(review);
    if (action == CODE_REVIEW_HANDLING_NONE) return;
    PullReviews *r = &s->reviews;
    bool saving = r->saving_review == code_review_id(review);
    const char *title = saving ? "Saving\xE2\x80\xA6" : code_review_handling_title(action);
    ButtonSpec button = {action == CODE_REVIEW_MARK_HANDLED ? 0xE73E : 0xE7A7, title, BUTTON_BORDERED, ACT_REVIEW_HANDLING, (intptr_t)code_review_id(review), !screen_pulls_reviews_writing(r)};
    doc_space(doc, px(8));
    doc_button_row(doc, c.ix, c.iw, &button, 1);
}
static void caption(Doc *doc, Col c, const char *text) {
    doc_text(doc, c.ix, c.iw, text, FONT_CAPTION_SEMIBOLD, theme.secondary, DT_SINGLELINE);
    doc_space(doc, px(4));
}
static void hint(Doc *doc, Col c, const char *text) {
    if (!text) return;
    doc_space(doc, px(4));
    doc_text(doc, c.ix, c.iw, text, FONT_FOOTNOTE, theme.secondary, DT_WORDBREAK);
}
static size_t index_of(const char *const *choices, size_t count, const char *value) {
    for (size_t i = 0; i < count; i++) if (choices[i] == value) return i;
    return count - 1;
}
// The editor, when it is open on a finding of `review`: boxed above the findings, so the reason box scrolls with the page.
void screen_pulls_review_editor(PullScreen *s, Doc *doc, Col c, const Json *review) {
    PullReviews *r = &s->reviews;
    CodeReviewFindingEdit *e = r->editing;
    const Json *finding = e && e->review_id == code_review_id(review) ? code_review_finding(review, e->finding_id) : NULL;
    if (!finding) return;
    bool busy = screen_pulls_reviews_writing(r);
    int pad = px(12);
    doc_space(doc, px(14));
    int box = doc_box_begin(doc, c.ix, c.iw, pad, theme.canvas, theme.accent, px(8));
    Col in = {c.x, c.w, c.ix + pad, c.iw - 2 * pad};
    char *title = xstrfmt("Change verdict \xC2\xB7 Finding #%lld", (long long)e->finding_id);
    doc_text(doc, in.ix, in.iw, title, FONT_CAPTION_SEMIBOLD, theme.accent, DT_SINGLELINE); free(title);
    doc_text(doc, in.ix, in.iw, json_str(json_get(finding, "title")), FONT_SUBHEADLINE_SEMIBOLD, theme.ink, DT_WORDBREAK);
    doc_space(doc, px(10));
    const char *verdicts[CODE_REVIEW_VERDICT_CHOICES], *levels[CODE_REVIEW_VERIFICATION_CHOICES];
    for (size_t i = 0; i < CODE_REVIEW_VERDICT_CHOICES; i++) verdicts[i] = code_review_verdict_title(code_review_verdict_choices[i]);
    for (size_t i = 0; i < CODE_REVIEW_VERIFICATION_CHOICES; i++) levels[i] = code_review_verification_title(code_review_verification_choices[i]);
    caption(doc, in, "Verdict");
    doc_segments(doc, in.ix, in.iw, verdicts, CODE_REVIEW_VERDICT_CHOICES,
        (int)index_of(code_review_verdict_choices, CODE_REVIEW_VERDICT_CHOICES, e->verdict), ACT_REVIEW_VERDICT, 0, !busy);
    doc_space(doc, px(10));
    caption(doc, in, "Evidence");
    doc_segments(doc, in.ix, in.iw, levels, CODE_REVIEW_VERIFICATION_CHOICES,
        (int)index_of(code_review_verification_choices, CODE_REVIEW_VERIFICATION_CHOICES, e->verification), ACT_REVIEW_EVIDENCE, 0, !busy);
    hint(doc, in, code_review_edit_verification_hint(e));
    doc_space(doc, px(10));
    caption(doc, in, code_review_edit_reason_prompt(e));
    // The box's frame; the reason control sits inside it, placed by screen_pulls_review_editor_place.
    int field = doc_box_begin(doc, in.ix, in.iw, 0, theme.field, theme.line, px(6));
    r->reason_rc = (RECT){in.ix + px(8), doc->y + px(6), in.ix + in.iw - px(8), doc->y + px(84)};
    r->reason_laid = true;
    doc_space(doc, px(90)); doc_box_end(doc, field, 0);
    doc_item(doc, field)->hover_fill = false;
    hint(doc, in, code_review_edit_reason_hint(e));
    if (r->edit_error) { doc_space(doc, px(8)); doc_notice(doc, in.ix, in.iw, r->edit_error); }
    doc_space(doc, px(10));
    ButtonSpec buttons[] = {
        {0, busy ? "Saving\xE2\x80\xA6" : "Save", BUTTON_PROMINENT, ACT_REVIEW_SAVE, 0, !busy && code_reviews_allow_saving(r->items, e)},
        {0, "Cancel", BUTTON_BORDERED, ACT_REVIEW_CANCEL, 0, !busy},
    };
    Item *save = doc_item(doc, doc_button_row(doc, in.ix, in.iw, buttons, 2));
    save->tip = xstrdup("Save (Ctrl+Enter)");
    doc_box_end(doc, box, pad);
    doc_item(doc, box)->hover_fill = false;
}
// Ctrl+Enter saves and Escape cancels from inside the reason box; Enter starts a new line.
static LRESULT CALLBACK reason_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref) {
    PullScreen *s = (PullScreen *)ref;
    bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    if (msg == WM_KEYDOWN && wp == VK_RETURN && ctrl) { screen_pulls_review_edit_save(s); return 0; }
    if (msg == WM_KEYDOWN && wp == VK_ESCAPE) { screen_pulls_review_edit_cancel(s); return 0; }
    if (msg == WM_CHAR && (wp == '\n' || wp == VK_ESCAPE)) return 0;   // Ctrl+Enter arrives as a line feed
    if (msg == WM_NCDESTROY) RemoveWindowSubclass(hwnd, reason_proc, id);
    return DefSubclassProc(hwnd, msg, wp, lp);
}
// The control breaks lines with CRLF; the reason keeps plain line feeds, so an untouched reason reads as saved.
static void reason_text(HWND control, const char *text) {
    char *plain = str_replace(text ? text : "", "\r\n", "\n"), *crlf = str_replace(plain, "\n", "\r\n");
    wchar_t *wide = utf8_to_wide(crlf);
    SetWindowTextW(control, wide); free(wide); free(crlf); free(plain);
}
static bool reason_shown(const PullScreen *s, const RECT *content) {
    const PullReviews *r = &s->reviews;
    if (!content || !s->base.pane || !s->shown || s->tab != PR_TAB_CODE_REVIEWS) return false;
    return r->open && r->editing && r->reason_laid;
}
// Shows the reason box where the editor laid it out, clipped to the view below the pinned header; hides it otherwise.
void screen_pulls_review_editor_place(PullScreen *s, const RECT *content, int scroll_y) {
    PullReviews *r = &s->reviews;
    Pane *pane = s->base.pane;
    if (!reason_shown(s, content)) { if (r->reason) ShowWindow(r->reason, SW_HIDE); return; }
    HWND parent = pane_hwnd(pane);
    if (!r->reason) {
        r->reason = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_TABSTOP | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN,
            0, 0, 10, 10, parent, NULL, GetModuleHandleW(NULL), NULL);
        SetWindowSubclass(r->reason, reason_proc, 0, (DWORD_PTR)s);
        SendMessageW(r->reason, EM_SETLIMITTEXT, 10000, 0);
        theme_apply_control(r->reason);
        r->reason_for = 0;
    }
    if (GetParent(r->reason) != parent) SetParent(r->reason, parent);
    SendMessageW(r->reason, WM_SETFONT, (WPARAM)font(FONT_FOOTNOTE), TRUE);
    if (r->reason_for != r->editing->finding_id) { r->reason_for = r->editing->finding_id; reason_text(r->reason, r->editing->reason); }
    EnableWindow(r->reason, !screen_pulls_reviews_writing(r));
    RECT client; GetClientRect(parent, &client);
    int margin = (client.right - pane_content_width(pane)) / 2;
    RECT rc = r->reason_rc, view = *content, visible;
    OffsetRect(&rc, content->left + margin, content->top - scroll_y);
    if (pane_doc(pane)->pin_last) view.top += pane_doc(pane)->pin_bottom;
    if (!IntersectRect(&visible, &rc, &view)) { ShowWindow(r->reason, SW_HIDE); return; }
    MoveWindow(r->reason, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top, TRUE);
    SetWindowRgn(r->reason, CreateRectRgn(visible.left - rc.left, visible.top - rc.top, visible.right - rc.left, visible.bottom - rc.top), TRUE);
    ShowWindow(r->reason, SW_SHOWNA);
    if (r->focus_reason) { r->focus_reason = false; SetFocus(r->reason); }
}
// The reason as typed, so Save and the hints follow it.
void screen_pulls_review_editor_command(PullScreen *s, HWND control, int code) {
    PullReviews *r = &s->reviews;
    if (code != EN_CHANGE || !control || control != r->reason || !r->editing) return;
    int n = GetWindowTextLengthW(control);
    wchar_t *wide = xcalloc((size_t)n + 1, sizeof *wide); GetWindowTextW(control, wide, n + 1);
    char *text = wide_to_utf8(wide), *plain = str_replace(text, "\r\n", "\n"); free(wide); free(text);
    code_review_edit_set_reason(r->editing, plain); free(plain);
    pane_relayout(s->base.pane);
}
