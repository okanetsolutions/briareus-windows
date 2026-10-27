#include "screen_fixture.h"
#include "credentials.h"
#include "screen_resources_internal.h"
#include "screen_status_internal.h"
#include "suites.h"
#include "fixtures/stats/server.h"

enum { STEPS = 12 };
typedef struct { HANDLE entered, release; const char *answer, *retry; int status; } Step;
typedef struct { ScreenFixture screen; Step steps[2][STEPS]; volatile LONG calls[2], session_calls; } Fixture;
static Route ROUTES[] = {{"GET", "/stats/server", "read"}, {"GET", "/stats/sessions", "read"}, {"GET", "/sessions/{sessionId}/stats", "read"}};
static bool transport(void *ctx, const char *method, const char *url, const char *const *headers,
    const void *body, size_t bytes, int timeout, int *status, char **type, char **retry, char **response, size_t *len, char **error) {
    (void)headers; (void)body; (void)bytes; (void)timeout; (void)error;
    Fixture *f = ctx; CHECK_STR(method, "GET");
    if (strstr(url, "/sessions/c/stats")) {
        CHECK_STR(url, "https://example.com/api/v1/sessions/c/stats"); InterlockedIncrement(&f->session_calls);
        *status = 200; *type = xstrdup("application/json"); *retry = NULL;
        *response = xstrdup("{\"host\":{},\"stats\":null,\"history\":[]}"); *len = strlen(*response); return true;
    }
    int route = str_eq(url, "https://example.com/api/v1/stats/server") ? 0 : 1;
    CHECK_STR(url, route ? "https://example.com/api/v1/stats/sessions" : "https://example.com/api/v1/stats/server");
    LONG index = InterlockedIncrement(&f->calls[route]) - 1; CHECK(index < STEPS); if (index >= STEPS) return false;
    Step *s = &f->steps[route][index]; SetEvent(s->entered); CHECK_INT(WaitForSingleObject(s->release, 10000), WAIT_OBJECT_0);
    *status = s->status; *type = xstrdup("application/json"); *retry = xstrdup(s->retry);
    *response = xstrdup(s->answer); *len = strlen(*response); return true;
}
static void init(Fixture *f) {
    memset(f, 0, sizeof *f); screen_fixture_init(&f->screen, ROUTES, 3); g_store.device.permission = "read";
    for (int r = 0; r < 2; r++) for (int i = 0; i < STEPS; i++)
        f->steps[r][i] = (Step){CreateEventW(NULL, TRUE, FALSE, NULL), CreateEventW(NULL, TRUE, FALSE, NULL), r ? OPEN_STATS_FIXTURE : SERVER_STATS_FIXTURE, NULL, 200};
    api_client_set_transport(f->screen.client, transport, f);
}
static void entered(Fixture *f, int r, int i) { CHECK_INT(WaitForSingleObject(f->steps[r][i].entered, 10000), WAIT_OBJECT_0); }
static void complete(Fixture *f, int r, int i) {
    entered(f, r, i); SetEvent(f->steps[r][i].release); screen_fixture_pump(&f->screen, 1);
}
static Screen *create(Fixture *f) {
    Screen *s = server_resources_screen_new();
    if (!f->screen.pane) screen_fixture_pane(&f->screen, s); else s->pane = f->screen.pane;
    return s;
}
static void clear(Fixture *f) {
    json_free(g_store.resources_retries); g_store.resources_retries = NULL;
    for (int r = 0; r < 2; r++) for (int i = 0; i < STEPS; i++) { CloseHandle(f->steps[r][i].entered); CloseHandle(f->steps[r][i].release); }
    screen_fixture_clear(&f->screen);
}
static void expire(ResourcesScreen *s) { json_set_num((Json *)json_get(g_store.resources_retries, s->retry_key), "until", (double)time(NULL) - 1); }
static void gating_and_status_entry(void) {
    Fixture f; init(&f); Screen *status = status_screen_new();
    Pane *window = detached_open(status, NULL, false); CHECK(window != NULL);
    HeaderInfo header = {0}; status->vt->header(status, &header);
    CHECK_INT(header.button_count, 1); CHECK_INT(header.buttons[0].action, STATUS_SERVER);
    g_store.route_count = 0; status->vt->action(status, STATUS_SERVER, 0, (POINT){0}); CHECK(pane_top(window) == status);
    header = (HeaderInfo){0}; status->vt->header(status, &header); CHECK_INT(header.button_count, 0);
    g_store.route_count = 3; g_store.device.permission = "unknown";
    status->vt->action(status, STATUS_SERVER, 0, (POINT){0}); CHECK(pane_top(window) == status);
    g_store.device.permission = "read"; status->vt->action(status, STATUS_SERVER, 0, (POINT){0});
    Screen *s = pane_top(window); CHECK(s != status); complete(&f, 0, 0); complete(&f, 1, 0);
    CHECK(((ResourcesScreen *)s)->loaded); CHECK(((ResourcesScreen *)s)->list->loaded);
    pane_pop(window); DestroyWindow(GetAncestor(pane_hwnd(window), GA_ROOT)); clear(&f);
    init(&f); g_store.route_count = 1; s = create(&f); s->vt->visible(s, true); complete(&f, 0, 0);
    CHECK_INT(f.calls[1], 0); CHECK(!((ResourcesScreen *)s)->list->loaded);
    Doc doc; doc_init(&doc); screen_fixture_draw(s, &doc); doc_free(&doc); s->vt->destroy(s); clear(&f);
}
static void independent_failure_recovery_and_polling(void) {
    Fixture f; init(&f); Screen *s = create(&f); ResourcesScreen *r = (ResourcesScreen *)s, *list = r->list;
    s->vt->refresh(s); CHECK_INT(f.calls[0], 0); CHECK_INT(f.calls[1], 0);
    s->vt->visible(s, true); entered(&f, 0, 0); entered(&f, 1, 0);
    Request *one = r->read, *two = list->read;
    s->vt->refresh(s); s->vt->timer(s, RESOURCES_POLL); s->vt->timer(s, RESOURCES_POLL + 1);
    CHECK(r->read == one); CHECK(list->read == two); complete(&f, 0, 0);
    CHECK(r->loaded && r->fresh); CHECK(!list->loaded); complete(&f, 1, 0); CHECK(list->fresh);
    CHECK_INT(r->poller.base_ms, 10000); CHECK_INT(list->poller.base_ms, 10000);
    f.steps[1][1].status = 503; s->vt->refresh(s); complete(&f, 1, 1); complete(&f, 0, 1);
    CHECK(r->fresh); CHECK(list->loaded && !list->fresh); CHECK(list->error != NULL); CHECK_INT(list->sessions.count, 3);
    double retry = resources_retry_until(list); CHECK(retry > (double)time(NULL));
    s->vt->refresh(s); CHECK(list->read == NULL); complete(&f, 0, 2); CHECK(resources_retry_until(list) == retry);
    expire(list); f.steps[0][3].status = 503; s->vt->refresh(s); complete(&f, 0, 3); complete(&f, 1, 2);
    CHECK(!r->fresh && r->loaded); CHECK(list->fresh && list->error == NULL);
    expire(r); f.steps[0][4].answer = "{\"host\":{\"intervalMs\":1000},\"process\":{},\"sessions\":{},\"disks\":[],\"history\":[]}";
    f.steps[1][3].answer = "{\"host\":{\"intervalMs\":500},\"sessions\":[]}";
    s->vt->refresh(s); complete(&f, 0, 4); complete(&f, 1, 3);
    CHECK(r->fresh && list->fresh); CHECK_INT(r->poller.base_ms, 2000); CHECK_INT(list->poller.base_ms, 2000);
    CHECK_INT(r->server.lines[0].count, 0); CHECK_INT(list->sessions.count, 0);
    Doc doc; doc_init(&doc); screen_fixture_draw(s, &doc);
    g_store.active = false; s->vt->refresh(s); s->vt->timer(s, RESOURCES_POLL); CHECK(r->read == NULL); CHECK(list->read == NULL);
    g_store.active = true; s->vt->activated(s, true); complete(&f, 0, 5); complete(&f, 1, 4);
    s->vt->visible(s, false); CHECK(!r->poller.running && !list->poller.running);
    s->vt->refresh(s); CHECK(r->read == NULL && list->read == NULL);
    doc_free(&doc); s->vt->destroy(s); clear(&f);
}
static void navigation_records_and_stale_actions(void) {
    Fixture f; init(&f);
    wchar_t temp[MAX_PATH], directory[MAX_PATH]; CHECK(GetTempPathW(MAX_PATH, temp) != 0); CHECK(GetTempFileNameW(temp, L"srv", 0, directory) != 0);
    DeleteFileW(directory); CHECK(CreateDirectoryW(directory, NULL)); g_store.cache = cache_new(directory);
    Json *projects = json_parsez("[{\"repo\":\"one/repo\",\"name\":\"One\"}]"); CHECK(cache_store(g_store.cache, projects, "projects")); json_free(projects);
    Json *sessions = json_parsez("[{\"id\":\"c\",\"repo\":\"one/repo\",\"title\":\"Saved name\",\"status\":\"idle\"}]");
    CHECK(cache_store(g_store.cache, sessions, "sessions:one/repo")); json_free(sessions);
    Screen *s = create(&f); ResourcesScreen *r = (ResourcesScreen *)s; s->vt->visible(s, true); complete(&f, 0, 0); complete(&f, 1, 0);
    CHECK_STR(session_display_title(&r->list->sessions.items[0].record), "Saved name");
    Doc doc; doc_init(&doc); screen_fixture_draw(s, &doc); intptr_t old = r->action_first;
    s->vt->refresh(s); complete(&f, 0, 1); complete(&f, 1, 1); CHECK_INT(r->action_count, 0);
    s->vt->action(s, RESOURCES_OPEN, old, (POINT){0}); CHECK_INT(f.session_calls, 0);
    // Transfer to a real detached pane: rows navigate through the production stack.
    Pane *window = detached_open(s, NULL, false); CHECK(window != NULL); complete(&f, 0, 2); complete(&f, 1, 2);
    screen_fixture_draw(s, &doc); g_store.route_count = 2;
    s->vt->action(s, RESOURCES_OPEN, r->action_first, (POINT){0}); CHECK(pane_top(window) == s);
    g_store.route_count = 3; screen_fixture_draw(s, &doc);
    s->vt->action(s, RESOURCES_OPEN, r->action_first, (POINT){0});
    Screen *child = pane_top(window); CHECK(child != s); screen_fixture_pump(&f.screen, 1);
    CHECK_STR(session_id(&((ResourcesScreen *)child)->session), "c"); CHECK_STR(session_repo(&((ResourcesScreen *)child)->session), "one/repo");
    CHECK_INT(f.session_calls, 1); CHECK(!r->poller.running && !r->list->poller.running);
    DestroyWindow(GetAncestor(pane_hwnd(window), GA_ROOT)); doc_free(&doc);
    cache_remove_all(g_store.cache); cache_free(g_store.cache); g_store.cache = NULL; clear(&f);
}
static void route_cooldowns_survive_cancel_and_reopen(void) {
    Fixture f; init(&f); Screen *s = create(&f); ResourcesScreen *r = (ResourcesScreen *)s;
    s->vt->visible(s, true); entered(&f, 0, 0); entered(&f, 1, 0);
    f.steps[0][0].status = 429; f.steps[0][0].retry = "3600"; s->vt->destroy(s);
    complete(&f, 0, 0); complete(&f, 1, 0);
    s = create(&f); r = (ResourcesScreen *)s; s->vt->visible(s, true);
    CHECK(r->read == NULL); complete(&f, 1, 1); CHECK(r->list->loaded); CHECK(!r->loaded);
    CHECK(resources_retry_until(r) >= (double)time(NULL) + 3598);
    s->vt->refresh(s); CHECK(r->read == NULL); complete(&f, 1, 2);
    s->vt->visible(s, false); s->vt->visible(s, true); CHECK(r->read == NULL); complete(&f, 1, 3);
    expire(r); s->vt->refresh(s); complete(&f, 0, 1); complete(&f, 1, 4); CHECK(r->fresh);
    s->vt->destroy(s); clear(&f);
}
static void refused_generation_and_concurrent_recovery(void) {
    for (int route = 0; route < 2; route++) {
        Fixture f; init(&f); Screen *a = create(&f), *b = create(&f);
        ResourcesScreen *ra = (ResourcesScreen *)a, *rb = (ResourcesScreen *)b;
        a->vt->visible(a, true); complete(&f, 0, 0); complete(&f, 1, 0);
        b->vt->visible(b, true); complete(&f, 0, 1); complete(&f, 1, 1);
        f.steps[route][2].answer = route ? "{\"host\":{},\"sessions\":[]}" : "{\"host\":{},\"process\":{},\"sessions\":{},\"disks\":[],\"history\":[]}";
        b->vt->refresh(b); entered(&f, 0, 2); entered(&f, 1, 2); // older successful response stays pending
        f.steps[route][3].status = route ? 404 : 403; f.steps[route][3].retry = "3600";
        a->vt->refresh(a); entered(&f, 0, 3); entered(&f, 1, 3); a->vt->destroy(a);
        complete(&f, route, 3); complete(&f, 1 - route, 3);
        ResourcesScreen *target = route ? rb->list : rb;
        CHECK(!target->loaded); CHECK(resources_retry_until(target) >= (double)time(NULL) + 3598);
        complete(&f, 1 - route, 2); CHECK((route ? rb : rb->list)->fresh);
        expire(target); b->vt->refresh(b); entered(&f, 0, 4); entered(&f, 1, 4);
        complete(&f, route, 4); CHECK(target->loaded && target->fresh);
        complete(&f, route, 2); CHECK(target->loaded && target->fresh); // late stale success cannot replace fresh recovery
        CHECK_INT(target->kind == RESOURCE_LIST ? target->sessions.count : target->server.disk_count, route ? 3 : 2);
        complete(&f, 1 - route, 4);
        // A success begun before a refusal is rejected while figures remain cleared.
        b->vt->refresh(b); entered(&f, 0, 5); entered(&f, 1, 5);
        a = create(&f); ra = (ResourcesScreen *)a; a->vt->visible(a, true); entered(&f, 0, 6); entered(&f, 1, 6);
        f.steps[route][6].status = 403; a->vt->destroy(a); complete(&f, route, 6); complete(&f, 1 - route, 6);
        CHECK(!target->loaded); complete(&f, route, 5); CHECK(!target->loaded); complete(&f, 1 - route, 5);
        CHECK((route ? rb : rb->list)->fresh);
        (void)ra; b->vt->destroy(b); clear(&f);
    }
}
static void visibility_detach_and_scope(void) {
    Fixture f; init(&f); Screen *s = create(&f); ResourcesScreen *r = (ResourcesScreen *)s;
    pane_set_root(f.screen.pane, s); entered(&f, 0, 0); entered(&f, 1, 0);
    pane_show(f.screen.pane, false); CHECK(!r->shown && !r->list->shown); complete(&f, 0, 0); complete(&f, 1, 0); CHECK(!r->loaded && !r->list->loaded);
    pane_show(f.screen.pane, true); complete(&f, 0, 1); complete(&f, 1, 1);
    Pane *window = detached_open(pane_take_root(f.screen.pane), NULL, false); CHECK(window != NULL); complete(&f, 0, 2); complete(&f, 1, 2);
    HWND hwnd = GetAncestor(pane_hwnd(window), GA_ROOT); ShowWindow(hwnd, SW_MINIMIZE);
    CHECK(!r->shown && !r->list->shown); CHECK(!r->poller.running && !r->list->poller.running);
    s->vt->refresh(s); CHECK(r->read == NULL && r->list->read == NULL);
    ShowWindow(hwnd, SW_RESTORE); entered(&f, 0, 3); entered(&f, 1, 3);
    g_store.device.permission = "manage"; complete(&f, 0, 3); complete(&f, 1, 3); CHECK(!r->fresh && !r->list->fresh);
    CHECK(!resources_current(r)); g_store.device.permission = "read";
    DestroyWindow(hwnd); clear(&f);
}

