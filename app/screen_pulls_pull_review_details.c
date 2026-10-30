#include "screen_pulls_internal.h"

static void metadata(Doc *doc, Col c, const Json *r, const char *field, const char *label) {
    const char *v = json_str(json_get(r, field));
    if (str_empty(v)) return;
    char *text = xstrfmt("%s: %s", label, v);
    doc_text(doc, c.ix, c.iw, text, FONT_FOOTNOTE, theme.secondary, DT_WORDBREAK); free(text);
}
static void date(Doc *doc, Col c, const Json *r, const char *field, const char *label) {
    if (str_empty(json_str(json_get(r, field)))) return;
    char *when = screen_pulls_review_date(r, field), *text = xstrfmt("%s: %s", label, when);
    int i = doc_text(doc, c.ix, c.iw, text, FONT_CAPTION, theme.secondary, DT_WORDBREAK);
    doc_item(doc, i)->tip = xstrdup(json_str(json_get(r, field))); free(when); free(text);
}
static void markdown(Doc *doc, Col c, const Json *r, const char *field, const char *label) {
    const char *v = json_str(json_get(r, field));
    if (str_empty(v)) return;
    doc_section(doc, c.ix, c.iw, label);
    doc_markdown(doc, c.ix, c.iw, v, FONT_FOOTNOTE);
}
static void finding_details(Doc *doc, Col c, const Json *r, const Json *f) {
    const char *type = json_str(json_get(f, "type"));
    char *label = str_empty(type) ? xstrfmt("Finding #%lld", (long long)code_review_id(f)) : xstrfmt("Finding #%lld · %s", (long long)code_review_id(f), type);
    doc_text(doc, c.ix, c.iw, label, FONT_CAPTION_SEMIBOLD, theme.accent, DT_WORDBREAK); free(label);
    doc_space(doc, px(6));
    doc_text(doc, c.ix, c.iw, json_str(json_get(f, "title")), FONT_TITLE3, theme.ink, DT_WORDBREAK);
    doc_space(doc, px(10));
    BadgeSpec badges[] = {{0, json_str(json_get(f, "severity")), screen_pulls_review_color(json_str(json_get(f, "severity")), true)},
        {0, code_review_verdict(r, f), screen_pulls_review_color(code_review_verdict(r, f), false)},
        {0, code_review_verification(r, f), screen_pulls_review_color(code_review_verification(r, f), false)}};
    int badge = doc_badges(doc, c.ix, c.iw, badges, str_eq(badges[1].text, badges[2].text) ? 2 : 3, theme.canvas);
    doc_item(doc, badge)->text = xstrfmt("Severity: %s · Verdict: %s · Verification: %s", badges[0].text, badges[1].text, badges[2].text);
    doc_space(doc, px(10));
    const char *path = json_str(json_get(f, "filePath"));
    if (!str_empty(path)) {
        // The full source can be copied or inspected in the tooltip; line ranges never wrap mid-number.
        int index = doc_text(doc, c.ix, c.iw, path, FONT_MONO_SMALL, theme.secondary, DT_WORDBREAK);
        doc_item(doc, index)->tip = code_review_location(f);
        double start, end;
        if (json_num(json_get(f, "startLine"), &start)) {
            char *lines = json_num(json_get(f, "endLine"), &end) && end != start ? xstrfmt("Lines %.0f–%.0f", start, end) : xstrfmt("Line %.0f", start);
            doc_text(doc, c.ix, c.iw, lines, FONT_CAPTION, theme.secondary, DT_SINGLELINE); free(lines);
        }
    }
    markdown(doc, c, f, "body", "Finding");
    markdown(doc, c, f, "comments", "Verification rationale");
    doc_space(doc, px(12));
    date(doc, c, f, "createdAt", "Registered"); date(doc, c, f, "updatedAt", "Updated");
}
// The selected finding of an open review, as the inspector beside or below its table shows it.
void screen_pulls_reviews_details(PullScreen *s, Doc *doc, Col c, const Json *review) {
    const Json *f = code_review_finding(review, s->reviews.finding_id);
    if (!f) { doc_text(doc, c.ix, c.iw, "Select a finding to read its details.", FONT_BODY, theme.secondary, DT_WORDBREAK); return; }
    finding_details(doc, c, review, f);
}
// The review's stored summary and the parts of its record the page does not otherwise show; exact usage stays unrounded.
void screen_pulls_review_about(Doc *doc, Col c, const Json *review) {
    const char *summary = json_str(json_get(review, "summary"));
    if (!str_empty(summary)) { doc_markdown(doc, c.ix, c.iw, summary, FONT_FOOTNOTE); doc_space(doc, px(10)); }
    metadata(doc, c, review, "headSha", "Reviewed commit");
    metadata(doc, c, review, "effort", "Effort");
    char *usage = code_review_usage(review), *text = xstrfmt("Usage: %s", usage);
    doc_text(doc, c.ix, c.iw, text, FONT_FOOTNOTE, theme.secondary, DT_WORDBREAK); free(usage); free(text);
    date(doc, c, review, "createdAt", "Started"); date(doc, c, review, "completedAt", "Completed");
}
