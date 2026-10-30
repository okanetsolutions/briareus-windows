#include "screen_pulls_internal.h"
#include "screen_fixture.h"
#include "suites.h"
#include "fixtures/code_reviews.h"
#include "fixtures/pull_review_polish.h"

static const char *retry_header;
static time_t clock_now;
static time_t fake_clock(void) { return clock_now; }
static bool review_transport(void *ctx, const char *method, const char *url, const char *const *headers,
    const void *body, size_t bytes, int timeout, int *status, char **type, char **retry, char **response, size_t *len, char **error) {
    bool ok = screen_fixture_transport(ctx, method, url, headers, body, bytes, timeout, status, type, retry, response, len, error);
    if (retry_header) { free(*retry); *retry = xstrdup(retry_header); }
    return ok;
}
static Route routes[] = {{"GET", "/pulls/{number}/code-reviews", "read"}};
static PullScreen *setup(ScreenFixture *f) {
    screen_fixture_init(f, routes, 1); retry_header = NULL; api_client_set_transport(f->client, review_transport, f); g_store.device.permission = "read"; g_store.device.id = "reader";
    Project p = {.repo = "o/r"}; PullScreen *s = (PullScreen *)pull_detail_screen_new(&p, 12, NULL, NULL);
    screen_fixture_pane(f, &s->base);
    json_free(s->pr); s->pr = json_parsez("{\"title\":\"Synthetic PR\",\"state\":\"open\",\"headSha\":\"aaaaaaaaaa1111\"}");
    f->read_response = CODE_REVIEWS_FIXTURE;
    s->tab = PR_TAB_FINDINGS; s->shown = true; s->reviews.visible = true;
    clock_now = 1000; s->reviews.clock = fake_clock; return s;
}
static void cleanup(ScreenFixture *f, PullScreen *s) { s->base.vt->destroy(&s->base); screen_fixture_clear(f); }
static bool contains(Doc *d, const char *text) {
    for (size_t i = 0; i < d->count; i++) if (d->items[i].text && strstr(d->items[i].text, text)) return true;
    return false;
}
static void draw(PullScreen *s, Doc *d, int width) { doc_begin(d, NULL, width); s->base.vt->layout(&s->base, d); doc_end(d); }
static void shared_read_and_tables(void) {
    ScreenFixture f; PullScreen *s = setup(&f);
    screen_pulls_reviews_load(s, false); screen_fixture_pump(&f, 1);
    CHECK_INT(f.reads, 1); CHECK_INT(f.writes, 0); CHECK(s->reviews.error == NULL);
    Doc d; doc_init(&d);
    const int widths[] = {1200, 390};
    for (size_t w = 0; w < 2; w++) {
        draw(s, &d, widths[w]);
        CHECK(contains(&d, "3 findings")); CHECK(contains(&d, "Latest 20"));
        CHECK(contains(&d, "current head")); CHECK(contains(&d, "older head")); CHECK(contains(&d, "pending"));
        int rows = 0;
        for (size_t i = 0; i < d.count; i++) {
            Item *it = &d.items[i];
            if (it->action != ACT_REVIEW_FINDING) continue;
            rows++; RECT r = it->rc;
            int hit = doc_hit(&d, r.right - px(3), (r.top + r.bottom) / 2);
            if (r.top >= d.regions[0].view.top && r.bottom <= d.regions[0].view.bottom) {
                CHECK(hit >= 0); if (hit >= 0) CHECK_INT(d.items[hit].arg, it->arg);
            }
            if (it->arg == 102) s->base.vt->action(&s->base, it->action, it->arg, (POINT){0});
        }
        CHECK_INT(rows, 3); CHECK_INT(s->reviews.review_id, 21); CHECK_INT(s->reviews.finding_id, 102);
        draw(s, &d, widths[w]); CHECK(contains(&d, "Verification rationale")); CHECK(contains(&d, "Confirmed") || contains(&d, "confirmed"));
        CHECK(contains(&d, "Unknown in / 0 out"));
    }
    s->base.vt->action(&s->base, ACT_PR_TAB, PR_TAB_CODE_REVIEWS, (POINT){0});
    CHECK_INT(f.reads, 1); draw(s, &d, 1200);
    CHECK(contains(&d, "Review / commit")); CHECK(contains(&d, "Findings"));
    s->base.vt->action(&s->base, ACT_REVIEW_ROW, 20, (POINT){0}); draw(s, &d, 390);
    CHECK(contains(&d, "Historical review summary")); s->reviews.metadata_open = true; draw(s, &d, 390);
    CHECK(contains(&d, "$0.0123 estimated")); CHECK(contains(&d, "partial cost"));
    CHECK_INT(s->run_count, 0); CHECK_INT(f.reads, 1); CHECK_INT(f.writes, 0);
    CHECK(s->base.vt->key(&s->base, VK_HOME, false, false)); CHECK_INT(s->reviews.row_review_id, 21);
    s->base.vt->action(&s->base, ACT_PR_TAB, PR_TAB_FINDINGS, (POINT){0});
    CHECK(s->base.vt->key(&s->base, VK_END, false, false)); CHECK_INT(s->reviews.review_id, 20); CHECK_INT(s->reviews.finding_id, 99);
    CHECK(s->base.vt->key(&s->base, VK_UP, false, false)); CHECK_INT(s->reviews.finding_id, 102);
    CHECK(s->base.vt->key(&s->base, VK_DOWN, false, false)); CHECK_INT(s->reviews.finding_id, 99);
    CHECK(!s->base.vt->key(&s->base, VK_DOWN, true, false));
    doc_free(&d); cleanup(&f, s);
}
static void empty_failure_unsupported(void) {
    ScreenFixture f; PullScreen *s = setup(&f); Doc d; doc_init(&d);
    g_store.route_count = 0; screen_pulls_reviews_load(s, false); CHECK_INT(f.reads, 0);
    draw(s, &d, 900); CHECK(contains(&d, "not supported")); CHECK(!contains(&d, "0 findings"));
    g_store.route_count = 1; f.read_status = 503; f.read_response = "{\"error\":\"Review database unreadable\"}";
    screen_pulls_reviews_load(s, false); screen_fixture_pump(&f, 1);
    draw(s, &d, 900); CHECK(contains(&d, "database unreadable")); CHECK(!contains(&d, "0 findings")); CHECK(s->reviews.items == NULL);
    f.read_status = 200; f.read_response = "{}";
    screen_pulls_reviews_load(s, true); screen_fixture_pump(&f, 1); CHECK(s->reviews.error != NULL); CHECK(s->reviews.items == NULL);
    f.read_response = "{\"codeReviews\":[]}";
    screen_pulls_reviews_load(s, true); screen_fixture_pump(&f, 1);
    draw(s, &d, 900); CHECK(s->reviews.error == NULL); CHECK(contains(&d, "No stored code reviews"));
    CHECK(contains(&d, "0 findings"));
    f.read_status = 503; screen_pulls_reviews_load(s, true); screen_fixture_pump(&f, 1);
    draw(s, &d, 900); CHECK(contains(&d, "Previous snapshot"));
    CHECK_INT(f.writes, 0); doc_free(&d); cleanup(&f, s);
}
static void refresh_replacement_and_stale(void) {
    for (int change = 0; change < 6; change++) {
        ScreenFixture f; PullScreen *s = setup(&f);
        screen_pulls_reviews_load(s, false); screen_fixture_pump(&f, 1);
        s->base.vt->action(&s->base, ACT_REVIEW_FINDING, 102, (POINT){0});
        f.block_read = true; screen_pulls_reviews_load(s, true);
        CHECK_INT(WaitForSingleObject(f.entered, 5000), WAIT_OBJECT_0);
        screen_pulls_reviews_load(s, false); screen_pulls_reviews_load(s, true);
        CHECK_INT(f.reads, 2); // One request shared by tabs, even while a poll is held.
        ApiClient *original = g_store.client; char *repo = s->project.repo;
        if (change == 1) g_store.device.id = "other-device";
        if (change == 2) g_store.client = NULL;
        if (change == 3) g_store.server = "https://another.example";
        if (change == 4) g_store.route_count = 0;
        if (change == 5) s->project.repo = "other/repo";
        SetEvent(f.resume); screen_fixture_pump(&f, 1);
        if (change) { CHECK(s->reviews.items == NULL); CHECK_INT(s->reviews.review_id, 0); }
        else {
            CHECK_INT(s->reviews.review_id, 21); CHECK_INT(s->reviews.finding_id, 102);
            f.block_read = false;
            f.read_response = "{\"codeReviews\":[{\"id\":21,\"status\":\"completed\",\"findings\":[{\"id\":103,\"severity\":\"low\",\"title\":\"Replacement\"}]}]}";
            screen_pulls_reviews_load(s, true); screen_fixture_pump(&f, 1);
            CHECK_INT(s->reviews.review_id, 21); CHECK_INT(s->reviews.finding_id, 103);
            CHECK(code_review_finding(code_review_find(s->reviews.items, 21), 102) == NULL);
        }
        s->project.repo = repo; g_store.client = original; CHECK_INT(f.writes, 0); cleanup(&f, s);
    }
}
static void cancel_on_destroy(void) {
    ScreenFixture f; PullScreen *s = setup(&f); f.block_read = true;
    screen_pulls_reviews_load(s, false); CHECK_INT(WaitForSingleObject(f.entered, 5000), WAIT_OBJECT_0);
    s->base.vt->destroy(&s->base); SetEvent(f.resume); screen_fixture_pump(&f, 1);
    CHECK_INT(f.writes, 0); screen_fixture_clear(&f);
}
static void bounded_polling_retry_and_visibility(void) {
    ScreenFixture f; PullScreen *s = setup(&f);
    screen_pulls_reviews_load(s, false); screen_fixture_pump(&f, 1);
    for (int i = 0; i < 5; i++) screen_pulls_reviews_poll(s);
    CHECK_INT(f.reads, 1);
    s->reviews.next_poll = 0; screen_pulls_reviews_poll(s); screen_fixture_pump(&f, 1);
    CHECK_INT(f.reads, 2);
    f.read_status = 503; retry_header = "60"; f.read_response = "{\"error\":\"Unavailable\"}";
    s->reviews.next_poll = 0; screen_pulls_reviews_poll(s); screen_fixture_pump(&f, 1);
    CHECK(s->reviews.items != NULL); CHECK(s->reviews.error != NULL); CHECK(s->reviews.retry_at >= fake_clock() + 59);
    for (int i = 0; i < 5; i++) screen_pulls_reviews_poll(s);
    CHECK_INT(f.reads, 3);
    s->reviews.retry_at = fake_clock() + 60; s->reviews.next_poll = 0;
    screen_pulls_reviews_load(s, true); screen_pulls_reviews_poll(s); CHECK_INT(f.reads, 3);
    s->reviews.retry_at = 0; retry_header = NULL; f.read_status = 200;
    f.read_response = "{\"codeReviews\":[{\"id\":21,\"status\":\"failed\",\"findings\":[]}]}";
    screen_pulls_reviews_load(s, true); screen_fixture_pump(&f, 1);
    screen_pulls_reviews_poll(s); CHECK_INT(f.reads, 4);
    f.block_read = true; screen_pulls_reviews_load(s, true);
    CHECK_INT(WaitForSingleObject(f.entered, 5000), WAIT_OBJECT_0);
    s->base.vt->window_visible(&s->base, false); CHECK(s->reviews.req == NULL);
    SetEvent(f.resume); screen_fixture_pump(&f, 1);
    CHECK_STR(json_str(json_get(json_at(s->reviews.items, 0), "status")), "failed");
    Doc d; doc_init(&d); s->tab = PR_TAB_CODE_REVIEWS; theme_set_dpi(192); draw(s, &d, 780);
    CHECK(d.content_width > d.width); CHECK(contains(&d, "Scroll horizontally"));
    theme_set_dpi(96); doc_free(&d); cleanup(&f, s);
}
static void terminal_labels_across_tables_and_details(void) {
    ScreenFixture f; PullScreen *s = setup(&f); f.read_response = CODE_REVIEWS_FINAL_NULL_FIXTURE;
    screen_pulls_reviews_load(s, false); screen_fixture_pump(&f, 1);
    Doc d; doc_init(&d);
    for (int tab = 0; tab < 2; tab++) for (int review = 0; review < 2; review++) {
        s->tab = tab ? PR_TAB_CODE_REVIEWS : PR_TAB_FINDINGS;
        s->reviews.review_id = s->reviews.row_review_id = review ? 23 : 22; s->reviews.finding_id = review ? 202 : 201;
        draw(s, &d, 1200);
        CHECK(contains(&d, "no verdict")); CHECK(contains(&d, "not verified"));
        char *counts = code_reviews_counts(s->reviews.items);
        CHECK_STR(counts, "2 findings: 0 critical, 0 high, 2 medium, 0 low; 0 pending verdicts; 2 without verdict"); free(counts);
        CHECK(!contains(&d, "1 pending verdict")); CHECK(!contains(&d, "partial cost"));
        CHECK(contains(&d, review ? "stopped before verification" : "Inconclusive:"));

    }
    json_free(s->reviews.items); s->reviews.items = NULL; s->reviews.read = false;
    clock_now += 30; f.read_response = CODE_REVIEWS_FIXTURE;
    screen_pulls_reviews_load(s, false); screen_fixture_pump(&f, 1);
    s->tab = PR_TAB_FINDINGS; s->reviews.review_id = 21; s->reviews.finding_id = 101;
    draw(s, &d, 390); char *counts = code_reviews_counts(s->reviews.items);
    CHECK(strstr(counts, "1 pending verdict; 0 without verdict") != NULL); free(counts);
    CHECK(contains(&d, "pending")); CHECK(!contains(&d, "not verified"));
    doc_free(&d); cleanup(&f, s);
}
static void completed_snapshots_discover_new_reviews(void) {
    static Route action_routes[] = {{"GET", "/pulls/{number}/code-reviews", "read"}, {"POST", "/actions", "manage"}};
    // PR start, board start, external review, new PR head, and an uncertain PR start.
    for (int source = 0; source < 5; source++) {
        ScreenFixture f; PullScreen *s = setup(&f);
        g_store.routes = action_routes; g_store.route_count = 2;
        g_store.device.permission = "admin"; json_set_str(s->pr, "headRef", "topic");
        f.read_response = CODE_REVIEWS_FINAL_NULL_FIXTURE;
        screen_pulls_reviews_load(s, false); screen_fixture_pump(&f, 1);
        CHECK(!code_reviews_running(s->reviews.items));
        s->reviews.review_id = 22; s->reviews.finding_id = 201;
        BoardAction action = {.id = "review", .advertised = true};
        if (source == 0 || source == 4) {
            f.fail_write = source == 4;
            screen_pulls_start_action(s, &action, NULL, NULL); screen_fixture_pump(&f, 1);
            CHECK_INT(f.writes, 1); CHECK(!s->busy); CHECK(s->uncertain == (source == 4));
        }
        if (source == 1) {
            PullsScreen board = {0}; board.project.repo = "o/r"; board.base.pane = f.pane;
            screen_pulls_board_start(&board, 12, "topic", &action, NULL, NULL); screen_fixture_pump(&f, 1);
            CHECK_INT(f.writes, 1); CHECK(!board.busy); CHECK(board.write_error == NULL);
            free(board.start_device); free(board.starting_id); free(board.write_error);
        }
        if (source == 3) json_set_str(s->pr, "headSha", "new-head");
        Json *payload = json_parsez(CODE_REVIEWS_FINAL_NULL_FIXTURE);
        Json *rows = (Json *)json_get(payload, "codeReviews");
        Json *r = json_object(); json_set_num(r, "id", 24); json_set_str(r, "status", "running");
        json_set_str(r, "headSha", source == 3 ? "new-head" : "aaaaaaaaaa1111"); json_object_set(r, "findings", json_array());
        Json *newest = json_array(); json_array_push(newest, r);
        for (size_t i = 0; i < json_count(rows); i++) json_array_push(newest, json_clone(json_at(rows, i)));
        json_object_set(payload, "codeReviews", newest); char *running = json_serialize(payload, false); f.read_response = running;
        clock_now += 29;
        screen_pulls_reviews_poll(s); screen_pulls_reviews_load(s, false); CHECK_INT(f.reads, 1);
        clock_now++;
        screen_pulls_reviews_poll(s); screen_pulls_reviews_poll(s);
        screen_fixture_pump(&f, 1); CHECK_INT(f.reads, 2);
        CHECK(code_review_find(s->reviews.items, 24) != NULL); CHECK(code_reviews_running(s->reviews.items));
        CHECK_INT(s->reviews.review_id, 22); CHECK_INT(s->reviews.finding_id, 201);
        CHECK_STR(code_review_head(code_review_find(s->reviews.items, 24), json_str(json_get(s->pr, "headSha"))), "current head");
        json_set_str(r, "status", "completed"); char *completed = json_serialize(payload, false); f.read_response = completed;
        s->base.vt->action(&s->base, ACT_PR_TAB, PR_TAB_CODE_REVIEWS, (POINT){0});
        CHECK_INT(f.reads, 2); // Switching tabs shares the snapshot and deadline.
        clock_now += 30; screen_pulls_reviews_poll(s); screen_fixture_pump(&f, 1);
        CHECK_INT(f.reads, 3); CHECK(!code_reviews_running(s->reviews.items));
        s->base.vt->window_visible(&s->base, false); clock_now += 60;
        screen_pulls_reviews_poll(s); screen_pulls_reviews_load(s, false); screen_pulls_reviews_load(s, true);
        CHECK_INT(f.reads, 3);
        s->base.vt->window_visible(&s->base, true); screen_fixture_pump(&f, 1); CHECK_INT(f.reads, 4);
        s->tab = PR_TAB_BODY; clock_now += 30; screen_pulls_reviews_poll(s); CHECK_INT(f.reads, 4);
        s->base.vt->action(&s->base, ACT_PR_TAB, PR_TAB_FINDINGS, (POINT){0});
        screen_fixture_pump(&f, 1); CHECK_INT(f.reads, 5);
        CHECK_INT(f.writes, source == 0 || source == 1 || source == 4 ? 1 : 0);
        CHECK(s->uncertain == (source == 4));
        free(running); free(completed); json_free(payload); cleanup(&f, s);
    }
}
static void cancelled_initial_read_keeps_cadence(void) {
    ScreenFixture f; PullScreen *s = setup(&f); f.block_read = true;
    screen_pulls_reviews_load(s, false); CHECK_INT(WaitForSingleObject(f.entered, 5000), WAIT_OBJECT_0);
    s->base.vt->window_visible(&s->base, false); SetEvent(f.resume); screen_fixture_pump(&f, 1);
    CHECK(!s->reviews.read); CHECK(s->reviews.items == NULL);
    s->base.vt->window_visible(&s->base, true); screen_pulls_reviews_poll(s);
    clock_now += 29; screen_pulls_reviews_poll(s); CHECK_INT(f.reads, 1);
    clock_now++; f.block_read = false; screen_pulls_reviews_poll(s); screen_fixture_pump(&f, 1);
    CHECK_INT(f.reads, 2); CHECK(s->reviews.items != NULL); cleanup(&f, s);
}
static void inspector_selection_and_scroll(void) {
    ScreenFixture f; PullScreen *s = setup(&f);
    Json *payload = pull_review_polish_fixture(); char *fixture = json_serialize(payload, false);
    f.read_response = fixture; screen_pulls_reviews_load(s, false); screen_fixture_pump(&f, 1);
    CHECK_INT(s->reviews.finding_id, 101); CHECK_INT(s->reviews.row_review_id, 21);
    Doc d; doc_init(&d);
    const int widths[] = {1652, 390, 780};
    for (size_t n = 0; n < 3; n++) {
        theme_set_dpi(n == 2 ? 192 : 96);
        RECT bounds = {0, 0, widths[n], 800}; pane_set_bounds(f.pane, &bounds);
        draw(s, &d, widths[n]); CHECK_INT(d.content_width, d.width);
        CHECK_INT(d.region_count, n == 0 ? 2 : 1);
        // Every row remains reachable inside a bounded table; the inspector has its own region on wide panes.
        CHECK(d.regions[0].max > 0);
        DocRegion *table = &d.regions[0];
        CHECK(doc_region_wheel(&d, table->view.left, table->view.top, table->max));
        int scrolled = d.regions[0].scroll;
        s->reviews.reveal_row = true; draw(s, &d, widths[n]);
        CHECK(d.regions[0].scroll < scrolled);
        int selected = -1;
        for (size_t i = 0; i < d.count; i++) if (d.items[i].action == ACT_REVIEW_FINDING && d.items[i].arg == 101) selected = (int)i;
        CHECK(selected >= 0);
        if (selected >= 0) {
            RECT row = d.items[selected].rc;
            CHECK(row.top >= d.regions[0].view.top); CHECK(row.bottom <= d.regions[0].view.bottom);
            CHECK_INT(doc_hit(&d, row.left + px(3), row.top + px(3)), selected);
        }
        if (n == 0) {
            CHECK(d.regions[1].view.left > d.regions[0].view.right);
            int before = d.regions[0].scroll;
            doc_region_wheel(&d, d.regions[1].view.left, d.regions[1].view.top, px(80));
            CHECK_INT(d.regions[0].scroll, before);
        }
        // Rebuilding for a refresh preserves the table position and selected record.
        int before = d.regions[0].scroll; draw(s, &d, widths[n]); CHECK_INT(d.regions[0].scroll, before);
    }
    theme_set_dpi(96);
    s->base.vt->action(&s->base, ACT_REVIEW_FINDING, 105, (POINT){0});
    s->base.vt->action(&s->base, ACT_PR_TAB, PR_TAB_CODE_REVIEWS, (POINT){0});
    s->base.vt->action(&s->base, ACT_REVIEW_ROW, 20, (POINT){0});
    CHECK_INT(s->reviews.row_review_id, 20); CHECK_INT(s->reviews.finding_id, 105); CHECK_INT(s->reviews.review_id, 21);
    s->base.vt->action(&s->base, ACT_PR_TAB, PR_TAB_FINDINGS, (POINT){0});
    screen_pulls_reviews_load(s, true); screen_fixture_pump(&f, 1);
    CHECK_INT(s->reviews.finding_id, 105); CHECK_INT(s->reviews.row_review_id, 20);
    draw(s, &d, 1652);
    CHECK(contains(&d, "Finding #105"));
    CHECK_INT(f.writes, 0);
    s->base.vt->action(&s->base, ACT_REVIEW_FINDING, 202, (POINT){0});
    s->reviews.reveal_row = true; draw(s, &d, 390);
    int old_scroll = d.regions[0].scroll; CHECK(old_scroll > 0);
    Json *removed = (Json *)json_at(json_get(payload, "codeReviews"), 2);
    json_object_set(removed, "findings", json_array());
    char *updated = json_serialize(payload, false); f.read_response = updated;
    screen_pulls_reviews_load(s, true); screen_fixture_pump(&f, 1); draw(s, &d, 390);
    CHECK_INT(s->reviews.finding_id, 101); CHECK(d.regions[0].scroll < old_scroll);
    doc_free(&d); free(updated); free(fixture); json_free(payload); cleanup(&f, s);
}
static void inspector_keyboard_page_visibility(void) {
    ScreenFixture f; PullScreen *s = setup(&f);
    Json *payload = pull_review_polish_fixture(); char *fixture = json_serialize(payload, false); f.read_response = fixture;
    screen_pulls_reviews_load(s, false); screen_fixture_pump(&f, 1);
    pane_set_root(f.pane, &s->base); poller_stop(&s->poller);
    RECT bounds = {0, 0, 390, 800}; pane_set_bounds(f.pane, &bounds);
    UpdateWindow(pane_hwnd(f.pane));
    Doc *doc = pane_doc(f.pane); CHECK_INT(doc->region_count, 1);
    pane_scroll_to(f.pane, doc_height(doc)); UpdateWindow(pane_hwnd(f.pane));
    int before = pane_scroll_y(f.pane); CHECK(before > 0);
    CHECK(s->base.vt->key(&s->base, VK_HOME, false, false)); UpdateWindow(pane_hwnd(f.pane));
    CHECK(pane_scroll_y(f.pane) < before);
    CHECK(doc->regions[0].view.top >= pane_scroll_y(f.pane) + doc->pin_bottom);
    // Force a long stored report so PageDown must scroll the inspector, not the page.
    Json *finding = (Json *)code_review_finding(code_review_find(s->reviews.items, 21), 101);
    Str body; str_init(&body);
    for (int i = 0; i < 50; i++) str_appendz(&body, "A synthetic paragraph with **evidence** and a [reference](https://example.com/synthetic).\n\n");
    json_set_str(finding, "body", body.data); str_free(&body);
    bounds.right = 1652; pane_set_bounds(f.pane, &bounds); pane_relayout(f.pane); UpdateWindow(pane_hwnd(f.pane));
    CHECK_INT(doc->region_count, 2); CHECK(doc->regions[1].max > 0);
    int table = doc->regions[0].scroll, page = pane_scroll_y(f.pane);
    CHECK(s->base.vt->key(&s->base, VK_NEXT, false, false)); CHECK(doc->regions[1].scroll > 0);
    CHECK_INT(doc->regions[0].scroll, table); CHECK_INT(pane_scroll_y(f.pane), page);
    CHECK(s->base.vt->key(&s->base, VK_PRIOR, false, false)); CHECK_INT(doc->regions[1].scroll, 0);
    CHECK_INT(s->reviews.finding_id, 101); CHECK_INT(f.writes, 0);
    pane_set_root(f.pane, NULL); screen_fixture_clear(&f); json_free(payload); free(fixture);
}
static void usage_cells_and_totals(void) {
    CodeReviewAmount unknown = {0, 0, 2}, zero = {0, 2, 0}, partial = {1240, 1, 2};
    CHECK_OWNED_STR(screen_pulls_review_amount(unknown, false, false, 0), "Unknown");
    CHECK_OWNED_STR(screen_pulls_review_amount(unknown, true, true, 2), "Unknown\n2 unpriced turns");
    CHECK_OWNED_STR(screen_pulls_review_amount(zero, true, false, 0), "$0.00");
    CHECK_OWNED_STR(screen_pulls_review_amount(partial, false, false, 0), "1.2k\npartial · 2 unknown");
    CodeReviewAmount price = {0.0042, 1, 1};
    CHECK_OWNED_STR(screen_pulls_review_amount(price, true, true, 3), "<$0.01 estimated\npartial · 1 unknown · 3 unpriced turns");
    ScreenFixture f; PullScreen *s = setup(&f);
    screen_pulls_reviews_load(s, false); screen_fixture_pump(&f, 1); s->tab = PR_TAB_CODE_REVIEWS;
    Doc d; doc_init(&d); draw(s, &d, 1652);
    CHECK_INT(d.region_count, 0); CHECK_INT(d.content_width, d.width);
    CHECK(contains(&d, "Input tokens")); CHECK(contains(&d, "Output tokens")); CHECK(contains(&d, "2 displayed reviews"));
    CHECK(contains(&d, "partial · 1 unknown")); CHECK(contains(&d, "3 unpriced turns"));
    draw(s, &d, 390); CHECK(d.content_width > d.width); CHECK_INT(d.region_count, 0);
    CHECK_INT(f.writes, 0); doc_free(&d); cleanup(&f, s);
}
void app_pull_reviews_tests(void) {
    screen_fixture_prepare();
    test_run("finding keyboard navigation reveals the table and pages the wide inspector independently", inspector_keyboard_page_visibility);
    test_run("review history shows usage columns and scoped totals with honest unknown and zero formatting", usage_cells_and_totals);
    test_run("review inspector retains independent tab selections and bounded scrolling across refresh and DPI", inspector_selection_and_scroll);
    test_run("cancelled initial review reads retain their deadline through hide and reveal", cancelled_initial_read_keeps_cadence);
    test_run("completed and failed nullable findings are final across tables, details and totals", terminal_labels_across_tables_and_details);
    test_run("completed-only PR snapshots discover started, external and new-head reviews on a fake clock", completed_snapshots_discover_new_reviews);
    test_run("PR review tabs share one read and render selectable wide/narrow tables without sessions", shared_read_and_tables);
    test_run("PR reviews distinguish unsupported, malformed, failed and empty reads", empty_failure_unsupported);
    test_run("PR reviews preserve stable selection and reject obsolete connection/project results", refresh_replacement_and_stale);
    test_run("PR review polling is bounded, honors retry deadlines, refreshes completed lists and cancels on hide", bounded_polling_retry_and_visibility);
    test_run("PR review destruction cancels held reads", cancel_on_destroy);
}
