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
static void finding_details(PullScreen *s, Doc *doc, Col c, const Json *r, const Json *f) {
    char *label = xstrfmt("Finding #%lld · Review #%lld · %s", (long long)code_review_id(f), (long long)code_review_id(r), code_review_head(r, json_str(json_get(s->pr, "headSha"))));
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
        int index = doc_text(doc, c.ix, c.iw, path, FONT_MONO_SMALL, theme.secondary, DT_SINGLELINE | DT_PATH_ELLIPSIS);
        doc_item(doc, index)->tip = code_review_location(f);
        double start, end;
        if (json_num(json_get(f, "startLine"), &start)) {
            char *lines = json_num(json_get(f, "endLine"), &end) && end != start ? xstrfmt("Lines %.0f–%.0f", start, end) : xstrfmt("Line %.0f", start);
            doc_text(doc, c.ix, c.iw, lines, FONT_CAPTION, theme.secondary, DT_SINGLELINE); free(lines);
        }
    }
    markdown(doc, c, f, "body", "Finding");
    markdown(doc, c, f, "comments", "Verification rationale");
}
static void review_summary(PullScreen *s, Doc *doc, Col c, const Json *r) {
    char *context = xstrfmt("Review #%lld · %.10s · %s", (long long)code_review_id(r),
        json_str(json_get(r, "headSha")) ? json_str(json_get(r, "headSha")) : "unknown", code_review_head(r, json_str(json_get(s->pr, "headSha"))));
    doc_text(doc, c.ix, c.iw, context, FONT_FOOTNOTE_SEMIBOLD, theme.secondary, DT_WORDBREAK); free(context);
    const char *provider = json_str(json_get(r, "provider")), *model = json_str(json_get(r, "model"));
    char *identity = xstrfmt("%s · %s", str_empty(provider) ? "Unknown provider" : provider, str_empty(model) ? "Default model" : model);
    doc_text(doc, c.ix, c.iw, identity, FONT_CAPTION, theme.secondary, DT_WORDBREAK); free(identity);
    date(doc, c, r, "createdAt", "Started");
    CodeReviewTotals t = code_review_totals(r);
    char *price = screen_pulls_review_amount(t.cost, true, t.estimated, t.unpriced_turns);
    char *in = screen_pulls_review_amount(t.input, false, false, 0), *out = screen_pulls_review_amount(t.output, false, false, 0);
    char *usage = xstrfmt("%s · %s in / %s out", price, in, out);
    int i = doc_text(doc, c.ix, c.iw, usage, FONT_CAPTION, theme.secondary, DT_WORDBREAK);
    doc_item(doc, i)->tip = code_review_usage(r);
    free(price); free(in); free(out); free(usage);
}
void screen_pulls_reviews_details(PullScreen *s, Doc *doc, Col c) {
    bool findings = s->tab == PR_TAB_FINDINGS;
    const Json *r = code_review_find(s->reviews.items, findings ? s->reviews.review_id : s->reviews.row_review_id);
    if (!r) { doc_text(doc, c.ix, c.iw, "Select a row to read its details.", FONT_BODY, theme.secondary, DT_WORDBREAK); return; }
    if (findings) {
        const Json *f = code_review_finding(r, s->reviews.finding_id);
        if (f) finding_details(s, doc, c, r, f);
    } else {
        char *title = xstrfmt("Review #%lld", (long long)code_review_id(r));
        doc_text(doc, c.ix, c.iw, title, FONT_TITLE3, theme.ink, DT_WORDBREAK); free(title);
        screen_pulls_review_badge(doc, c.ix, c.iw, json_str(json_get(r, "status")), false, theme.canvas);
        markdown(doc, c, r, "summary", "Review summary");
        const Json *rows = json_get(r, "findings");
        if (!json_count(rows)) doc_text(doc, c.ix, c.iw, "No findings registered in this review.", FONT_FOOTNOTE, theme.secondary, DT_WORDBREAK);
        for (size_t i = 0; i < json_count(rows); i++) { doc_space(doc, px(18)); finding_details(s, doc, c, r, json_at(rows, i)); }
    }
    doc_space(doc, px(18)); doc_rule(doc, c.ix, c.iw); doc_space(doc, px(12));
    review_summary(s, doc, c, r);
    doc_space(doc, px(10));
    doc_button(doc, c.ix, 0, s->reviews.metadata_open ? "Hide review metadata" : "Review metadata", BUTTON_PLAIN, ACT_REVIEW_METADATA, 0, true);
    if (!s->reviews.metadata_open) return;
    metadata(doc, c, r, "headSha", "Reviewed commit");
    metadata(doc, c, r, "effort", "Effort");
    char *usage = code_review_usage(r);
    doc_text(doc, c.ix, c.iw, usage, FONT_FOOTNOTE, theme.secondary, DT_WORDBREAK); free(usage);
    date(doc, c, r, "completedAt", "Completed");
    const Json *f = code_review_finding(r, s->reviews.finding_id);
    if (findings && f) {
        metadata(doc, c, f, "type", "Type"); date(doc, c, f, "createdAt", "Registered"); date(doc, c, f, "updatedAt", "Updated");
    }
}
