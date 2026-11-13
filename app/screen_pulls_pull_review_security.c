#include "screen_pulls_internal.h"
#include "review_security.h"

// A security pass in full on the Code reviews tab: the pull request's as its own block under the review outcome, and an
// opened review's own as a section folded under its result. Both say the result under the policy, the risk tier that
// made the pass required, the stage and head it covered, how its findings stand, why it is not clean and the findings it
// recorded, then what it examined per risk category and the earlier claims it checked. The words and tones are
// review_security.c's.

static void caption(Doc *doc, Col c, const char *text) {
    doc_space(doc, px(10));
    doc_text(doc, c.ix, c.iw, text, FONT_CAPTION_SEMIBOLD, theme.secondary, DT_SINGLELINE);
}
// Each finding the pass recorded: its severity and title, where it is, and the earlier claim it raises again.
static void entries(Doc *doc, Col c, const ReviewSecurity *sec) {
    if (!sec->entry_count) return;
    int recorded = sec->recorded > (int)sec->entry_count ? sec->recorded : (int)sec->entry_count;
    char *title = xstrfmt("Findings the pass recorded (%d)", recorded);
    caption(doc, c, title); free(title);
    for (size_t i = 0; i < sec->entry_count; i++) {
        const SecurityEntry *e = &sec->entries[i];
        doc_space(doc, px(4));
        if (e->severity) screen_pulls_review_badge(doc, c.ix, c.iw, e->severity, true, theme.canvas);
        doc_text(doc, c.ix, c.iw, e->title, FONT_FOOTNOTE_SEMIBOLD, theme.ink, DT_WORDBREAK);
        char *location = review_security_entry_location(e), *again = e->claim ? xstrfmt("Raises %s again", e->claim) : NULL;
        review_line_draw(doc, c.ix, c.iw, location, FONT_CAPTION, theme.muted);
        review_line_draw(doc, c.ix, c.iw, again, FONT_CAPTION, theme.secondary);
        free(location); free(again);
    }
}
// The risk tier, the stage and head, how the findings stand, why, and the findings recorded.
static void overview(Doc *doc, Col c, const ReviewSecurity *sec) {
    char *risk = review_security_risk_note(sec), *scope = review_security_scope_line(sec);
    char *findings = review_security_findings_line(sec), *claims = review_security_claims_line(sec);
    review_line_draw(doc, c.ix, c.iw, risk, FONT_FOOTNOTE, theme.secondary);
    review_line_draw(doc, c.ix, c.iw, scope, FONT_CAPTION, theme.muted);
    review_line_draw(doc, c.ix, c.iw, findings, FONT_FOOTNOTE, theme.ink);
    review_line_draw(doc, c.ix, c.iw, claims, FONT_FOOTNOTE, theme.secondary);
    free(risk); free(scope); free(findings); free(claims);
    review_reasons_draw(doc, c.ix, c.iw, &sec->reasons, px(4));
    entries(doc, c, sec);
}
// What the pass examined per risk category, the earlier claims it checked, and what an incremental pass widened to.
static void coverage(Doc *doc, Col c, const ReviewSecurity *sec) {
    for (size_t i = 0; i < sec->category_count; i++) {
        const SecurityCategory *category = &sec->categories[i];
        char *title = review_security_category_title(category);
        doc_space(doc, px(6));
        review_fact_draw(doc, c.ix, c.iw, title, review_security_category_line(category), review_security_category_tone(category));
        free(title);
        review_line_draw(doc, c.ix, c.iw, category->assessment, FONT_FOOTNOTE, theme.secondary);
    }
    if (sec->claim_count) caption(doc, c, "Earlier claims checked");
    for (size_t i = 0; i < sec->claim_count; i++) {
        char *text = review_security_claim_line(&sec->claims[i]);
        doc_space(doc, px(4));
        doc_text(doc, c.ix, c.iw, text, FONT_FOOTNOTE, review_tone_color(review_security_claim_tone(&sec->claims[i])), DT_WORDBREAK);
        free(text);
    }
    if (!sec->widened) return;
    char *widened = xstrfmt("Widened to: %s", sec->widened);
    doc_space(doc, px(8)); review_line_draw(doc, c.ix, c.iw, widened, FONT_FOOTNOTE, theme.secondary); free(widened);
}
static bool examined(const ReviewSecurity *sec) { return sec->category_count || sec->claim_count || sec->widened; }
// A disclosure line; ACT_REVIEW_SECURITY with `arg` folds or unfolds it.
static void toggle(Doc *doc, Col c, bool open, const char *title, FontId f, COLORREF color, int64_t arg) {
    Item *it = doc_item(doc, doc_label(doc, c.ix, c.iw, open ? 0xE70D : 0xE76C, title, f, color));
    it->action = ACT_REVIEW_SECURITY; it->arg = (intptr_t)arg; it->hand = true;
}
// The pull request's block, while the project asks for a pass at all; nothing from an older server.
void screen_pulls_review_security(PullScreen *s, Doc *doc, Col c) {
    const ReviewOutcome *o = s->reviews.outcome;
    if (!o || !review_security_shown(&o->security)) return;
    const ReviewSecurity *sec = &o->security;
    int pad = px(12);
    int box = doc_box_begin(doc, c.ix, c.iw, pad, theme.canvas, theme.line, px(8));
    Col in = {c.x, c.w, c.ix + pad, c.iw - 2 * pad};
    review_fact_draw(doc, in.ix, in.iw, "Security", review_security_line(sec), review_security_tone(sec));
    doc_space(doc, px(4));
    overview(doc, in, sec);
    if (examined(sec)) {
        doc_space(doc, px(10));
        toggle(doc, in, s->reviews.security_open, "What the pass examined", FONT_FOOTNOTE, theme.ink, 0);
        Col inner = {in.x, in.w, in.ix + px(22), in.iw - px(22)};
        if (s->reviews.security_open) coverage(doc, inner, sec);
    }
    doc_box_end(doc, box, pad);
    doc_item(doc, box)->hover_fill = false;
    doc_space(doc, px(14));
}
// An opened review's own pass, "Security review · Failed · required", unfolded while `security_id` names the review.
void screen_pulls_review_security_of(PullScreen *s, Doc *doc, Col c, const Json *review) {
    ReviewSecurity sec;
    if (!review_security_of_review(review, &sec)) return;
    int64_t id = code_review_id(review);
    bool open = s->reviews.security_id == id;
    char *line = review_security_line(&sec), *title = xstrfmt("Security review \xC2\xB7 %s", line);
    doc_space(doc, px(10));
    toggle(doc, c, open, title, FONT_SUBHEADLINE_SEMIBOLD, review_tone_color(review_security_tone(&sec)), id);
    free(line); free(title);
    if (open) {
        Col inner = {c.x, c.w, c.ix + px(22), c.iw - px(22)};
        doc_space(doc, px(8));
        overview(doc, inner, &sec);
        if (sec.category_count) caption(doc, inner, "Coverage");
        coverage(doc, inner, &sec);
    }
    review_security_free(&sec);
}
