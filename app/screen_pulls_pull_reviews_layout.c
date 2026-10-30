#include "screen_pulls_internal.h"

static void divider(Doc *doc, Item *it, Canvas *cv, const RECT *rc) { fill_rect(cv, rc, theme.line); }

static void reveal_selection(PullScreen *s, Doc *doc, int first, int last) {
    if (!s->reviews.reveal_row) return;
    s->reviews.reveal_row = false;
    DocRegion *region = &doc->regions[doc->region_count - 1];
    for (int i = first; i < last; i++) {
        const Item *it = &doc->items[i];
        if (it->action != ACT_REVIEW_FINDING || it->arg != (intptr_t)s->reviews.finding_id) continue;
        int delta = 0;
        if (it->rc.top < region->view.top) delta = it->rc.top - region->view.top;
        else if (it->rc.bottom > region->view.bottom) delta = it->rc.bottom - region->view.bottom;
        doc_region_wheel(doc, region->view.left, region->view.top, delta); break;
    }
}
// The Code reviews tab: the table of reviews, or the findings of the one opened from it.
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
    const Json *review = r->open ? code_review_find(r->items, r->review_id) : NULL;
    if (review) { screen_pulls_review_page(s, doc, c, review); return; }
    if (!json_count(r->items)) { doc_text(doc, c.ix, c.iw, "No stored code reviews.", FONT_BODY, theme.secondary, DT_WORDBREAK); return; }
    screen_pulls_review_history(s, doc, c);
}
// A review's findings table. On a wide pane the selected finding sits beside it: both share the viewport, scroll on
// their own and end with the longer of the two. Otherwise the detail follows a bounded table.
void screen_pulls_review_findings(PullScreen *s, Doc *doc, Col c, const Json *review) {
    PullReviews *r = &s->reviews;
    bool wide = c.iw >= px(1100);
    int top = doc->y, gap = px(28);
    Col table = c, detail = c;
    if (wide) {
        table.iw = (c.iw - gap) / 2;
        detail.ix = table.ix + table.iw + gap; detail.iw = c.iw - table.iw - gap;
    }
    screen_pulls_reviews_table(s, doc, table, review, true);
    int first = (int)doc->count, body_top = doc->y;
    screen_pulls_reviews_table(s, doc, table, review, false);
    int last = (int)doc->count, bottom = doc->y;
    if (!wide) {
        // Only as tall as its rows, up to a bound that leaves the detail in reach.
        RECT view = {table.ix, body_top, table.ix + table.iw, body_top + (bottom - body_top < px(240) ? bottom - body_top : px(240))};
        doc_region(doc, first, last, &view, bottom);
        reveal_selection(s, doc, first, last);
        doc->y = view.bottom + px(20);
        screen_pulls_reviews_details(s, doc, detail, review);
        r->reset_inspector = false;
        return;
    }
    doc->y = top;
    int detail_first = (int)doc->count;
    screen_pulls_reviews_details(s, doc, detail, review);
    int detail_last = (int)doc->count, detail_bottom = doc->y;
    RECT viewport = s->base.pane ? pane_content_rect(s->base.pane) : (RECT){0, 0, doc->width, px(900)};
    int height = viewport.bottom - viewport.top - top - px(18);
    if (height < px(240)) height = px(240);
    int end = bottom > detail_bottom ? bottom : detail_bottom;
    if (end > top + height) end = top + height;
    RECT view = {table.ix, body_top, table.ix + table.iw, end};
    doc_region(doc, first, last, &view, bottom);
    reveal_selection(s, doc, first, last);
    // The inspector keeps its position on a background refresh, and starts at the top for another finding.
    size_t index = doc->region_count;
    if (r->reset_inspector && index < doc->regions_kept) doc->regions[index].scroll = 0;
    RECT inspector = {detail.ix, top, detail.ix + detail.iw, end};
    doc_region(doc, detail_first, detail_last, &inspector, detail_bottom);
    // A fine divider separates the two reading surfaces without adding another card.
    RECT rule = {detail.ix - gap / 2, top, detail.ix - gap / 2 + 1, end};
    int i = doc_add(doc, &rule, divider); doc_item(doc, i)->fill = theme.line;
    doc->y = end;
    r->reset_inspector = false;
}
