#include "screen_pulls_internal.h"
#include "stat_components.h"

// The way back to the table, then the review's number and status; a narrow pane puts the title under the button.
static void heading(Doc *doc, Col c, const Json *r) {
    int top = doc->y;
    ButtonSpec back = {0, "\xE2\x80\xB9 All reviews", BUTTON_BORDERED, ACT_REVIEW_BACK, 0, true};
    Item *button = doc_item(doc, doc_button_row(doc, c.ix, c.iw, &back, 1));
    button->tip = xstrdup("Back to the table of code reviews (Backspace)");
    int h = button->rc.bottom - top, x = button->rc.right + px(14);
    const char *status = json_str(json_get(r, "status"));
    BadgeSpec badge = {0, status, screen_pulls_review_color(status, false), false};
    char *title = xstrfmt("Review #%lld", (long long)code_review_id(r));
    int tw = text_width(doc->cv, title, FONT_TITLE3) + px(2), th = font_height(doc->cv, FONT_TITLE3);
    int bw = doc_badges_width(doc, &badge, 1);
    if (x + tw + px(10) + bw > c.ix + c.iw) { x = c.ix; top = doc->y + px(10); h = th; }
    RECT t = {x, top + (h - th) / 2, x + tw, top + (h - th) / 2 + th};
    doc_text_at(doc, &t, title, FONT_TITLE3, theme.ink, DT_SINGLELINE); free(title);
    doc->y = top;
    screen_pulls_review_badge(doc, x + tw + px(10), bw, status, false, theme.canvas);
    Item *it = doc_item(doc, (int)doc->count - 1);
    OffsetRect(&it->rc, 0, (h - (it->rc.bottom - it->rc.top)) / 2);
    doc->y = top + h;
}
// The reviewed commit, who reviewed it and when.
static void identity(PullScreen *s, Doc *doc, Col c, const Json *r) {
    const char *sha = json_str(json_get(r, "headSha")), *provider = json_str(json_get(r, "provider")), *model = json_str(json_get(r, "model"));
    char *when = screen_pulls_review_date(r, "createdAt"), *commit = str_empty(sha) ? xstrdup("Unknown commit") : xstrfmt("%.10s", sha);
    char *line = xstrfmt("%s · %s · %s · %s · %s", commit, code_review_head(r, json_str(json_get(s->pr, "headSha"))),
        str_empty(provider) ? "Unknown provider" : provider, str_empty(model) ? "Default model" : model, when);
    int i = doc_text(doc, c.ix, c.iw, line, FONT_FOOTNOTE, theme.secondary, DT_WORDBREAK);
    doc_item(doc, i)->tip = xstrdup(str_empty(sha) ? "The reviewed commit is unknown" : sha);
    free(when); free(commit); free(line);
}
// As the table's cost cell words it: a known cost with unpriced turns is partial.
static char *cost_detail(CodeReviewTotals t) {
    Str text; str_init(&text);
    if (t.cost.known && t.estimated) str_appendz(&text, "estimated");
    if (t.cost.known && t.unpriced_turns) str_appendf(&text, "%spartial", text.len ? " · " : "");
    if (t.unpriced_turns) str_appendf(&text, "%s%zu unpriced turn%s", text.len ? " · " : "", t.unpriced_turns, t.unpriced_turns == 1 ? "" : "s");
    return str_detach(&text);
}
// What it found and what it cost, as the Resources tiles show figures; unknown usage stays Unknown.
static void figures(Doc *doc, Col c, const Json *r) {
    CodeReviewTotals t = code_review_totals(r);
    char *counts = code_review_counts_compact(r), *breakdown = strchr(counts, '\n');
    const char *none = str_eq(json_str(json_get(r, "status")), "running") ? "None registered yet" : "None registered";
    StatTile tiles[] = {
        {"Findings", xstrfmt("%zu", t.findings), xstrdup(breakdown ? breakdown + 1 : none), {0}},
        {"Input tokens", screen_pulls_review_amount(t.input, false, false, 0), xstrdup(""), {0}},
        {"Output tokens", screen_pulls_review_amount(t.output, false, false, 0), xstrdup(""), {0}},
        {"Cost", screen_pulls_review_amount(t.cost, true, false, 0), cost_detail(t), {0}},
    };
    free(counts);
    doc_stat_tiles(doc, c.ix, c.iw, tiles, 4); stats_tiles_free(tiles, 4);
}
// The stored summary and the rest of the review's record: folded under its findings, open when it found none.
static void about(PullScreen *s, Doc *doc, Col c, const Json *r) {
    const char *title = str_empty(json_str(json_get(r, "summary"))) ? "Review details" : "Review summary";
    if (!json_count(json_get(r, "findings"))) { doc_section(doc, c.ix, c.iw, title); screen_pulls_review_about(doc, c, r); return; }
    bool open = s->reviews.summary_id == code_review_id(r);
    Item *toggle = doc_item(doc, doc_label(doc, c.ix, c.iw, open ? 0xE70D : 0xE76C, title, FONT_SUBHEADLINE_SEMIBOLD, theme.ink));
    toggle->action = ACT_REVIEW_SUMMARY; toggle->hand = true;
    if (!open) return;
    doc_space(doc, px(8));
    Col inner = {c.x, c.w, c.ix + px(22), c.iw - px(22)};
    screen_pulls_review_about(doc, inner, r);
}
// A review opened from the Code reviews table: its findings as a table of their own, under what identifies the review.
void screen_pulls_review_page(PullScreen *s, Doc *doc, Col c, const Json *review) {
    heading(doc, c, review);
    doc_space(doc, px(6));
    identity(s, doc, c, review);
    doc_space(doc, px(14));
    figures(doc, c, review);
    doc_space(doc, px(6));
    about(s, doc, c, review);
    if (!json_count(json_get(review, "findings"))) return;
    doc_space(doc, px(16));
    screen_pulls_review_findings(s, doc, c, review);
}
