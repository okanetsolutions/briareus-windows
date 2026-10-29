#include "screen_pulls_internal.h"

static void divider(Doc *doc, Item *it, Canvas *cv, const RECT *rc) { fill_rect(cv, rc, theme.line); }

static void reveal_selection(PullScreen *s, Doc *doc, int first, int last) {
    if (!s->reviews.reveal_row) return;
    s->reviews.reveal_row = false;
    bool findings = s->tab == PR_TAB_FINDINGS;
    int action = findings ? ACT_REVIEW_FINDING : ACT_REVIEW_ROW;
    int64_t id = findings ? s->reviews.finding_id : s->reviews.row_review_id;
    DocRegion *region = &doc->regions[doc->region_count - 1];
    for (int i = first; i < last; i++) {
        const Item *it = &doc->items[i];
        if (it->action != action || it->arg != (intptr_t)id) continue;
        int delta = 0;
        if (it->rc.top < region->view.top) delta = it->rc.top - region->view.top;
        else if (it->rc.bottom > region->view.bottom) delta = it->rc.bottom - region->view.bottom;
        doc_region_wheel(doc, region->view.left, region->view.top, delta); break;
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
    if (s->tab == PR_TAB_CODE_REVIEWS) {
        if (!json_count(r->items)) { doc_text(doc, c.ix, c.iw, "No stored code reviews.", FONT_BODY, theme.secondary, DT_WORDBREAK); return; }
        screen_pulls_review_history(s, doc, c);
        doc_space(doc, px(18)); screen_pulls_reviews_details(s, doc, c);
        return;
    }
    char *scope = xstrfmt("%zu findings · %zu reviews", code_reviews_findings(r->items), json_count(r->items));
    int overview = doc_text(doc, c.ix, c.iw, scope, FONT_SUBHEADLINE_SEMIBOLD, theme.ink, DT_WORDBREAK); free(scope);
    doc_item(doc, overview)->tip = code_reviews_counts(r->items);
    doc_text(doc, c.ix, c.iw, "Latest 20 reviews · includes historical and dismissed findings", FONT_CAPTION, theme.secondary, DT_WORDBREAK);
    if (r->req) doc_text(doc, c.ix, c.iw, "Refreshing…", FONT_CAPTION, theme.secondary, DT_WORDBREAK);
    doc_space(doc, px(14));
    if (!json_count(r->items)) { doc_text(doc, c.ix, c.iw, "No stored code reviews.", FONT_BODY, theme.secondary, DT_WORDBREAK); return; }
    if (s->tab == PR_TAB_FINDINGS && !code_reviews_findings(r->items)) {
        doc_text(doc, c.ix, c.iw, "No findings registered in these reviews; check review status in Code reviews.", FONT_BODY, theme.secondary, DT_WORDBREAK); return;
    }
    bool wide = c.iw >= px(1100);
    int top = doc->y, gap = px(28);
    Col table = c, detail = c;
    if (wide) {
        table.iw = (c.iw - gap) / 2;
        detail.ix = table.ix + table.iw + gap; detail.iw = c.iw - table.iw - gap;
    }
    screen_pulls_reviews_table(s, doc, table, true);
    int first = (int)doc->count, body_top = doc->y;
    screen_pulls_reviews_table(s, doc, table, false);
    int last = (int)doc->count, bottom = doc->y;
    RECT viewport = s->base.pane ? pane_content_rect(s->base.pane) : (RECT){0, 0, doc->width, px(900)};
    int height = viewport.bottom - viewport.top - top - px(18);
    if (height < px(240)) height = px(240);
    if (!wide && height > px(240)) height = px(240);
    RECT view = {table.ix, body_top, table.ix + table.iw, wide ? top + height : body_top + height};
    doc_region(doc, first, last, &view, bottom);
    reveal_selection(s, doc, first, last);
    int table_bottom = view.bottom;
    doc->y = wide ? top : table_bottom + px(20);
    if (wide) {
        // A fine divider separates the two reading surfaces without adding another card.
        RECT rule = {detail.ix - gap / 2, top, detail.ix - gap / 2 + 1, table_bottom};
        int i = doc_add(doc, &rule, divider); doc_item(doc, i)->fill = theme.line;
    }
    first = (int)doc->count;
    screen_pulls_reviews_details(s, doc, detail);
    if (wide) {
        // The inspector scrolls independently, retaining its position on a background refresh.
        size_t index = doc->region_count;
        if (r->reset_inspector && index < doc->regions_kept) doc->regions[index].scroll = 0;
        RECT inspector = {detail.ix, top, detail.ix + detail.iw, table_bottom};
        doc_region(doc, first, (int)doc->count, &inspector, doc->y);
        doc->y = table_bottom;
    }
    r->reset_inspector = false;
}
