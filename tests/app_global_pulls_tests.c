#include "screen_fixture.h"
#include "screen_global_pulls_internal.h"
#include "suites.h"

typedef struct {
    ScreenFixture screen; wchar_t cache_directory[MAX_PATH];
    const char *projects, *boards[2];
    int statuses[2]; volatile LONG calls[3], active, peak;
    char *urls[3]; bool block_board, reject_fresh;
} GlobalFixture;
static Route GLOBAL_ROUTES[] = {{"GET", "/projects", "read"}, {"GET", "/pulls", "read"}};
static bool global_transport(void *ctx, const char *method, const char *url, const char *const *headers,
    const void *body, size_t bytes, int timeout, int *status, char **type, char **retry, char **response, size_t *len, char **error) {
    (void)headers; (void)body; (void)bytes; (void)timeout; (void)error;
    GlobalFixture *f = ctx; CHECK_STR(method, "GET");
    int channel = strstr(url, "/projects") ? 0 : strstr(url, "repo=o%2Fa") ? 1 : 2;
    LONG active = InterlockedIncrement(&f->active); if (active > f->peak) f->peak = active;
    free(f->urls[channel]); f->urls[channel] = xstrdup(url); InterlockedIncrement(&f->calls[channel]);
    if (channel && f->block_board) { SetEvent(f->screen.entered); WaitForSingleObject(f->screen.resume, 10000); }
    *status = channel ? f->statuses[channel - 1] : f->screen.read_status;
    if (channel && f->reject_fresh && strstr(url, "fresh=1")) *status = 400;
    *type = xstrdup("application/json"); *retry = xstrdup(*status == 429 ? "120" : NULL);
    *response = xstrdup(channel ? f->boards[channel - 1] : f->projects); *len = strlen(*response);
    InterlockedDecrement(&f->active); return true;
}
static GlobalPullsScreen *global_setup(GlobalFixture *f) {
    memset(f, 0, sizeof *f); screen_fixture_init(&f->screen, GLOBAL_ROUTES, 2); g_store.device.permission = "read";
    // Shared repository cooldowns are account scoped, including across test fixtures.
    static char ids[64][32]; static size_t id;
    snprintf(ids[id], sizeof ids[id], "global-test-%zu", id); g_store.device.id = ids[id++];
    GetTempPathW(MAX_PATH, f->cache_directory);
    wchar_t suffix[64]; swprintf(suffix, 64, L"briareus-global-tests-%zu", id); wcscat(f->cache_directory, suffix);
    g_store.cache = cache_new(f->cache_directory); cache_remove_all(g_store.cache);
    f->projects = "{\"projects\":[{\"repo\":\"o/a\",\"label\":\"Alpha\"},{\"repo\":\"o/b\",\"label\":\"Beta\"}]}";
    f->boards[0] = "{\"repo\":\"o/a\",\"pulls\":[{\"number\":7,\"title\":\"Alpha change\",\"author\":\"alice\",\"checks\":\"success\",\"recommended\":\"review\"}]}";
    f->boards[1] = "{\"repo\":\"o/b\",\"pulls\":[{\"number\":7,\"title\":\"Beta change\",\"author\":\"bob\",\"checks\":\"failure\"}]}";
    f->statuses[0] = f->statuses[1] = 200;
    api_client_set_transport(f->screen.client, global_transport, f);
    GlobalPullsScreen *s = (GlobalPullsScreen *)global_pulls_screen_new(); screen_fixture_pane(&f->screen, &s->base); return s;
}
static void global_fixture_clear(GlobalFixture *f) {
    for (int i = 0; i < 3; i++) free(f->urls[i]);
    DiskCache *cache = g_store.cache;
    screen_fixture_clear(&f->screen); cache_remove_all(cache); cache_free(cache); RemoveDirectoryW(f->cache_directory);
}
static void global_cleanup(GlobalFixture *f, GlobalPullsScreen *s) { s->base.vt->destroy(&s->base); global_fixture_clear(f); }
static void global_load(GlobalFixture *f, GlobalPullsScreen *s) { s->base.vt->visible(&s->base, true); screen_fixture_pump(&f->screen, 3); }
static void global_navigation(void) {
    GlobalFixture f; GlobalPullsScreen *s = global_setup(&f); global_load(&f, s);
    CHECK_INT(f.peak, 1); CHECK(!s->loading); CHECK_INT(s->state.count, 2);
    Doc doc; doc_init(&doc); screen_fixture_draw(&s->base, &doc); CHECK_INT(s->row_count, 2);
    for (int i = 0; i < 2; i++) {
        Screen *detail = global_pulls_detail(s, s->action_first + i); CHECK(detail != NULL); if (!detail) continue;
        PullScreen *pull = (PullScreen *)detail; CHECK_INT(pull->number, 7);
        CHECK_STR(pull->project.repo, i ? "o/b" : "o/a"); CHECK_STR(pull->summary.title, i ? "Beta change" : "Alpha change"); detail->vt->destroy(detail);
    }
    intptr_t old = s->action_first;
    s->base.vt->action(&s->base, GLOBAL_CHECKS, 0, (POINT){0}); CHECK(global_pulls_detail(s, old) == NULL);
    screen_fixture_draw(&s->base, &doc); CHECK_INT(s->row_count, 1);
    s->base.vt->action(&s->base, GLOBAL_CLEAR, 0, (POINT){0}); screen_fixture_draw(&s->base, &doc); CHECK_INT(s->row_count, 2);
    s->base.vt->activated(&s->base, true); s->base.vt->timer(&s->base, GLOBAL_POLL);
    CHECK_INT(f.calls[0], 1); CHECK_INT(f.calls[1], 1); CHECK_INT(f.calls[2], 1);
    doc_free(&doc); global_cleanup(&f, s);
}
static void global_partial_and_scope(void) {
    GlobalFixture f; GlobalPullsScreen *s = global_setup(&f); f.statuses[1] = 403; global_load(&f, s);
    CHECK(s->state.projects[0].loaded); CHECK(!s->state.projects[1].loaded); CHECK(s->state.projects[1].error != NULL);
    Doc doc; doc_init(&doc); screen_fixture_draw(&s->base, &doc); CHECK_INT(s->row_count, 1);
    f.projects = "{\"projects\":[{\"repo\":\"o/b\",\"label\":\"Renamed Beta\"}]}";
    s->base.vt->refresh(&s->base); screen_fixture_pump(&f.screen, 1); // B remains on cooldown, A is removed immediately.
    CHECK_INT(s->state.count, 1); CHECK_STR(s->state.projects[0].project.repo, "o/b");
    screen_fixture_draw(&s->base, &doc); CHECK_INT(s->row_count, 0); CHECK_INT(f.calls[1], 1); CHECK_INT(f.calls[2], 1);
    doc_free(&doc); global_cleanup(&f, s);
}
static void global_cancel_and_stale(void) {
    GlobalFixture f; GlobalPullsScreen *s = global_setup(&f); f.block_board = true;
    s->base.vt->visible(&s->base, true); screen_fixture_pump(&f.screen, 1);
    CHECK_INT(WaitForSingleObject(f.screen.entered, 10000), WAIT_OBJECT_0);
    Request *pending = s->req; s->base.vt->refresh(&s->base); s->base.vt->refresh(&s->base); CHECK(s->req == pending); CHECK(s->queued_fresh);
    s->base.vt->visible(&s->base, false); CHECK(s->req == NULL); SetEvent(f.screen.resume); screen_fixture_pump(&f.screen, 1);
    CHECK(!s->state.projects[0].loaded); f.block_board = false;
    s->base.vt->visible(&s->base, true); screen_fixture_pump(&f.screen, 5); // Resume two boards, then one coalesced fresh round.
    CHECK(s->state.projects[0].loaded && s->state.projects[1].loaded); CHECK(!s->queued_fresh); CHECK_INT(f.peak, 1);
    CHECK(strstr(f.urls[1], "fresh=1") != NULL);
    f.block_board = true; ResetEvent(f.screen.resume); ResetEvent(f.screen.entered);
    s->base.vt->refresh(&s->base); screen_fixture_pump(&f.screen, 1);
    CHECK_INT(WaitForSingleObject(f.screen.entered, 10000), WAIT_OBJECT_0);
    g_store.device.permission = "admin"; SetEvent(f.screen.resume); screen_fixture_pump(&f.screen, 1);
    CHECK_INT(s->state.count, 0); CHECK(!s->loading); global_cleanup(&f, s);
}
static void global_cooldown_and_projects_retry(void) {
    GlobalFixture f; GlobalPullsScreen *s = global_setup(&f); f.statuses[0] = 429; global_load(&f, s);
    CHECK(pulls_retry_deadline("o/a") >= time(NULL) + 119); CHECK(s->state.projects[1].loaded);
    s->base.vt->refresh(&s->base); screen_fixture_pump(&f.screen, 2);
    CHECK_INT(f.calls[1], 1); CHECK_INT(f.calls[2], 2); CHECK(s->state.projects[0].fresh);
    f.screen.read_status = 429; s->base.vt->refresh(&s->base); screen_fixture_pump(&f.screen, 1);
    CHECK(s->projects_retry > time(NULL)); CHECK(s->error != NULL);
    s->base.vt->refresh(&s->base); CHECK(s->req == NULL); CHECK_INT(f.calls[0], 3);
    global_cleanup(&f, s);
}
static void global_project_paging(void) {
    GlobalFixture f; GlobalPullsScreen *s = global_setup(&f);
    Json *projects = json_array();
    for (int i = 0; i < 27; i++) { Json *p = json_object(); char *repo = xstrfmt("o/p%d", i); json_set_str(p, "repo", repo); free(repo); json_array_push(projects, p); }
    char *text = json_serialize(projects, false); f.projects = text; json_free(projects);
    f.boards[1] = "{\"pulls\":[]}";
    s->base.vt->visible(&s->base, true); screen_fixture_pump(&f.screen, 26);
    CHECK_INT(f.calls[2], 25); CHECK_INT(s->state.count, 27); CHECK(!s->state.projects[25].loaded);
    s->base.vt->action(&s->base, GLOBAL_MORE, 0, (POINT){0}); screen_fixture_pump(&f.screen, 2);
    CHECK_INT(f.calls[2], 27); CHECK(s->state.projects[26].loaded); CHECK_INT(f.peak, 1);
    free(text); global_cleanup(&f, s);
}
static void global_back_retains_context(void) {
    GlobalFixture f; GlobalPullsScreen *s = global_setup(&f); global_load(&f, s);
    Json *board = json_object(), *pulls = json_array(); json_object_set(board, "pulls", pulls);
    for (int i = 1; i <= 110; i++) { Json *p = json_object(); json_set_num(p, "number", i); json_set_str(p, "title", "Synthetic scroll row"); json_set_str(p, "author", "alice"); json_array_push(pulls, p); }
    CHECK(global_pulls_adopt(&s->state.projects[0], board)); json_free(board);
    board_filter_set(&s->filter, FILTER_AUTHOR, "alice"); s->visible_rows = 100;
    pane_set_root(f.screen.pane, &s->base);
    RedrawWindow(pane_hwnd(f.screen.pane), NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW);
    pane_scroll_to(f.screen.pane, 508); int scroll = pane_scroll_y(f.screen.pane); CHECK(scroll > 0);
    Screen *detail = global_pulls_detail(s, s->action_first + 3); CHECK(detail != NULL);
    if (detail) {
        int selected = s->selected_number; pane_push(f.screen.pane, detail); CHECK(!s->shown);
        pane_pop(f.screen.pane); RedrawWindow(pane_hwnd(f.screen.pane), NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW);
        CHECK(pane_top(f.screen.pane) == &s->base); CHECK_INT(pane_scroll_y(f.screen.pane), scroll);
        CHECK_STR(board_filter_get(&s->filter, FILTER_AUTHOR), "alice"); CHECK_INT(s->selected_number, selected); CHECK_STR(s->selected_repo, "o/a");
    }
    CHECK(pane_take_root(f.screen.pane) == &s->base); s->base.pane = f.screen.pane;
    global_cleanup(&f, s);
}
static void global_failures_and_compatibility(void) {
    GlobalFixture f; GlobalPullsScreen *s = global_setup(&f); global_load(&f, s);
    f.statuses[0] = 502; f.statuses[1] = 404;
    s->base.vt->refresh(&s->base); screen_fixture_pump(&f.screen, 3);
    CHECK_INT(s->state.projects[0].count, 1); CHECK(s->state.projects[0].error != NULL);
    CHECK_INT(s->state.projects[1].count, 0); CHECK(!s->state.projects[1].loaded);
    Json *cached = cache_value(g_store.cache, "pulls:o/b"); CHECK(cached == NULL); json_free(cached);
    f.projects = "{}"; s->base.vt->refresh(&s->base); screen_fixture_pump(&f.screen, 1);
    CHECK(s->error != NULL); CHECK_INT(s->state.projects[0].count, 1);
    global_cleanup(&f, s);
    s = global_setup(&f); f.reject_fresh = true;
    s->base.vt->visible(&s->base, true); s->base.vt->refresh(&s->base); screen_fixture_pump(&f.screen, 8);
    CHECK(s->state.projects[0].loaded && s->state.projects[1].loaded); CHECK(!s->loading);
    CHECK_INT(f.calls[1], 3); CHECK_INT(f.calls[2], 3); CHECK(strstr(f.urls[1], "fresh=") == NULL);
    global_cleanup(&f, s);
}
static void global_cancel_projects_and_client(void) {
    GlobalFixture f; GlobalPullsScreen *s = global_setup(&f);
    s->base.vt->visible(&s->base, true); s->base.vt->visible(&s->base, false);
    screen_fixture_pump(&f.screen, 1); CHECK(!s->listed);
    s->base.vt->visible(&s->base, true); screen_fixture_pump(&f.screen, 3); CHECK(s->listed);
    f.block_board = true; s->base.vt->refresh(&s->base); screen_fixture_pump(&f.screen, 1);
    CHECK_INT(WaitForSingleObject(f.screen.entered, 10000), WAIT_OBJECT_0);
    ApiClient *client = g_store.client; g_store.client = NULL; SetEvent(f.screen.resume); screen_fixture_pump(&f.screen, 1);
    CHECK_INT(s->state.count, 0); CHECK(!s->listed); g_store.client = client;
    global_cleanup(&f, s);
}
static void global_destroy_pending(void) {
    GlobalFixture f; GlobalPullsScreen *s = global_setup(&f);
    s->base.vt->visible(&s->base, true); s->base.vt->destroy(&s->base);
    screen_fixture_pump(&f.screen, 1); CHECK_INT(f.calls[1], 0); global_fixture_clear(&f);
    s = global_setup(&f); f.block_board = true;
    s->base.vt->visible(&s->base, true); screen_fixture_pump(&f.screen, 1);
    CHECK_INT(WaitForSingleObject(f.screen.entered, 10000), WAIT_OBJECT_0);
    s->base.vt->destroy(&s->base); SetEvent(f.screen.resume); screen_fixture_pump(&f.screen, 1);
    CHECK_INT(f.calls[2], 0); global_fixture_clear(&f);
}
static WNDPROC menu_original;
static Pane *menu_pane;
static bool menu_retired;
static LRESULT CALLBACK retire_menu_owner(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
    if (message == WM_ENTERMENULOOP) PostMessageW(hwnd, WM_APP + 90, 0, 0);
    if (message == WM_APP + 90) {
        pane_set_root(menu_pane, NULL); menu_retired = true; EndMenu(); return 0;
    }
    return CallWindowProcW(menu_original, hwnd, message, wp, lp);
}
static void global_popup_owner_lifetime(void) {
    GlobalFixture f; GlobalPullsScreen *s = global_setup(&f); global_load(&f, s);
    pane_set_root(f.screen.pane, &s->base); menu_pane = f.screen.pane; menu_retired = false;
    HWND hwnd = pane_hwnd(menu_pane);
    menu_original = (WNDPROC)SetWindowLongPtrW(hwnd, GWLP_WNDPROC, (LONG_PTR)retire_menu_owner);
    global_pulls_pick(s, FILTER_AUTHOR, (POINT){200, 100});
    CHECK(menu_retired); CHECK(pane_top(menu_pane) == NULL);
    SetWindowLongPtrW(hwnd, GWLP_WNDPROC, (LONG_PTR)menu_original);
    Json *saved = cache_value(g_store.cache, "global-pulls-filters"); CHECK(saved == NULL); json_free(saved);
    pane_set_root(menu_pane, NULL); menu_pane = NULL; global_fixture_clear(&f);
}
void app_global_pulls_tests(void) {
    screen_fixture_prepare();
    test_run("global PR filter popup survives owner retirement", global_popup_owner_lifetime);
    test_run("global PR destruction cancels both project and board owners", global_destroy_pending);
    test_run("global PR failed snapshots and fresh compatibility", global_failures_and_compatibility);
    test_run("global PR cancelled project list and stale client", global_cancel_projects_and_client);
    test_run("global PR Back retains filter selection and scroll", global_back_retains_context);
    test_run("global PR native list identity filters and navigation", global_navigation);
    test_run("global PR partial permissions and removed project scope", global_partial_and_scope);
    test_run("global PR cancellation fresh coalescing and stale device", global_cancel_and_stale);
    test_run("global PR independent cooldowns and project retry", global_cooldown_and_projects_retry);
    test_run("global PR bounded sequential project pages", global_project_paging);
}
