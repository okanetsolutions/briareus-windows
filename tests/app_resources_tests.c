#include "screen_fixture.h"
#include "screen_resources_internal.h"
#include "screen_conversation_internal.h"
#include "suites.h"
#include "fixtures/stats/session.h"

enum { STEPS = 12 };
typedef struct { HANDLE entered, release; const char *answer, *retry, *url; int status; } Step;
typedef struct { ScreenFixture screen; Step steps[STEPS]; volatile LONG calls; } Fixture;
static Route ROUTES[] = {{"GET", "/sessions/{sessionId}/stats", "read"}};

static bool transport(void *ctx, const char *method, const char *url, const char *const *headers,
    const void *body, size_t bytes, int timeout, int *status, char **type, char **retry, char **response, size_t *len, char **error) {
    (void)headers; (void)body; (void)bytes; (void)timeout; (void)error;
    Fixture *f = ctx; LONG index = InterlockedIncrement(&f->calls) - 1;
    CHECK(index < STEPS); CHECK_STR(method, "GET");
    if (index >= STEPS) return false;
    Step *s = &f->steps[index];
    CHECK_STR(url, s->url ? s->url : "https://example.com/api/v1/sessions/s1/stats");
    // Each read has its own completion gate: later reads never consume an old completion.
    SetEvent(s->entered); CHECK_INT(WaitForSingleObject(s->release, 10000), WAIT_OBJECT_0);
    *status = s->status; *type = xstrdup("application/json"); *retry = xstrdup(s->retry);
    *response = xstrdup(s->answer); *len = strlen(*response); return true;
}
static void init(Fixture *f) {
    memset(f, 0, sizeof *f); screen_fixture_init(&f->screen, ROUTES, 1);
    g_store.device.permission = "read";
    for (int i = 0; i < STEPS; i++) {
        f->steps[i] = (Step){CreateEventW(NULL, TRUE, FALSE, NULL), CreateEventW(NULL, TRUE, FALSE, NULL), SESSION_STATS_FIXTURE, NULL, NULL, 200};
    }
    api_client_set_transport(f->screen.client, transport, f);
}
static void entered(Fixture *f, int index) { CHECK_INT(WaitForSingleObject(f->steps[index].entered, 10000), WAIT_OBJECT_0); }
static void complete(Fixture *f, int index) {
    entered(f, index); SetEvent(f->steps[index].release); screen_fixture_pump(&f->screen, 1);
}
static Screen *create(Fixture *f, const char *id) {
    Session session = {0}; session.raw = json_object(); json_set_str(session.raw, "id", id); json_set_str(session.raw, "repo", "one/repo");
    json_set_str(session.raw, "status", "idle"); json_set_str(session.raw, "title", "Named session");
    Screen *s = resources_screen_new(&session); session_free(&session);
    if (!f->screen.pane) screen_fixture_pane(&f->screen, s); else s->pane = f->screen.pane;
    return s;
}
static void clear(Fixture *f) {
    json_free(g_store.resources_retries); g_store.resources_retries = NULL;
    for (int i = 0; i < STEPS; i++) { CloseHandle(f->steps[i].entered); CloseHandle(f->steps[i].release); }
    screen_fixture_clear(&f->screen);
}
static void expire(ResourcesScreen *s) {
    Json *entry = (Json *)json_get(g_store.resources_retries, s->retry_key);
    json_set_num(entry, "until", (double)time(NULL) - 1);
}
static void gating_and_navigation(void) {
    Fixture f; init(&f);
    Session session = {0}; session.raw = json_parsez("{\"id\":\"s1\",\"repo\":\"one/repo\",\"status\":\"idle\"}");
    Screen *conversation = conversation_screen_new(&session); session_free(&session);
    Pane *window = detached_open(conversation, NULL, false); CHECK(window != NULL);
    HeaderInfo header = {0}; conversation->vt->header(conversation, &header);
    int resource = 0;
    for (int i = 0; i < header.button_count; i++) if (header.buttons[i].action == ACT_MENU_ITEM + MENU_RESOURCES) resource = header.buttons[i].action;
    CHECK(resource != 0);
    // A stale action is gated again after the catalog or token changes.
    g_store.route_count = 0; conversation->vt->action(conversation, resource, 0, (POINT){0}); CHECK(pane_top(window) == conversation);
    header = (HeaderInfo){0}; conversation->vt->header(conversation, &header);
    for (int i = 0; i < header.button_count; i++) CHECK_INT(header.buttons[i].action == resource, 0);
    g_store.route_count = 1; g_store.device.permission = "unknown";
    conversation->vt->action(conversation, resource, 0, (POINT){0}); CHECK(pane_top(window) == conversation);
    g_store.device.permission = "read";
    conversation->vt->action(conversation, resource, 0, (POINT){0});
    Screen *s = pane_top(window); CHECK(s != conversation); complete(&f, 0);
    CHECK_STR(session_id(&((ResourcesScreen *)s)->session), "s1"); CHECK(((ResourcesScreen *)s)->loaded);
    CHECK(!((ConversationScreen *)conversation)->poller.running);
    pane_pop(window); CHECK(pane_top(window) == conversation);
    CHECK_INT(f.calls, 1);
    DestroyWindow(GetAncestor(pane_hwnd(window), GA_ROOT)); clear(&f);
}
static void polling_recovery_and_empty(void) {
    Fixture f; init(&f); Screen *s = create(&f, "s1"); ResourcesScreen *r = (ResourcesScreen *)s;
    CHECK_INT(f.calls, 0); s->vt->refresh(s); CHECK(r->read == NULL);
    s->vt->visible(s, true); entered(&f, 0);
    Request *first = r->read; s->vt->refresh(s); s->vt->timer(s, RESOURCES_POLL);
    CHECK(r->read == first); CHECK_INT(f.calls, 1); complete(&f, 0);
    CHECK(r->loaded && r->fresh); CHECK_INT(r->poller.base_ms, 5000);
    Doc doc; doc_init(&doc); screen_fixture_draw(s, &doc);
    f.steps[1].status = 503; f.steps[1].answer = "{\"error\":\"sample temporarily unavailable\"}";
    s->vt->timer(s, RESOURCES_POLL); complete(&f, 1);
    CHECK(r->loaded && !r->fresh); CHECK(r->error != NULL); CHECK_INT(r->reading.total.processes.value, 5);
    s->vt->refresh(s); CHECK(r->read == NULL); expire(r);
    f.steps[2].answer = "{\"host\":{\"intervalMs\":1000},\"stats\":{\"sessionId\":\"s1\",\"cpuPercent\":0},\"history\":[]}";
    s->vt->refresh(s); complete(&f, 2); CHECK(r->fresh); CHECK(r->error == NULL);
    CHECK(r->reading.total.cpu.present); CHECK(!r->reading.total.memory.present); CHECK_INT(r->poller.base_ms, 2000);
    screen_fixture_draw(s, &doc);
    f.steps[3].answer = "{\"host\":{},\"stats\":{\"sessionId\":\"wrong\"},\"history\":[]}";
    s->vt->refresh(s); complete(&f, 3); CHECK(!r->fresh); CHECK(r->reading.total.cpu.present); expire(r);
    f.steps[4].answer = "{\"host\":{},\"stats\":null,\"history\":[]}";
    s->vt->refresh(s); complete(&f, 4); CHECK(r->fresh); CHECK(!r->reading.open); CHECK_INT(r->reading.history_count, 0);
    screen_fixture_draw(s, &doc);
    // A not-open response keeps following: an idle/stopped session can later sample or reopen.
    s->vt->timer(s, RESOURCES_POLL); complete(&f, 5); CHECK(r->reading.open);
    f.steps[6].status = 403;
    s->vt->refresh(s); complete(&f, 6); CHECK(!r->loaded); CHECK(r->reading.history == NULL); CHECK(!r->reading.open); expire(r);
    s->vt->refresh(s); complete(&f, 7); CHECK(r->loaded);
    // Session visibility refusals use 404, including access revoked on the same token.
    f.steps[8].status = 404;
    s->vt->refresh(s); complete(&f, 8); CHECK(!r->loaded); CHECK(!r->reading.total.memory.present); CHECK(r->reading.history == NULL);
    s->vt->visible(s, false); CHECK(!r->poller.running); s->vt->refresh(s); CHECK(r->read == NULL);
    doc_free(&doc); s->vt->destroy(s); clear(&f);
}
static void retry_across_reopen(void) {
    Fixture f; init(&f); f.steps[0].status = 429; f.steps[0].retry = "3600"; f.steps[0].answer = "{\"error\":\"slow down\"}";
    Screen *s = create(&f, "s1"); s->vt->visible(s, true); complete(&f, 0);
    ResourcesScreen *r = (ResourcesScreen *)s; CHECK(!r->loaded); CHECK(r->error != NULL);
    double until = json_num_or(json_get(json_get(g_store.resources_retries, r->retry_key), "until"), 0);
    CHECK(until >= (double)time(NULL) + 3598);
    s->vt->refresh(s); s->vt->timer(s, RESOURCES_POLL); CHECK(r->read == NULL);
    s->vt->visible(s, false); s->vt->visible(s, true); CHECK(r->read == NULL);
    s->vt->destroy(s); s = create(&f, "s1"); r = (ResourcesScreen *)s;
    s->vt->visible(s, true); s->vt->refresh(s); CHECK(r->read == NULL); CHECK_INT(f.calls, 1);
    expire(r); s->vt->timer(s, RESOURCES_POLL); complete(&f, 1); CHECK(r->loaded && r->fresh);
    // A huge deadline stays intact while each timer wait remains representable.
    f.steps[2].status = 503; f.steps[2].retry = "3000000"; s->vt->refresh(s); complete(&f, 2);
    CHECK(r->poller.base_ms > 2000000000); s->vt->refresh(s); CHECK(r->read == NULL);
    s->vt->destroy(s); clear(&f);
}
static void cancellation_and_isolation(void) {
    Fixture f; init(&f); Screen *a = create(&f, "s1"); ResourcesScreen *r = (ResourcesScreen *)a;
    a->vt->visible(a, true); entered(&f, 0); a->vt->visible(a, false); CHECK(r->read == NULL);
    // Cancelled read is delivered before reopening starts its separately gated replacement.
    complete(&f, 0); CHECK(!r->loaded); a->vt->visible(a, true); complete(&f, 1); CHECK(r->loaded);
    g_store.active = false; a->vt->refresh(a); a->vt->timer(a, RESOURCES_POLL); CHECK(r->read == NULL);
    g_store.active = true; a->vt->activated(a, true); entered(&f, 2);
    ApiClient *original = g_store.client;
    ServerAddress address; ApiError error; api_error_init(&error); CHECK(server_address_parse("https://another.example", &address));
    g_store.client = api_client_new(&address, "brm_bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb", &error);
    server_address_free(&address); api_error_clear(&error);
    complete(&f, 2); CHECK(!resources_current(r));
    Doc doc; doc_init(&doc); screen_fixture_draw(a, &doc);
    a->vt->timer(a, RESOURCES_POLL); CHECK(!r->poller.running);
    a->vt->destroy(a); api_client_release(g_store.client); g_store.client = original;
    a = create(&f, "s1"); r = (ResourcesScreen *)a; a->vt->visible(a, true); entered(&f, 3);
    g_store.device.permission = "manage"; complete(&f, 3); CHECK(!r->loaded); CHECK(!resources_current(r));
    a->vt->destroy(a); g_store.device.permission = "read";
    a = create(&f, "s1"); a->vt->visible(a, true); entered(&f, 4);
    a->vt->destroy(a); complete(&f, 4); // teardown drops completion without touching the freed owner
    a = create(&f, "s1"); r = (ResourcesScreen *)a;
    Screen *b = create(&f, "other/session"); ResourcesScreen *other = (ResourcesScreen *)b;
    CHECK(!str_eq(a->id, b->id)); CHECK(!str_eq(r->retry_key, other->retry_key));
    f.steps[5].status = 429; f.steps[5].retry = "120";
    a->vt->visible(a, true); complete(&f, 5);
    f.steps[6].url = "https://example.com/api/v1/sessions/other%2Fsession/stats";
    f.steps[6].answer = "{\"host\":{},\"stats\":{\"sessionId\":\"other/session\",\"memoryBytes\":77},\"history\":[]}";
    b->vt->visible(b, true); complete(&f, 6); CHECK(other->fresh); CHECK_INT(other->reading.total.memory.value, 77);
    a->vt->refresh(a); CHECK(r->read == NULL); CHECK(!r->loaded);
    a->vt->destroy(a); b->vt->destroy(b); doc_free(&doc); clear(&f);
}
void app_resources_tests(void) {
    screen_fixture_prepare();
    Session session = {0}; session.raw = json_parsez("{\"id\":\"warm\"}");
    Pane *warm = detached_open(resources_screen_new(&session), NULL, false);
    if (warm) DestroyWindow(GetAncestor(pane_hwnd(warm), GA_ROOT));
    session_free(&session);
    test_run("Resources header action honors capability and read-token navigation", gating_and_navigation);
    test_run("Resources polling coalesces reads, retains stale data and recovers partial/closed sessions", polling_recovery_and_empty);
    test_run("Resources Retry-After survives refresh hide and recreation", retry_across_reopen);
    test_run("Resources cancel stale owners and isolate server permission and session snapshots", cancellation_and_isolation);
}
