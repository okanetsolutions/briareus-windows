#include "screen_fixture.h"
#include "screen_ci_internal.h"
#include "suites.h"
#include "fixtures/ci.h"

typedef struct { ScreenFixture screen; char *queries[2]; volatile LONG calls[2]; int statuses[2]; const char *answers[2]; } Fixture;
static Route ROUTES[] = {{"GET", "/stats/ci", "read"}, {"GET", "/ci/runs", "read"}};
static bool transport(void *ctx, const char *method, const char *url, const char *const *headers,
    const void *body, size_t bytes, int timeout, int *status, char **type, char **retry, char **response, size_t *len, char **error) {
    (void)headers; (void)body; (void)bytes; (void)timeout; (void)error;
    Fixture *f = ctx; CHECK_STR(method, "GET"); int ch = strstr(url, "/ci/runs?") != NULL;
    free(f->queries[ch]); f->queries[ch] = xstrdup(url); InterlockedIncrement(&f->calls[ch]);
    if (f->screen.block_read) { SetEvent(f->screen.entered); WaitForSingleObject(f->screen.resume, 10000); }
    *status = f->statuses[ch]; *type = xstrdup("application/json"); *retry = xstrdup(*status == 429 ? "120" : NULL);
    *response = xstrdup(f->answers[ch]); *len = strlen(*response); return true;
}
static CIScreen *setup(Fixture *f) {
    memset(f, 0, sizeof *f); screen_fixture_init(&f->screen, ROUTES, 2); g_store.device.permission = "read";
    f->statuses[0] = f->statuses[1] = 200; f->answers[0] = CI_STATS_FIXTURE; f->answers[1] = CI_RUNS_FIXTURE;
    api_client_set_transport(f->screen.client, transport, f);
    CIScreen *s = (CIScreen *)ci_screen_new(); screen_fixture_pane(&f->screen, &s->base); return s;
}
static void cleanup(Fixture *f, CIScreen *s) {
    s->base.vt->destroy(&s->base); free(f->queries[0]); free(f->queries[1]); screen_fixture_clear(&f->screen);
}
static void load(Fixture *f, CIScreen *s) { s->base.vt->visible(&s->base, true); screen_fixture_pump(&f->screen, 2); }
static void history_and_paging(void) {
    Fixture f; CIScreen *s = setup(&f); s->repo = xstrdup("acme/shop"); s->branch = xstrdup("feature/a b"); load(&f, s);
    CHECK(s->stats != NULL); CHECK_INT(json_count(s->runs), 1); CHECK_INT(s->offset, 50);
    CHECK(strstr(f.queries[0], "/stats/ci?") != NULL); CHECK(strstr(f.queries[1], "/ci/runs?") != NULL);
    CHECK(strstr(f.queries[0], "repo=acme%2Fshop") != NULL); CHECK(strstr(f.queries[0], "branch=feature%2Fa%20b") != NULL);
    CHECK(strstr(f.queries[1], "kind=check_run") != NULL); CHECK(strstr(f.queries[1], "limit=50") != NULL);
    Json *window = json_clone(s->window);
    f.answers[1] = "{\"runs\":[],\"nextOffset\":null,\"truncated\":true}";
    s->base.vt->action(&s->base, CI_MORE, 0, (POINT){0}); screen_fixture_pump(&f.screen, 1);
    CHECK(json_equal(window, s->window)); CHECK(strstr(f.queries[1], "offset=50") != NULL); CHECK_INT(s->offset, -1); CHECK(s->truncated);
    CHECK_INT(json_count(s->runs), 1); CHECK_INT(f.calls[0], 1);
    s->base.vt->timer(&s->base, CI_POLL); screen_fixture_pump(&f.screen, 1);
    CHECK(json_equal(window, s->window)); CHECK_INT(json_count(s->runs), 1); CHECK_INT(f.calls[1], 2);
    s->base.vt->activated(&s->base, true); CHECK(s->reads[0] == NULL && s->reads[1] == NULL); json_free(window);
    Doc doc; doc_init(&doc); screen_fixture_draw(&s->base, &doc); intptr_t old = s->action_first;
    s->base.vt->action(&s->base, CI_DAYS, 7, (POINT){0}); screen_fixture_pump(&f.screen, 2);
    CHECK_INT(s->days, 7); CHECK(old < s->action_first); CHECK_INT(json_count(s->runs), 0);
    s->base.vt->action(&s->base, CI_KIND, 1, (POINT){0}); screen_fixture_pump(&f.screen, 2);
    CHECK(strstr(f.queries[0], "kind=status") != NULL);
    doc_free(&doc); cleanup(&f, s);
}
static void failures_and_capabilities(void) {
    Fixture f; CIScreen *s = setup(&f); f.statuses[1] = 429; load(&f, s);
    CHECK(s->stats != NULL && s->runs == NULL); CHECK(s->retry[1] >= (double)time(NULL) + 119); CHECK(s->errors[1] != NULL);
    s->base.vt->refresh(&s->base); CHECK_INT(f.calls[0], 1); CHECK_INT(f.calls[1], 1);
    s->base.vt->action(&s->base, CI_DAYS, 90, (POINT){0}); CHECK(s->stats == NULL); CHECK(s->reads[0] == NULL); CHECK(s->window == NULL);
    s->base.vt->timer(&s->base, CI_POLL); CHECK_INT(f.calls[0], 1); CHECK(s->reads[0] == NULL);
    s->retry[1] = 0; f.statuses[1] = 200; f.answers[0] = "{}";
    s->base.vt->refresh(&s->base); screen_fixture_pump(&f.screen, 2); CHECK(s->stats == NULL); CHECK(s->errors[0] != NULL); CHECK(s->runs != NULL);
    CHECK(json_num_or(json_get(s->window, "until"), 0) - json_num_or(json_get(s->window, "since"), 0) == 7776000000.0);
    s->retry[0] = 0; f.answers[0] = CI_STATS_FIXTURE; f.statuses[1] = 403;
    s->base.vt->refresh(&s->base); screen_fixture_pump(&f.screen, 2); CHECK(s->stats != NULL); CHECK(s->runs == NULL);
    g_store.route_count = 1; s->retry[1] = 0; s->base.vt->refresh(&s->base); screen_fixture_pump(&f.screen, 1);
    CHECK_INT(f.calls[1], 3);
    Doc doc; doc_init(&doc); screen_fixture_draw(&s->base, &doc); doc_free(&doc);
    g_store.device.permission = "unknown"; s->base.vt->timer(&s->base, CI_POLL); CHECK(s->stats == NULL); CHECK(!s->poller.running);
    cleanup(&f, s);
}
static void cancellation_and_identity(void) {
    Fixture f; CIScreen *s = setup(&f); f.screen.block_read = true; s->base.vt->visible(&s->base, true);
    CHECK_INT(WaitForSingleObject(f.screen.entered, 10000), WAIT_OBJECT_0);
    Request *one = s->reads[0], *two = s->reads[1]; s->base.vt->refresh(&s->base); CHECK(one == s->reads[0] && two == s->reads[1]);
    g_store.device.permission = "admin"; SetEvent(f.screen.resume); screen_fixture_pump(&f.screen, 2); CHECK(s->stats == NULL && s->runs == NULL);
    g_store.device.permission = "read"; ResetEvent(f.screen.resume); ResetEvent(f.screen.entered);
    s->base.vt->refresh(&s->base); CHECK_INT(WaitForSingleObject(f.screen.entered, 10000), WAIT_OBJECT_0);
    g_store.client = NULL; SetEvent(f.screen.resume); screen_fixture_pump(&f.screen, 2); CHECK(s->stats == NULL && s->runs == NULL);
    g_store.client = f.screen.client; ResetEvent(f.screen.resume); ResetEvent(f.screen.entered);
    s->base.vt->refresh(&s->base); CHECK_INT(WaitForSingleObject(f.screen.entered, 10000), WAIT_OBJECT_0);
    s->base.vt->visible(&s->base, false); SetEvent(f.screen.resume); screen_fixture_pump(&f.screen, 2); CHECK(s->stats == NULL && s->runs == NULL);
    ResetEvent(f.screen.resume); ResetEvent(f.screen.entered); s->base.vt->visible(&s->base, true);
    CHECK_INT(WaitForSingleObject(f.screen.entered, 10000), WAIT_OBJECT_0); s->base.vt->destroy(&s->base);
    SetEvent(f.screen.resume); screen_fixture_pump(&f.screen, 2); free(f.queries[0]); free(f.queries[1]); screen_fixture_clear(&f.screen);
}
void app_ci_tests(void) {
    screen_fixture_prepare(); test_run("CI read-only history and fixed paging", history_and_paging);
    test_run("CI failure backoff and capabilities", failures_and_capabilities); test_run("CI cancellation and identity", cancellation_and_identity);
}
