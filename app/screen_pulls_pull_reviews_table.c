#include "screen_pulls_internal.h"

// Uses document cells and whole-row hit targets, like its native Markdown tables.
static void table_row(Doc *doc, Col c, char **cells, const int *widths, int columns, bool heading, bool selected, int action, int64_t id) {
    int top = doc->y, h = px(38), x = c.ix, pad = px(8);
    FontId font = heading ? FONT_CAPTION_SEMIBOLD : FONT_FOOTNOTE;
    for (int i = 0; i < columns; i++) {
        int want = measure_text(doc->cv, cells[i], c.iw * widths[i] / 100 - 2 * pad, font, DT_WORDBREAK) + 2 * pad;
        if (want > h) h = want;
    }
    if (h > px(110)) h = px(110);
    RECT row = {c.ix, top, c.ix + c.iw, top + h};
    int box = doc_box_begin(doc, c.ix, c.iw, 0, selected ? theme.sunken : heading ? theme.raise : theme.canvas, theme.line, 0);
    if (action) doc_box_action(doc, box, action, (intptr_t)id);
    Item *it;
    for (int i = 0; i < columns; i++) {
        int w = i + 1 == columns ? row.right - x : c.iw * widths[i] / 100;
        RECT cell = {x + pad, top + pad, x + w - pad, top + h - pad};
        UINT flags = DT_WORDBREAK;
        if (measure_text(doc->cv, cells[i], w - 2 * pad, font, DT_WORDBREAK) > h - 2 * pad) flags |= DT_END_ELLIPSIS;
        int index = doc_text_at(doc, &cell, cells[i], font, heading ? theme.secondary : theme.ink, flags);
        it = doc_item(doc, index); it->cell = true;
        it->tip = xstrdup(cells[i]);
        x += w;
    }
    doc->y = top + h; doc_box_end(doc, box, 0);
}
static const char *field(const Json *r, const char *name) {
    const char *v = json_str(json_get(r, name)); return str_empty(v) ? "unknown" : v;
}
static char *context(PullScreen *s, const Json *r) {
    return xstrfmt("#%lld · %.10s\n%s", (long long)code_review_id(r), field(r, "headSha"), code_review_head(r, json_str(json_get(s->pr, "headSha"))));
}
static void findings_table(PullScreen *s, Doc *doc, Col c, bool wide) {
    int wide_widths[] = {34, 10, 22, 17, 17}, small_widths[] = {44, 28, 28};
    char *wide_heads[] = {"Finding", "Severity", "Location", "Verdict / verification", "Review / commit"};
    char *small_heads[] = {"Finding / severity / review", "Location", "Verdict / verification"};
    table_row(doc, c, wide ? wide_heads : small_heads, wide ? wide_widths : small_widths, wide ? 5 : 3, true, false, 0, 0);
    for (size_t i = 0; i < json_count(s->reviews.items); i++) {
        const Json *r = json_at(s->reviews.items, i), *findings = json_get(r, "findings");
        for (size_t j = 0; j < json_count(findings); j++) {
            const Json *f = json_at(findings, j);
            char *where = code_review_location(f), *ctx = context(s, r);
            char *verdict = xstrfmt("%s\n%s", code_review_verdict(r, f), code_review_verification(r, f));
            char *title = wide ? xstrdup(field(f, "title")) : xstrfmt("%s\n%s · %s", field(f, "title"), field(f, "severity"), ctx);
            char *wide_cells[] = {title, (char *)field(f, "severity"), where, verdict, ctx};
            char *small_cells[] = {title, where, verdict};
            bool selected = code_review_id(r) == s->reviews.review_id && code_review_id(f) == s->reviews.finding_id;
            table_row(doc, c, wide ? wide_cells : small_cells, wide ? wide_widths : small_widths, wide ? 5 : 3, false, selected, ACT_REVIEW_FINDING, code_review_id(f));
            free(where); free(ctx); free(verdict); free(title);
        }
    }
}
static void reviews_table(PullScreen *s, Doc *doc, Col c, bool wide) {
    int wide_widths[] = {17, 10, 18, 23, 32}, small_widths[] = {32, 33, 35};
    char *wide_heads[] = {"Date (UTC)", "Status", "Review / commit", "Provider / model", "Findings"};
    char *small_heads[] = {"Date (UTC) / status", "Review / commit / provider", "Findings"};
    table_row(doc, c, wide ? wide_heads : small_heads, wide ? wide_widths : small_widths, wide ? 5 : 3, true, false, 0, 0);
    for (size_t i = 0; i < json_count(s->reviews.items); i++) {
        const Json *r = json_at(s->reviews.items, i);
        char *ctx = context(s, r), *counts = code_review_counts(r);
        char *provider = xstrfmt("%s\n%s", field(r, "provider"), str_empty(json_str(json_get(r, "model"))) ? "Provider default model" : field(r, "model"));
        char *date = xstrfmt("%s%s%s", field(r, "createdAt"), wide ? "" : "\n", wide ? "" : field(r, "status"));
        char *compact = xstrfmt("%s\n%s", ctx, provider);
        char *wide_cells[] = {date, (char *)field(r, "status"), ctx, provider, counts};
        char *small_cells[] = {date, compact, counts};
        table_row(doc, c, wide ? wide_cells : small_cells, wide ? wide_widths : small_widths, wide ? 5 : 3, false, code_review_id(r) == s->reviews.review_id, ACT_REVIEW_ROW, code_review_id(r));
        free(ctx); free(counts); free(provider); free(date); free(compact);
    }
}
void screen_pulls_layout_reviews(PullScreen *s, Doc *doc, Col c) {
    if (!screen_pulls_reviews_sync(s)) {
        doc_notice(doc, c.ix, c.iw, "Stored code reviews are not supported by this server or token. Update the server or check project read access."); return;
    }
    PullReviews *r = &s->reviews;
    if (r->error) {
        doc_notice(doc, c.ix, c.iw, r->error);
        doc_text(doc, c.ix, c.iw, r->items ? "Previous snapshot below; totals may be stale. F5 retries." : "Review results are unavailable. F5 retries.", FONT_FOOTNOTE, theme.secondary, DT_WORDBREAK);
    }
    if (!r->items) { if (!r->error) doc_loading(doc, c.ix, c.iw, "Loading stored code reviews…"); return; }
    char *scope = xstrfmt("%zu findings across %zu stored reviews · latest 20 reviews, newest first", code_reviews_findings(r->items), json_count(r->items));
    doc_text(doc, c.ix, c.iw, scope, FONT_SUBHEADLINE_SEMIBOLD, theme.ink, DT_WORDBREAK); free(scope);
    char *totals = code_reviews_counts(r->items);
    doc_text(doc, c.ix, c.iw, totals, FONT_FOOTNOTE, theme.ink, DT_WORDBREAK); free(totals);
    doc_text(doc, c.ix, c.iw, "Totals include historical duplicates and dismissed findings. Verdicts are verification results, not fixed status. Select a row for details; Up/Down or Home/End moves selection.", FONT_CAPTION, theme.secondary, DT_WORDBREAK);
    if (r->req) doc_text(doc, c.ix, c.iw, "Refreshing stored reviews…", FONT_CAPTION, theme.secondary, DT_WORDBREAK);
    doc_space(doc, px(12));
    if (!json_count(r->items)) { doc_text(doc, c.ix, c.iw, "No stored code reviews.", FONT_BODY, theme.secondary, DT_WORDBREAK); return; }
    bool findings = s->tab == PR_TAB_FINDINGS;
    if (findings && !code_reviews_findings(r->items)) doc_text(doc, c.ix, c.iw, "No findings registered in these reviews; check review status in Code reviews.", FONT_BODY, theme.secondary, DT_WORDBREAK);
    else {
        bool wide = c.iw >= px(900);
        Col table = c;
        if (table.iw < px(540)) {
            doc_text(doc, c.ix, c.iw, "Scroll horizontally to see all columns.", FONT_CAPTION, theme.secondary, DT_WORDBREAK);
            table.iw = px(540);
        }
        if (findings) findings_table(s, doc, table, wide); else reviews_table(s, doc, table, wide);
    }
    doc_space(doc, px(18)); screen_pulls_reviews_details(s, doc, c);
}