static const char *AUTH_ORIGIN = "https://server-resources-auth-tests.invalid";
static const char *AUTH_TOKEN = "brm_aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
static void auth_init(Fixture *f) {
    init(f);
    g_store.device = (Device){.id = xstrdup("server-device"), .label = xstrdup("Server tests"), .permission = xstrdup("read")};
    g_store.routes = routes_copy(ROUTES, 3); api_client_retain(g_store.client); g_store.server = xstrdup(AUTH_ORIGIN);
    wchar_t temp[MAX_PATH], directory[MAX_PATH]; CHECK(GetTempPathW(MAX_PATH, temp) != 0); CHECK(GetTempFileNameW(temp, L"sra", 0, directory) != 0);
    DeleteFileW(directory); CHECK(CreateDirectoryW(directory, NULL)); g_store.cache = cache_new(directory);
    Json *private = json_object(); json_set_num(private, "memoryBytes", 99); CHECK(cache_store(g_store.cache, private, "server-private")); json_free(private);
    CHECK(credentials_save(AUTH_TOKEN, AUTH_ORIGIN));
}
static void auth_clear(Fixture *f) {
    if (g_store.client) api_client_release(g_store.client);
    if (g_store.has_device) device_free(&g_store.device);
    routes_free(g_store.routes, g_store.route_count); cache_remove_all(g_store.cache); cache_free(g_store.cache);
    free(g_store.server); free(g_store.connection_error); CHECK(credentials_remove(AUTH_ORIGIN)); clear(f);
}
static void cancelled_401_clears_all_resources(void) {
    for (int route = 0; route < 2; route++) {
        Fixture f; auth_init(&f); Screen *a = create(&f), *b = create(&f); ResourcesScreen *r = (ResourcesScreen *)b;
        a->vt->visible(a, true); complete(&f, 0, 0); complete(&f, 1, 0);
        b->vt->visible(b, true); complete(&f, 0, 1); complete(&f, 1, 1);
        // Session-specific Resources shares the same finalizer registry.
        Session session = {0}; session.raw = json_parsez("{\"id\":\"c\",\"repo\":\"one/repo\"}");
        Screen *child = resources_screen_new(&session); child->pane = f.screen.pane; session_free(&session);
        child->vt->visible(child, true); screen_fixture_pump(&f.screen, 1); CHECK(((ResourcesScreen *)child)->loaded);
        f.steps[route][2].status = 401;
        a->vt->refresh(a); entered(&f, 0, 2); entered(&f, 1, 2); a->vt->destroy(a);
        b->vt->refresh(b); entered(&f, 0, 3); entered(&f, 1, 3);
        complete(&f, route, 2); complete(&f, 1 - route, 2);
        CHECK(g_store.client == NULL && !g_store.has_device); CHECK(g_store.routes == NULL);
        CHECK(!r->loaded && !r->list->loaded); CHECK(!r->poller.running && !r->list->poller.running);
        CHECK(r->server.lines[0].points == NULL); CHECK(r->list->sessions.items == NULL); CHECK(!((ResourcesScreen *)child)->loaded);
        complete(&f, 0, 3); complete(&f, 1, 3); CHECK(!r->loaded && !r->list->loaded);
        bool failed = false; char *token = credentials_read(AUTH_ORIGIN, &failed); CHECK(token == NULL && !failed); free(token);
        Json *saved = cache_value(g_store.cache, "server-private"); CHECK(saved == NULL); json_free(saved);
        child->vt->destroy(child); b->vt->destroy(b); auth_clear(&f);
    }
}
static void replaced_client_and_device_401_immunity(void) {
    for (int device = 0; device < 2; device++) {
        Fixture f; auth_init(&f); Screen *s = create(&f); ResourcesScreen *r = (ResourcesScreen *)s;
        s->vt->visible(s, true); entered(&f, 0, 0); entered(&f, 1, 0); f.steps[0][0].status = 401; f.steps[1][0].status = 401;
        ApiClient *old = g_store.client;
        if (device) set_string(&g_store.device.id, "replacement");
        else {
            s->vt->destroy(s); s = NULL;
            ServerAddress address; ApiError error; api_error_init(&error); CHECK(server_address_parse("https://replacement.invalid", &address));
            g_store.client = api_client_new(&address, AUTH_TOKEN, &error); server_address_free(&address); api_error_clear(&error);
        }
        ApiClient *replacement = g_store.client; complete(&f, 0, 0); complete(&f, 1, 0);
        CHECK(g_store.client == replacement && g_store.has_device);
        bool failed = false; CHECK_OWNED_STR(credentials_read(AUTH_ORIGIN, &failed), AUTH_TOKEN); CHECK(!failed);
        Json *saved = cache_value(g_store.cache, "server-private"); CHECK(saved != NULL); json_free(saved);
        if (s) { CHECK(!r->loaded && !r->list->loaded); s->vt->destroy(s); }
        if (!device) api_client_release(old);
        auth_clear(&f);
    }
}

void app_server_resources_tests(void) {
    screen_fixture_prepare();
    test_run("Server resources entry and routes honor read token and catalog gating", gating_and_status_entry);
    test_run("Server routes load fail recover and poll independently with partial empty figures", independent_failure_recovery_and_polling);
    test_run("Server rows use saved names repo identity and invalidate stale actions", navigation_records_and_stale_actions);
    test_run("Server route Retry-After survives destroyed cancelled owner and reopening", route_cooldowns_survive_cancel_and_reopen);
    test_run("Server refusal generations survive recovery and reject older pending successes", refused_generation_and_concurrent_recovery);
    test_run("Server cancelled 401 invalidates credentials cache and every surviving Resources channel", cancelled_401_clears_all_resources);
    test_run("Server cancelled old-client and live changed-device 401 protect replacement credentials", replaced_client_and_device_401_immunity);
    test_run("Server polling stops on pane hide detach minimize and scope replacement", visibility_detach_and_scope);
}
