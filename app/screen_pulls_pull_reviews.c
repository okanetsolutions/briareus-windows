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
            if (!review) { r->review_id = 0; r->finding_id = 0; }
            else if (r->finding_id && !code_review_finding(review, r->finding_id)) r->finding_id = 0;
        }
    }
    pane_relayout(s->base.pane);
}
void screen_pulls_reviews_load(PullScreen *s, bool refresh) {
    if (!screen_pulls_reviews_sync(s)) return;
    PullReviews *r = &s->reviews;
    if (!r->visible || r->req || r->retry_at > review_time(s)) return;
    bool visible = s->tab == PR_TAB_FINDINGS || s->tab == PR_TAB_CODE_REVIEWS;
    if (!visible && !r->read) return;
    if (!refresh && (r->read || r->next_poll > review_time(s))) return;
    Json *args = json_object(); json_set_str(args, "repo", s->project.repo); json_set_num(args, "pr", s->number);
    r->next_poll = review_time(s) + 30;
    store_call("code_reviews", args, 0, s, done, 0, &r->req);
}
void screen_pulls_reviews_poll(PullScreen *s) {
    if (!screen_pulls_reviews_sync(s)) return;
    if (!s->reviews.visible || s->reviews.next_poll > review_time(s)) return;
    if (s->tab != PR_TAB_FINDINGS && s->tab != PR_TAB_CODE_REVIEWS) return;
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
bool screen_pulls_reviews_key(Screen *base, WPARAM vk, bool ctrl, bool shift) {
    PullScreen *s = (PullScreen *)base;
    if (ctrl || shift) return false;
    if (s->tab != PR_TAB_FINDINGS && s->tab != PR_TAB_CODE_REVIEWS) return false;
    if (vk != VK_UP && vk != VK_DOWN && vk != VK_HOME && vk != VK_END) return false;
    if (!screen_pulls_reviews_sync(s)) return false;
    bool findings = s->tab == PR_TAB_FINDINGS;
    size_t total = findings ? code_reviews_findings(s->reviews.items) : json_count(s->reviews.items);
    if (!total) return false;
    size_t selected = SIZE_MAX, n = 0;
    for (size_t i = 0; i < json_count(s->reviews.items); i++) {
        const Json *r = json_at(s->reviews.items, i), *rows = findings ? json_get(r, "findings") : NULL;
        for (size_t j = 0; j < (findings ? json_count(rows) : 1); j++, n++) {
            if (code_review_id(r) != s->reviews.review_id) continue;
            if (!findings || code_review_id(json_at(rows, j)) == s->reviews.finding_id) selected = n;
        }
    }
    if (selected == SIZE_MAX) { selected = vk == VK_UP || vk == VK_END ? total - 1 : 0; }
    else {
        if (vk == VK_HOME) selected = 0;
        if (vk == VK_END) selected = total - 1;
        if (vk == VK_UP && selected) selected--;
        if (vk == VK_DOWN && selected + 1 < total) selected++;
    }
    n = 0;
    for (size_t i = 0; i < json_count(s->reviews.items); i++) {
        const Json *r = json_at(s->reviews.items, i), *rows = json_get(r, "findings");
        for (size_t j = 0; j < (findings ? json_count(rows) : 1); j++, n++) if (n == selected) {
            s->reviews.review_id = code_review_id(r);
            s->reviews.finding_id = findings ? code_review_id(json_at(rows, j)) : 0;
        }
    }
    pane_relayout(base->pane);
    // Follow the selected stable record, including rows beyond the visible viewport.
    Doc d; doc_init(&d); doc_begin(&d, NULL, pane_content_width(base->pane)); base->vt->layout(base, &d);
    int action = findings ? ACT_REVIEW_FINDING : ACT_REVIEW_ROW;
    intptr_t id = (intptr_t)(findings ? s->reviews.finding_id : s->reviews.review_id);
    for (size_t i = 0; i < d.count; i++) if (d.items[i].action == action && d.items[i].arg == id) { pane_scroll_to(base->pane, d.items[i].rc.top - d.pin_bottom); break; }
    doc_free(&d); return true;
}
