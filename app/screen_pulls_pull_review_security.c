#include "screen_pulls_internal.h"
#include "review_security.h"

// The pull request's security pass in full, under the review outcome on its Code reviews tab: its result under the
// policy, the risk tier that made it required, the stage and head it covered, how its findings stand, why it is not
// clean, the findings it recorded, and folded under them what it examined per risk category. The words and tones are
// review_security.c's.

// Each finding the pass recorded: its severity and title, and where it is.
static void entries(Doc *doc, Col c, const ReviewSecurity *sec) {
    if (!sec->entry_count) return;
    doc_space(doc, px(10));
    doc_text(doc, c.ix, c.iw, "Findings the pass recorded", FONT_CAPTION_SEMIBOLD, theme.secondary, DT_SINGLELINE);
    for (size_t i = 0; i < sec->entry_count; i++) {
        const SecurityEntry *e = &sec->entries[i];
        doc_space(doc, px(4));
        if (e->severity) screen_pulls_review_badge(doc, c.ix, c.iw, e->severity, true, theme.canvas);
        doc_text(doc, c.ix, c.iw, e->title, FONT_FOOTNOTE_SEMIBOLD, theme.ink, DT_WORDBREAK);
        char *location = review_security_entry_location(e);
        review_line_draw(doc, c.ix, c.iw, location, FONT_CAPTION, theme.muted); free(location);
    }
}
// What the pass examined per risk category, folded: each category, whether it applies, and its assessment.
static void categories(PullScreen *s, Doc *doc, Col c, const ReviewSecurity *sec) {
    if (!sec->category_count) return;
    bool open = s->reviews.security_open;
    doc_space(doc, px(10));
    Item *toggle = doc_item(doc, doc_label(doc, c.ix, c.iw, open ? 0xE70D : 0xE76C, "What the pass examined", FONT_FOOTNOTE, theme.ink));
    toggle->action = ACT_REVIEW_SECURITY; toggle->hand = true;
    if (!open) return;
    Col in = {c.x, c.w, c.ix + px(22), c.iw - px(22)};
    for (size_t i = 0; i < sec->category_count; i++) {
        const SecurityCategory *category = &sec->categories[i];
        char *title = review_security_category_title(category);
        doc_space(doc, px(6));
        review_fact_draw(doc, in.ix, in.iw, title, review_security_category_line(category), review_security_category_tone(category));
        free(title);
        if (category->assessment) doc_text(doc, in.ix, in.iw, category->assessment, FONT_FOOTNOTE, theme.secondary, DT_WORDBREAK);
    }
}
// The block, while the project asks for a pass at all; nothing from an older server.
void screen_pulls_review_security(PullScreen *s, Doc *doc, Col c) {
    const ReviewOutcome *o = s->reviews.outcome;
    if (!o || !review_security_shown(&o->security)) return;
    const ReviewSecurity *sec = &o->security;
    int pad = px(12);
    int box = doc_box_begin(doc, c.ix, c.iw, pad, theme.canvas, theme.line, px(8));
    Col in = {c.x, c.w, c.ix + pad, c.iw - 2 * pad};
    review_fact_draw(doc, in.ix, in.iw, "Security", review_security_line(sec), review_security_tone(sec));
    doc_space(doc, px(4));
    char *risk = review_security_risk_note(sec), *scope = review_security_scope_line(sec);
    char *findings = review_security_findings_line(sec), *claims = review_security_claims_line(sec);
    review_line_draw(doc, in.ix, in.iw, risk, FONT_FOOTNOTE, theme.secondary);
    review_line_draw(doc, in.ix, in.iw, scope, FONT_CAPTION, theme.muted);
    review_line_draw(doc, in.ix, in.iw, findings, FONT_FOOTNOTE, theme.ink);
    review_line_draw(doc, in.ix, in.iw, claims, FONT_FOOTNOTE, theme.secondary);
    free(risk); free(scope); free(findings); free(claims);
    review_reasons_draw(doc, in.ix, in.iw, &sec->reasons, px(4));
    entries(doc, in, sec);
    categories(s, doc, in, sec);
    doc_box_end(doc, box, pad);
    doc_item(doc, box)->hover_fill = false;
    doc_space(doc, px(14));
}
