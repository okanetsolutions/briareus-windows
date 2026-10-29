#include "screen_pulls_internal.h"

static const int widths[] = {12, 8, 15, 15, 17, 10, 10, 13};
static const char *field(const Json *r, const char *key, const char *fallback) {
    const char *value = json_str(json_get(r, key)); return str_empty(value) ? fallback : value;
}
static void history_row(Doc *doc, Col c, char **cells, bool header, bool selected, int64_t id, const Json *r) {
    int top = doc->y, x = c.ix, pad = px(10), h = px(44);
    FontId font = header ? FONT_CAPTION_SEMIBOLD : FONT_FOOTNOTE;
    for (int i = 0; i < 8; i++) {
        int want = measure_text(doc->cv, cells[i], c.iw * widths[i] / 100 - 2 * pad, font, DT_WORDBREAK) + 2 * pad;
        if (want > h) h = want;
    }
    COLORREF fill = selected ? blend(theme.accent, theme.canvas, 0.10) : header ? theme.raise : theme.canvas;
    int box = doc_box_begin(doc, c.ix, c.iw, 0, fill, selected ? theme.accent : theme.line, 0);
    if (id) {
        doc_box_action(doc, box, ACT_REVIEW_ROW, (intptr_t)id);
        doc_item(doc, box)->paint = screen_pulls_paint_review_row;
    }
    for (int i = 0; i < 8; i++) {
        int w = i == 7 ? c.ix + c.iw - x : c.iw * widths[i] / 100;
        RECT rc = {x + pad, top + pad, x + w - pad, top + h - pad};
        int item = doc_text_at(doc, &rc, cells[i], font, header ? theme.secondary : theme.ink, DT_WORDBREAK);
        doc_item(doc, item)->cell = true; doc_item(doc, item)->tip = xstrdup(cells[i]);
        if (r && i == 0) { free(doc_item(doc, item)->tip); doc_item(doc, item)->tip = xstrdup(json_str(json_get(r, "createdAt"))); }
        if (r && i == 7) { free(doc_item(doc, item)->tip); doc_item(doc, item)->tip = code_review_usage(r); }
        x += w;
    }
    doc->y = top + h; doc_box_end(doc, box, 0);
}
void screen_pulls_review_history(PullScreen *s, Doc *doc, Col c) {
    Col table = c;
    if (table.iw < px(1200)) {
        doc_text(doc, c.ix, c.iw, "Scroll horizontally for usage and cost.", FONT_CAPTION, theme.secondary, DT_WORDBREAK);
        doc_space(doc, px(6)); table.iw = px(1200);
    }
    char *headers[] = {"Date", "Status", "Review / commit", "Provider / model", "Findings", "Input tokens", "Output tokens", "Cost"};
    history_row(doc, table, headers, true, false, 0, NULL);
    for (size_t i = 0; i < json_count(s->reviews.items); i++) {
        const Json *r = json_at(s->reviews.items, i);
        CodeReviewTotals t = code_review_totals(r);
        char *date = screen_pulls_review_date(r, "createdAt");
        char *context = xstrfmt("#%lld · %.10s\n%s", (long long)code_review_id(r), field(r, "headSha", "unknown"), code_review_head(r, json_str(json_get(s->pr, "headSha"))));
        char *provider = xstrfmt("%s\n%s", field(r, "provider", "Unknown provider"), field(r, "model", "Default model"));
        char *counts = code_review_counts_compact(r);
        char *input = screen_pulls_review_amount(t.input, false, false, 0), *output = screen_pulls_review_amount(t.output, false, false, 0);
        char *cost = screen_pulls_review_amount(t.cost, true, t.estimated, t.unpriced_turns);
        char *cells[] = {date, (char *)field(r, "status", "unknown"), context, provider, counts, input, output, cost};
        history_row(doc, table, cells, false, code_review_id(r) == s->reviews.row_review_id, code_review_id(r), r);
        free(date); free(context); free(provider); free(counts); free(input); free(output); free(cost);
    }
    CodeReviewTotals t = code_reviews_totals(s->reviews.items);
    char *scope = xstrfmt("Total\n%zu displayed reviews", t.reviews), *counts = xstrfmt("%zu findings", t.findings);
    char *input = screen_pulls_review_amount(t.input, false, false, 0), *output = screen_pulls_review_amount(t.output, false, false, 0);
    char *cost = screen_pulls_review_amount(t.cost, true, t.estimated, t.unpriced_turns);
    char *cells[] = {scope, "", "Latest 20 stored reviews", "Includes historical / dismissed", counts, input, output, cost};
    history_row(doc, table, cells, true, false, 0, NULL);
    // Exact aggregate values stay available alongside abbreviated table cells.
    for (size_t i = 0; i < 3; i++) {
        CodeReviewAmount a = i == 0 ? t.input : i == 1 ? t.output : t.cost;
        Item *item = doc_item(doc, (int)doc->count - 3 + (int)i);
        free(item->tip);
        item->tip = a.known ? xstrfmt("Known sum: %.6f · %zu unknown reviews", a.value, a.unknown) : xstrdup("Unknown");
    }
    free(scope); free(counts); free(input); free(output); free(cost);
}
