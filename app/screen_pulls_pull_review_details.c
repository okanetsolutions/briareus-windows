#include "screen_pulls_internal.h"

static void metadata(Doc *doc, Col c, const Json *r, const char *field, const char *label) {
    const char *v = json_str(json_get(r, field));
    char *text = xstrfmt("%s: %s", label, str_empty(v) ? "unknown" : v);
    doc_text(doc, c.ix, c.iw, text, FONT_FOOTNOTE, theme.secondary, DT_WORDBREAK); free(text);
}
static void markdown(Doc *doc, Col c, const Json *r, const char *field, const char *label) {
    const char *v = json_str(json_get(r, field));
    if (str_empty(v)) return;
    doc_section(doc, c.ix, c.iw, label);
    doc_markdown(doc, c.ix, c.iw, v, FONT_FOOTNOTE);
}
static void finding_details(Doc *doc, Col c, const Json *r, const Json *f) {
    doc_text(doc, c.ix, c.iw, json_str(json_get(f, "title")), FONT_SUBHEADLINE_SEMIBOLD, theme.ink, DT_WORDBREAK);
    char *location = code_review_location(f);
    doc_text(doc, c.ix, c.iw, location, FONT_FOOTNOTE, theme.secondary, DT_WORDBREAK); free(location);
    char *verdict = xstrfmt("Severity: %s · Verdict: %s · Verification: %s", json_str(json_get(f, "severity")), code_review_verdict(r, f), code_review_verification(r, f));
    doc_text(doc, c.ix, c.iw, verdict, FONT_FOOTNOTE, theme.ink, DT_WORDBREAK); free(verdict);
    metadata(doc, c, f, "type", "Type");
    markdown(doc, c, f, "body", "Finding");
    markdown(doc, c, f, "comments", "Verification rationale");
    metadata(doc, c, f, "createdAt", "Registered (UTC)");
    metadata(doc, c, f, "updatedAt", "Updated (UTC)");
}
void screen_pulls_reviews_details(PullScreen *s, Doc *doc, Col c) {
    const Json *r = code_review_find(s->reviews.items, s->reviews.review_id);
    if (!r) return;
    doc_rule(doc, c.ix, c.iw); doc_space(doc, px(10));
    char *title = xstrfmt("Review #%lld · %s", (long long)code_review_id(r), json_str(json_get(r, "status")));
    doc_text(doc, c.ix, c.iw, title, FONT_SUBHEADLINE_SEMIBOLD, theme.ink, DT_WORDBREAK); free(title);
    metadata(doc, c, r, "headSha", "Reviewed commit");
    doc_text(doc, c.ix, c.iw, code_review_head(r, json_str(json_get(s->pr, "headSha"))), FONT_FOOTNOTE, theme.secondary, DT_WORDBREAK);
    metadata(doc, c, r, "provider", "Provider"); metadata(doc, c, r, "model", "Model (empty uses provider default)");
    metadata(doc, c, r, "effort", "Effort (empty uses default)");
    char *usage = code_review_usage(r);
    doc_text(doc, c.ix, c.iw, usage, FONT_FOOTNOTE, theme.secondary, DT_WORDBREAK); free(usage);
    metadata(doc, c, r, "createdAt", "Started (UTC)"); metadata(doc, c, r, "completedAt", "Completed (UTC)");
    // No session lookup or navigation: review sessions can already have been deleted.
    if (s->tab == PR_TAB_FINDINGS) {
        const Json *f = code_review_finding(r, s->reviews.finding_id);
        if (f) finding_details(doc, c, r, f);
    } else {
        markdown(doc, c, r, "summary", "Review summary");
        const Json *findings = json_get(r, "findings");
        if (!json_count(findings)) doc_text(doc, c.ix, c.iw, "No findings registered in this review.", FONT_FOOTNOTE, theme.secondary, DT_WORDBREAK);
        // The full stored review remains readable as a report below its compact overview row.
        for (size_t i = 0; i < json_count(findings); i++) {
            doc_space(doc, px(14)); finding_details(doc, c, r, json_at(findings, i));
        }
    }
}
