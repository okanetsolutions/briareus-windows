#include "screen_pulls_internal.h"
#include "security_review.h"

// An opened review's own security pass (security_review.c): its result under the project's policy, folded under that
// line, then what was examined in each risk category, the findings the pass recorded, the earlier claims it checked and
// how the review's security findings stand. The pull request's security result stays in the outcome card on the table.

static void line(Doc *doc, Col c, const char *text, FontId font_id, COLORREF color) {
    doc_text(doc, c.ix, c.iw, text, font_id, color, DT_WORDBREAK);
}
static void heading(Doc *doc, Col c, const char *title) {
    doc_space(doc, px(10));
    doc_text(doc, c.ix, c.iw, title, FONT_CAPTION_SEMIBOLD, theme.secondary, DT_SINGLELINE);
    doc_space(doc, px(4));
}
// Each risk category: whether the change touches it, how many findings fall under it, and what was concluded.
static void categories(Doc *doc, Col c, const SecurityReview *sr) {
    if (!sr->category_count) return;
    heading(doc, c, "Coverage");
    for (size_t i = 0; i < sr->category_count; i++) {
        const SecurityCategory *k = &sr->categories[i];
        char *title = security_category_title(k->id);
        char *head = !k->applicable ? xstrfmt("%s \xC2\xB7 does not apply", title)
            : xstrfmt("%s \xC2\xB7 %d finding%s", title, k->findings, k->findings == 1 ? "" : "s");
        if (i) doc_space(doc, px(6));
        line(doc, c, head, FONT_FOOTNOTE_SEMIBOLD, k->applicable ? theme.ink : theme.secondary);
        if (k->assessment) line(doc, c, k->assessment, FONT_FOOTNOTE, theme.secondary);
        free(title); free(head);
    }
}
static void recorded(Doc *doc, Col c, const SecurityReview *sr) {
    if (!sr->entry_count) return;
    char *title = xstrfmt("Recorded findings (%d)", sr->recorded > (int)sr->entry_count ? sr->recorded : (int)sr->entry_count);
    heading(doc, c, title); free(title);
    for (size_t i = 0; i < sr->entry_count; i++) {
        char *text = security_entry_line(&sr->entries[i]);
        if (i) doc_space(doc, px(4));
        line(doc, c, text, FONT_FOOTNOTE, theme.ink); free(text);
        if (sr->entries[i].claim) {
            char *again = xstrfmt("Raises %s again", sr->entries[i].claim);
            line(doc, c, again, FONT_CAPTION, theme.secondary); free(again);
        }
    }
}
static void claims(Doc *doc, Col c, const SecurityReview *sr) {
    if (!sr->claim_count) return;
    heading(doc, c, "Earlier claims checked");
    for (size_t i = 0; i < sr->claim_count; i++) {
        char *text = security_claim_line(&sr->claims[i]);
        if (i) doc_space(doc, px(4));
        line(doc, c, text, FONT_FOOTNOTE, str_eq(sr->claims[i].status, "stands") ? theme.warn : theme.ink); free(text);
    }
}
static void detail(Doc *doc, Col c, const SecurityReview *sr) {
    line(doc, c, security_review_stage_line(sr), FONT_FOOTNOTE, theme.secondary);
    char *risk = security_review_risk_line(sr), *counts = security_review_counts_line(sr);
    if (risk) line(doc, c, risk, FONT_FOOTNOTE, theme.secondary);
    if (counts) line(doc, c, counts, FONT_FOOTNOTE, theme.ink);
    free(risk); free(counts);
    if (sr->stale) line(doc, c, "The pull request moved on from the revision this pass covered.", FONT_FOOTNOTE, theme.secondary);
    review_reasons_draw(doc, c.ix, c.iw, &sr->result.reasons, px(6));
    categories(doc, c, sr);
    recorded(doc, c, sr);
    claims(doc, c, sr);
    if (sr->widened) {
        char *text = xstrfmt("Widened to: %s", sr->widened);
        doc_space(doc, px(8)); line(doc, c, text, FONT_FOOTNOTE, theme.secondary); free(text);
    }
}
// "Security review  Failed · required" as a disclosure line; the detail under it once unfolded.
void screen_pulls_review_security(PullScreen *s, Doc *doc, Col c, const Json *review) {
    SecurityReview sr;
    if (!security_review_parse(review, &sr)) return;
    bool open = s->reviews.security_id == code_review_id(review);
    doc_space(doc, px(10));
    char *result = review_security_line(&sr.result), *title = xstrfmt("Security review \xC2\xB7 %s", result);
    Item *toggle = doc_item(doc, doc_label(doc, c.ix, c.iw, open ? 0xE70D : 0xE76C, title, FONT_SUBHEADLINE_SEMIBOLD,
        review_tone_color(review_security_tone(&sr.result))));
    toggle->action = ACT_REVIEW_SECURITY; toggle->hand = true;
    free(result); free(title);
    if (open) {
        doc_space(doc, px(8));
        Col inner = {c.x, c.w, c.ix + px(22), c.iw - px(22)};
        detail(doc, inner, &sr);
    }
    security_review_free(&sr);
}
