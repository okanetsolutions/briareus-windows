#include "screen_pulls_internal.h"

static const char *field(const Json *r, const char *name) {
    const char *v = json_str(json_get(r, name)); return str_empty(v) ? "unknown" : v;
}
static void cell(Doc *doc, int x, int y, int w, int h, const char *text, FontId font, COLORREF color, UINT flags) {
    RECT rc = {x, y, x + w, y + h};
    int i = doc_text_at(doc, &rc, text, font, color, flags);
    Item *it = doc_item(doc, i); it->cell = true; it->tip = xstrdup(text);
}
COLORREF screen_pulls_review_color(const char *text, bool severity) {
    COLORREF color = theme.secondary;
    if (str_eq(text, "critical") || str_eq(text, "high")) color = theme.danger;
    else if (str_eq(text, "medium") || str_eq(text, "pending") || str_eq(text, "running")) color = theme.warn;
    else if (str_eq(text, "fix") || str_eq(text, "failed")) color = theme.danger;
    else if (str_eq(text, "confirmed") || str_eq(text, "completed")) color = theme.ok;
    else if (severity || str_eq(text, "optional")) color = theme.accent;
    return color;
}
void screen_pulls_review_badge(Doc *doc, int x, int w, const char *text, bool severity, COLORREF background) {
    BadgeSpec badge = {0, text, screen_pulls_review_color(text, severity), false};
    int i = doc_badges(doc, x, w, &badge, 1, background);
    // Keep badge text available to document copying, find and assistive names.
    doc_item(doc, i)->text = xstrfmt("%s: %s", severity ? "Severity" : "Verdict", text); doc_item(doc, i)->tip = xstrdup(doc_item(doc, i)->text);
    doc_item(doc, i)->cell = true;
}
char *screen_pulls_review_date(const Json *r, const char *name) {
    time_t when;
    return board_date_parse(json_str(json_get(r, name)), &when) ? format_event_time(when) : xstrdup("Unknown date");
}
static void heading(Doc *doc, Col c) {
    int top = doc->y, pad = px(10), status = px(78), last = px(112);
    int first = c.iw - status - last;
    int box = doc_box_begin(doc, c.ix, c.iw, 0, theme.raise, theme.line, 0);
    cell(doc, c.ix + pad, top + pad, first - pad, px(18), "Finding / source", FONT_CAPTION_SEMIBOLD, theme.secondary, DT_SINGLELINE);
    cell(doc, c.ix + first, top + pad, status, px(18), "Severity", FONT_CAPTION_SEMIBOLD, theme.secondary, DT_SINGLELINE);
    cell(doc, c.ix + first + status, top + pad, last - pad, px(18), "Verdict", FONT_CAPTION_SEMIBOLD, theme.secondary, DT_SINGLELINE);
    doc->y = top + px(38); doc_box_end(doc, box, 0);
}
void screen_pulls_paint_review_row(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    COLORREF fill = it->fill;
    if (it->border != theme.accent && doc_item_hovered(doc, it)) fill = theme.raise;
    fill_round_rect(cv, rc, 0, fill, it->border);
}
static int row_begin(Doc *doc, Col c, bool selected, int action, int64_t id) {
    COLORREF bg = selected ? blend(theme.accent, theme.canvas, 0.10) : theme.canvas;
    int box = doc_box_begin(doc, c.ix, c.iw, 0, bg, selected ? theme.accent : theme.line, 0);
    doc_box_action(doc, box, action, (intptr_t)id);
    // Hover changes the fill, never the selection outline.
    doc_item(doc, box)->paint = screen_pulls_paint_review_row;
    return box;
}
static void finding_row(PullScreen *s, Doc *doc, Col c, const Json *r, const Json *f) {
    int top = doc->y, pad = px(10), first = c.iw - px(190);
    int title_h = measure_text(doc->cv, field(f, "title"), first - 2 * pad, FONT_FOOTNOTE_SEMIBOLD, DT_WORDBREAK);
    int max_title = 3 * font_height(doc->cv, FONT_FOOTNOTE_SEMIBOLD);
    if (title_h > max_title) title_h = max_title;
    int h = title_h + px(58); if (h < px(88)) h = px(88);
    bool selected = code_review_id(r) == s->reviews.review_id && code_review_id(f) == s->reviews.finding_id;
    int box = row_begin(doc, c, selected, ACT_REVIEW_FINDING, code_review_id(f));
    cell(doc, c.ix + pad, top + pad, first - 2 * pad, title_h, field(f, "title"), FONT_FOOTNOTE_SEMIBOLD, theme.ink, DT_WORDBREAK | DT_END_ELLIPSIS);
    char *where = code_review_location(f);
    const char *path = json_str(json_get(f, "filePath"));
    const char *base = path ? strrchr(path, '/') : NULL; base = base ? base + 1 : path ? path : "No location";
    cell(doc, c.ix + pad, top + h - px(40), first - 2 * pad, px(20), base, FONT_CAPTION, theme.secondary, DT_SINGLELINE | DT_PATH_ELLIPSIS);
    free(doc_item(doc, (int)doc->count - 1)->tip); doc_item(doc, (int)doc->count - 1)->tip = where;
    double start, end;
    if (json_num(json_get(f, "startLine"), &start)) {
        char *lines = json_num(json_get(f, "endLine"), &end) && end != start ? xstrfmt("%.0f–%.0f", start, end) : xstrfmt("%.0f", start);
        cell(doc, c.ix + pad, top + h - px(22), first - 2 * pad, px(18), lines, FONT_CAPTION, theme.secondary, DT_SINGLELINE); free(lines);
    }
    doc->y = top + pad;
    screen_pulls_review_badge(doc, c.ix + first, px(72), field(f, "severity"), true, theme.canvas);
    doc->y = top + pad;
    screen_pulls_review_badge(doc, c.ix + first + px(78), px(102), code_review_verdict(r, f), false, theme.canvas);
    if (!str_eq(code_review_verdict(r, f), code_review_verification(r, f))) cell(doc, c.ix + first + px(78), top + px(38), px(102), px(30), code_review_verification(r, f), FONT_CAPTION, theme.secondary, DT_WORDBREAK);
    doc->y = top + h; doc_box_end(doc, box, 0);
}
void screen_pulls_reviews_table(PullScreen *s, Doc *doc, Col c, bool header) {
    if (header) { heading(doc, c); return; }
    for (size_t i = 0; i < json_count(s->reviews.items); i++) {
        const Json *r = json_at(s->reviews.items, i), *rows = json_get(r, "findings");
        char *context = xstrfmt("Review #%lld · %.10s · %s", (long long)code_review_id(r), field(r, "headSha"), code_review_head(r, json_str(json_get(s->pr, "headSha"))));
        if (json_count(rows)) {
            doc_space(doc, px(10));
            doc_text(doc, c.ix + px(10), c.iw - px(20), context, FONT_CAPTION_SEMIBOLD, theme.secondary, DT_WORDBREAK);
            doc_space(doc, px(6));
        }
        for (size_t j = 0; j < json_count(rows); j++) finding_row(s, doc, c, r, json_at(rows, j));
        free(context);
    }
}
