#include "screen_pulls_internal.h"
#include <math.h>

static time_t review_time(const PullScreen *s) { return s->reviews.clock ? s->reviews.clock() : time(NULL); }

static char *scope(PullScreen *s) {
    char *base = xstrfmt("%s|%s|%s|%s|%s", g_store.server ? g_store.server : "", g_store.device.id ? g_store.device.id : "",
        g_store.device.permission ? g_store.device.permission : "", s->project.repo, store_supports("code_reviews") ? "supported" : "unsupported");
    Str key; str_init(&key); str_appendf(&key, "%s|%d", base, s->number); free(base);
    for (size_t i = 0; i < g_store.device.repo_count; i++) str_appendf(&key, "|%s", g_store.device.repos[i]);
    return str_detach(&key);
}
void screen_pulls_reviews_clear(PullScreen *s) {
    PullReviews *r = &s->reviews;
    request_cancel(&r->req); json_free(r->items); free(r->scope); free(r->error);
    if (r->client) api_client_release(r->client);
    memset(r, 0, sizeof *r);
}
bool screen_pulls_reviews_sync(PullScreen *s) {
    PullReviews *r = &s->reviews; char *now = scope(s);
    if (r->client != g_store.client || !str_eq(r->scope, now)) {
        bool visible = r->visible; time_t (*clock)(void) = r->clock;
        screen_pulls_reviews_clear(s);
        r->visible = visible; r->clock = clock;
        r->scope = now; now = NULL;
        r->client = g_store.client ? api_client_retain(g_store.client) : NULL;
    }
    free(now);
    return r->client && store_supports("code_reviews");
}
// The first finding a review registered, or 0 when it has none.
static int64_t first_finding(const Json *review) { return code_review_id(json_at(json_get(review, "findings"), 0)); }
static void done(void *owner, Request *req) {
    PullScreen *s = owner;
    char *now = scope(s);
    bool current = req->client == g_store.client && str_eq(now, s->reviews.scope);
    free(now);
    if (!current) { screen_pulls_reviews_sync(s); pane_relayout(s->base.pane); return; }
    PullReviews *r = &s->reviews; r->read = true;
    if (!req->ok) {
        request_error_into(&r->error, req);
        if (req->error.retry_after > 0) r->retry_at = review_time(s) + (time_t)ceil(req->error.retry_after);
    } else {
        Json *items = code_reviews_parse(req->result);
        if (!items) set_string(&r->error, "The server returned an invalid code review list.");
        else {
            json_free(r->items); r->items = items; set_string(&r->error, NULL);
            const Json *review = code_review_find(items, r->review_id);
            // A review no longer among the latest 20 gives way to the table, which selects the newest.
            if (!review) { review = json_at(items, 0); r->review_id = code_review_id(review); screen_pulls_review_close(s); }
            if (!code_review_finding(review, r->finding_id)) { r->finding_id = first_finding(review); r->reset_inspector = true; r->reveal_row = true; }
        }
    }
    pane_relayout(s->base.pane);
}
void screen_pulls_reviews_load(PullScreen *s, bool refresh) {
    if (!screen_pulls_reviews_sync(s)) return;
    PullReviews *r = &s->reviews;
    if (!r->visible || r->req || r->retry_at > review_time(s)) return;
    if (s->tab != PR_TAB_CODE_REVIEWS && !r->read) return;
    if (!refresh && (r->read || r->next_poll > review_time(s))) return;
    Json *args = json_object(); json_set_str(args, "repo", s->project.repo); json_set_num(args, "pr", s->number);
    r->next_poll = review_time(s) + 30;
    store_call("code_reviews", args, 0, s, done, 0, &r->req);
}
void screen_pulls_reviews_poll(PullScreen *s) {
    if (!screen_pulls_reviews_sync(s)) return;
    if (!s->reviews.visible || s->reviews.next_poll > review_time(s)) return;
    if (s->tab != PR_TAB_CODE_REVIEWS) return;
    // Even completed-only or empty snapshots can gain reviews started elsewhere.
    screen_pulls_reviews_load(s, true);
}
void screen_pulls_reviews_visible(Screen *base, bool shown) {
    PullScreen *s = (PullScreen *)base;
    s->reviews.visible = shown;
    if (!shown) { request_cancel(&s->reviews.req); return; }
    screen_pulls_reviews_load(s, false);
    screen_pulls_reviews_poll(s);
}
void screen_pulls_review_open(PullScreen *s, int64_t id) {
    PullReviews *r = &s->reviews;
    const Json *review = code_review_find(r->items, id);
    if (!review) return;
    // Opening the same review again keeps the finding last read in it. A review without findings shows its summary,
    // which stays unfolded when its first findings arrive.
    if (!code_review_finding(review, r->finding_id)) r->finding_id = first_finding(review);
    if (!json_count(json_get(review, "findings"))) r->summary_id = id;
    r->review_id = id; r->open = true; r->reveal_row = true; r->reset_inspector = true;
    if (!s->base.pane) return;
    r->list_scroll = pane_scroll_y(s->base.pane);
    pane_scroll_to_top(s->base.pane); pane_relayout(s->base.pane);
}
void screen_pulls_review_close(PullScreen *s) {
    if (!s->reviews.open) return;
    s->reviews.open = false;
    // The table comes back where it was left; another tab, refreshed meanwhile, keeps its own place.
    if (!s->base.pane || s->tab != PR_TAB_CODE_REVIEWS) return;
    pane_relayout(s->base.pane); pane_scroll_after_layout(s->base.pane, s->reviews.list_scroll);
}
bool screen_pulls_reviews_back(Screen *base) {
    PullScreen *s = (PullScreen *)base;
    if (s->tab != PR_TAB_CODE_REVIEWS || !s->reviews.open) return false;
    screen_pulls_review_close(s);
    return true;
}
// The page offset that brings content rows [top, bottom) into view under the pinned header with the least scrolling,
// 8px clear of either edge.
static int reveal_offset(Pane *pane, int top, int bottom, int pin_bottom) {
    RECT view = pane_content_rect(pane);
    int scroll = pane_scroll_y(pane), height = view.bottom - view.top;
    if (top < scroll + pin_bottom) return top - pin_bottom - px(8);
    if (bottom > scroll + height) return bottom - height + px(8);
    return scroll;
}
// Keeps the selected review row in view on the page, which lays the table out without a region of its own.
static void reveal_row(PullScreen *s) {
    Pane *pane = s->base.pane;
    Doc d; doc_init(&d); doc_begin(&d, NULL, pane_content_width(pane)); s->base.vt->layout(&s->base, &d);
    for (size_t i = 0; i < d.count; i++) {
        Item *it = &d.items[i];
        if (it->action != ACT_REVIEW_ROW || it->arg != (intptr_t)s->reviews.review_id) continue;
        pane_scroll_after_layout(pane, reveal_offset(pane, it->rc.top, it->rc.bottom, d.pin_bottom)); break;
    }
    doc_free(&d);
}
static bool navigation_key(WPARAM vk) { return vk == VK_UP || vk == VK_DOWN || vk == VK_HOME || vk == VK_END; }
static size_t index_of(const Json *records, int64_t id) {
    for (size_t i = 0; i < json_count(records); i++) if (code_review_id(json_at(records, i)) == id) return i;
    return SIZE_MAX;
}
// Where a navigation key moves the selection among `count` rows; without one, it starts from an end.
static size_t step(size_t at, size_t count, WPARAM vk) {
    if (vk == VK_HOME || (at == SIZE_MAX && vk == VK_DOWN)) return 0;
    if (vk == VK_END || at == SIZE_MAX) return count - 1;
    if (vk == VK_UP) return at ? at - 1 : 0;
    return at + 1 < count ? at + 1 : at;
}
// Up/Down and Home/End select findings, keeping their table in view on the page.
static bool finding_key(PullScreen *s, WPARAM vk) {
    const Json *findings = json_get(code_review_find(s->reviews.items, s->reviews.review_id), "findings");
    if (!json_count(findings)) return false;
    s->reviews.finding_id = code_review_id(json_at(findings, step(index_of(findings, s->reviews.finding_id), json_count(findings), vk)));
    s->reviews.reveal_row = true; s->reviews.reset_inspector = true;
    // The table's region keeps its place on the page whichever finding is selected.
    Pane *pane = s->base.pane; pane_relayout(pane);
    Doc *doc = pane_doc(pane);
    if (doc->region_count) pane_scroll_after_layout(pane, reveal_offset(pane, doc->regions[0].view.top, doc->regions[0].view.bottom, doc->pin_bottom));
    return true;
}
// PageUp/PageDown page the wide inspector, which scrolls on its own beside the table.
static bool inspector_key(PullScreen *s, WPARAM vk) {
    Doc *doc = pane_doc(s->base.pane);
    if (doc->region_count < 2) return false;
    DocRegion *detail = &doc->regions[1];
    int page = detail->view.bottom - detail->view.top - px(32);
    if (page < px(32)) page = px(32);
    bool moved = doc_region_wheel(doc, detail->view.left, detail->view.top, vk == VK_NEXT ? page : -page);
    if (moved) InvalidateRect(pane_hwnd(s->base.pane), NULL, FALSE);
    return moved;
}
// Leaving an opened review is the pane's back (screen_pulls_reviews_back), so the header button and back keys agree.
bool screen_pulls_reviews_key(Screen *base, WPARAM vk, bool ctrl, bool shift) {
    PullScreen *s = (PullScreen *)base;
    if (ctrl || shift || !base->pane || s->tab != PR_TAB_CODE_REVIEWS) return false;
    if (!screen_pulls_reviews_sync(s) || !s->reviews.items) return false;
    if (s->reviews.open) {
        if (vk == VK_PRIOR || vk == VK_NEXT) return inspector_key(s, vk);
        return navigation_key(vk) && finding_key(s, vk);
    }
    // In the table, Up/Down and Home/End select a review and Enter opens its findings.
    const Json *items = s->reviews.items;
    if (vk == VK_RETURN && code_review_find(items, s->reviews.review_id)) { screen_pulls_review_open(s, s->reviews.review_id); return true; }
    if (!navigation_key(vk) || !json_count(items)) return false;
    s->reviews.review_id = code_review_id(json_at(items, step(index_of(items, s->reviews.review_id), json_count(items), vk)));
    pane_relayout(base->pane); reveal_row(s);
    return true;
}
