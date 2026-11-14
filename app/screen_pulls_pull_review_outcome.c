#include "screen_pulls_internal.h"

// What the pull request's code review amounts to, at the top of the Code reviews tab (the Mac's ReviewOutcomeCard): the
// outcome and the rounds used, whether it is ready, stale or still to reach GitHub, why, and the security pass. The
// approvals the change's risk asks for follow in their own block (screen_pulls_pull_review_approvals.c). The words and
// tones are review_outcome.c's.

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
COLORREF screen_pulls_review_tone_color(ReviewTone tone) {
    switch (tone) {
    case REVIEW_TONE_RUNNING: return theme.accent;
    case REVIEW_TONE_OK: return theme.ok;
    case REVIEW_TONE_WARN: return theme.warn;
    case REVIEW_TONE_DANGER: return theme.danger;
    default: return theme.secondary;
    }
}
static wchar_t outcome_glyph(ReviewOutcomeKind kind) {
    switch (kind) {
    case REVIEW_OUTCOME_PASSED: return 0xE73E;
    case REVIEW_OUTCOME_CHANGES_REQUIRED: return 0xE711;
    case REVIEW_OUTCOME_INCOMPLETE: return 0xE7BA;
    default: return 0xE823;
    }
}
void screen_pulls_review_reasons(Doc *doc, Col c, const ReviewReasons *list, int gap) {
    if (!list->count) return;
    const ReviewReason **listed = xcalloc(list->count, sizeof *listed);
    review_reasons_listed(list, listed);
    doc_space(doc, gap);
    for (size_t i = 0; i < list->count; i++) {
        COLORREF color = review_reason_is_note(listed[i]) ? theme.secondary : screen_pulls_review_tone_color(review_reason_tone(listed[i]));
        doc_text(doc, c.ix, c.iw, listed[i]->message, FONT_FOOTNOTE, color, DT_WORDBREAK);
        if (i + 1 < list->count) doc_space(doc, px(4));
    }
    free(listed);
}
void screen_pulls_review_line(Doc *doc, int x, int w, const char *text, FontId f, COLORREF color) {
    if (text && *text) doc_text(doc, x, w, text, f, color, DT_WORDBREAK);
}
// A titled fact, "Security  Passed · required", on a line of its own.
static void fact(Doc *doc, Col c, const char *title, char *value, ReviewTone tone) {
    doc_space(doc, px(8));
    int top = doc->y, tw = text_width(doc->cv, title, FONT_FOOTNOTE_SEMIBOLD) + px(8);
    RECT t = {c.ix, top, c.ix + tw, top + font_height(doc->cv, FONT_FOOTNOTE_SEMIBOLD)};
    doc_text_at(doc, &t, title, FONT_FOOTNOTE_SEMIBOLD, theme.ink, DT_SINGLELINE);
    doc->y = top;
    doc_text(doc, c.ix + tw, c.iw - tw, value, FONT_FOOTNOTE, screen_pulls_review_tone_color(tone), DT_WORDBREAK);
    if (doc->y < t.bottom) doc->y = t.bottom;
    free(value);
}
static void heading(Doc *doc, Col c, const ReviewOutcome *o) {
    int top = doc->y, tw = text_width(doc->cv, "Review outcome", FONT_FOOTNOTE_SEMIBOLD) + px(10);
    BadgeSpec badge = {outcome_glyph(o->outcome), review_outcome_title(o->outcome), screen_pulls_review_tone_color(review_outcome_tone(o->outcome)), false};
    int bw = doc_badges_width(doc, &badge, 1);
    int h = font_height(doc->cv, FONT_FOOTNOTE_SEMIBOLD);
    RECT t = {c.ix, top, c.ix + tw, top + h};
    doc_text_at(doc, &t, "Review outcome", FONT_FOOTNOTE_SEMIBOLD, theme.ink, DT_SINGLELINE);
    doc->y = top;
    Item *it = doc_item(doc, doc_badges(doc, c.ix + tw, bw, &badge, 1, theme.canvas));
    it->text = xstrfmt("Review outcome: %s", badge.text); it->tip = xstrdup(it->text); it->cell = true;
    int bottom = doc->y > t.bottom ? doc->y : t.bottom;
    if (o->rounds.present) {
        char *line = review_rounds_line(&o->rounds);
        int lw = text_width(doc->cv, line, FONT_CAPTION) + px(2), x = c.ix + c.iw - lw;
        if (x < c.ix + tw + bw + px(10)) { x = c.ix; top = bottom + px(4); }
        RECT r = {x, top, x + lw, top + font_height(doc->cv, FONT_CAPTION)};
        doc_text_at(doc, &r, line, FONT_CAPTION, screen_pulls_review_tone_color(review_rounds_tone(&o->rounds)), DT_SINGLELINE);
        if (r.bottom > bottom) bottom = r.bottom;
        free(line);
    }
    doc->y = bottom;
}
// The card, while the server has sent an outcome; nothing from an older server.
void screen_pulls_review_outcome(PullScreen *s, Doc *doc, Col c) {
    const ReviewOutcome *o = s->reviews.outcome;
    if (!o) return;
    int pad = px(12);
    int box = doc_box_begin(doc, c.ix, c.iw, pad, theme.canvas, theme.line, px(8));
    Col in = {c.x, c.w, c.ix + pad, c.iw - 2 * pad};
    heading(doc, in, o);
    char *status = review_outcome_status_note(o);
    const char *notes[] = {review_outcome_ready_note(o), review_outcome_stale_note(o), status};
    for (size_t i = 0; i < 3; i++) if (notes[i]) {
        doc_space(doc, px(6));
        doc_text(doc, in.ix, in.iw, notes[i], FONT_FOOTNOTE, theme.secondary, DT_WORDBREAK);
    }
    free(status);
    screen_pulls_review_reasons(doc, in, &o->reasons, px(8));
    if (review_security_shown(&o->security)) {
        fact(doc, in, "Security", review_security_line(&o->security), review_security_tone(&o->security));
        screen_pulls_review_reasons(doc, in, &o->security.reasons, px(2));
    }
    doc_box_end(doc, box, pad);
    doc_item(doc, box)->hover_fill = false;
    doc_space(doc, px(14));
}
