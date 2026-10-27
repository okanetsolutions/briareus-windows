#include "screen_projects_internal.h"
#include "suites.h"
#include "test.h"

static Pane *pane;
static Route routes[] = { { "GET", "/projects", "read" }, { "GET", "/sessions", "read" } };
static const char *rows = "{\"sessions\":["
    "{\"id\":\"a\",\"repo\":\"o/one\",\"status\":\"idle\",\"title\":\"First\"},"
    "{\"id\":\"b\",\"repo\":\"o/two\",\"status\":\"running\",\"title\":\"Second\"},"
    "{\"id\":\"c\",\"repo\":\"other/repo\",\"status\":\"closed\"}]}";
static const char *project_rows = "{\"projects\":[{\"repo\":\"o/one\"},{\"repo\":\"o/two\"}]}";
static const char *answer;
static int answer_status;
static char *last_url;
static HANDLE entered, resume;

static bool transport(void *ctx, const char *method, const char *url, const char *const *headers,
    const void *body, size_t bytes, int timeout, int *status, char **type, char **retry,
    char **response, size_t *len, char **error) {
    CHECK_STR(method, "GET"); CHECK_INT(bytes, 0);
    set_string(&last_url, url);
    bool projects = strstr(url, "/projects") != NULL;
    *status = projects ? 200 : answer_status;
    *type = xstrdup("application/json");
    *response = xstrdup(projects ? project_rows : answer); *len = strlen(*response);
    if (entered && !projects) { SetEvent(entered); WaitForSingleObject(resume, 5000); }
    return true;
}

static void drain(int expected) {
    int done = 0;
    DWORD start = GetTickCount();
    while (done < expected && GetTickCount() - start < 5000) {
        MSG msg;
        while (PeekMessageW(&msg, g_store.hwnd, WM_APP_REQUEST_DONE, WM_APP_REQUEST_DONE, PM_REMOVE)) {
            store_handle_message(msg.message, msg.wParam, msg.lParam); done++;
        }
        if (done < expected) Sleep(1);
    }
    CHECK_INT(done, expected);
}

static void save(const char *key, const char *value) {
    Json *json = json_parse(value, strlen(value)); CHECK(json != NULL);
    CHECK(cache_store(g_store.cache, json, key)); json_free(json);
}

static void check_saved(const char *key, size_t count, const char *first) {
    Json *saved = cache_value(g_store.cache, key);
    CHECK(json_is_array(saved)); CHECK_INT(json_count(saved), count);
    if (first) CHECK_STR(json_str(json_get(json_at(saved, 0), "id")), first);
    json_free(saved);
}

static void reset(void) {
    pane_set_root(pane, NULL); cache_remove_all(g_store.cache);
    free(last_url); last_url = NULL;
    answer = rows; answer_status = 200;
    g_store.routes = routes; g_store.route_count = 2; g_store.device.permission = "read";
    save("projects", project_rows);
}

static SessionsScreen *open_sessions(const Project *project) {
    Screen *screen = sessions_screen_new(project); pane_set_root(pane, screen);
    screen->vt->refresh(screen);
    return (SessionsScreen *)screen;
}

static void finish(void) { pane_set_root(pane, NULL); free(last_url); last_url = NULL; }

static void test_all_and_project_requests_use_separate_scopes(void) {
    reset();
    SessionsScreen *s = open_sessions(NULL); drain(1);
    CHECK_STR(last_url, "https://test.example/api/v1/sessions");
    CHECK_INT(s->count, 3); CHECK(s->all_projects);
    CHECK_STR(session_repo(&s->sessions[2]), "other/repo");
    Doc doc; doc_init(&doc); doc_begin(&doc, NULL, 320); s->base.vt->layout(&s->base, &doc);
    size_t open_rows = 0;
    for (size_t i = 0; i < doc.count; i++) {
        if (doc.items[i].action == ACT_OPEN_SESSION) open_rows++;
        CHECK(doc.items[i].action != ACT_BOARD);
    }
    CHECK_INT(open_rows, 3); doc_free(&doc);
    s->select_mode = true;
    s->base.vt->action(&s->base, ACT_PICK, 0, (POINT){0}); s->base.vt->action(&s->base, ACT_PICK, 1, (POINT){0});
    CHECK_INT(s->picked_count, 2); CHECK_STR(s->picked[0], "a"); CHECK_STR(s->picked[1], "b");
    CHECK(s->base.vt->key(&s->base, VK_ESCAPE, false, false)); CHECK_INT(s->picked_count, 0);
    check_saved("sessions", 3, "a"); check_saved("sessions:o/one", 1, "a"); check_saved("sessions:o/two", 1, "b");
    Project project = { .repo = "o/one", .label = "One" };
    s = open_sessions(&project); drain(1);
    // The sessions route filters its aggregate response in core/api.c.
    CHECK_STR(last_url, "https://test.example/api/v1/sessions");
    CHECK_INT(s->count, 1); CHECK(!s->all_projects);
    CHECK_STR(session_id(&s->sessions[0]), "a");
    check_saved("sessions", 3, "a");
    finish();
}

static void test_projects_all_navigation_and_capability(void) {
    reset();
    Screen *root = projects_screen_new(); pane_set_root(pane, root);
    root->vt->refresh(root); drain(2);
    check_saved("sessions", 3, "a");
    Doc doc; doc_init(&doc); doc_begin(&doc, NULL, 320); root->vt->layout(root, &doc);
    bool found = false;
    for (size_t i = 0; i < doc.count; i++) if (doc.items[i].action == ACT_ALL_SESSIONS) {
        found = true; root->vt->action(root, doc.items[i].action, doc.items[i].arg, (POINT){0}); break;
    }
    CHECK(found); doc_free(&doc);
    CHECK_INT(pane_depth(pane), 2);
    Screen *all = pane_top(pane); CHECK_STR(all->id, "sessions");
    all->vt->refresh(all); drain(1);
    CHECK_INT(((SessionsScreen *)all)->count, 3);
    all->vt->action(all, ACT_BACK, 0, (POINT){0}); CHECK(pane_top(pane) == root);
    g_store.route_count = 1;
    root->vt->action(root, ACT_ALL_SESSIONS, 0, (POINT){0}); CHECK_INT(pane_depth(pane), 1);
    doc_init(&doc); doc_begin(&doc, NULL, 320); root->vt->layout(root, &doc);
    for (size_t i = 0; i < doc.count; i++) CHECK(doc.items[i].action != ACT_ALL_SESSIONS);
    doc_free(&doc); finish();
}

static void test_all_restores_cache_preserves_errors_and_accepts_empty(void) {
    reset();
    SessionsScreen *s = open_sessions(NULL); drain(1);
    answer_status = 503; answer = "{\"error\":\"Unavailable\"}";
    s = open_sessions(NULL);
    CHECK_INT(s->count, 3); CHECK(s->loaded); drain(1);
    CHECK_INT(s->count, 3); CHECK(s->error != NULL);
    check_saved("sessions", 3, "a");
    answer_status = 200; answer = "{\"sessions\":[]}";
    s->base.vt->refresh(&s->base); drain(1);
    CHECK_INT(s->count, 0); CHECK(s->error == NULL);
    check_saved("sessions", 0, NULL); check_saved("sessions:o/one", 0, NULL); check_saved("sessions:o/two", 0, NULL);
    finish();
}

static void test_delete_updates_aggregate_and_project_caches(void) {
    reset(); open_sessions(NULL); drain(1);
    save("transcript:a", "{\"messages\":[]}");
    sessions_forget("o/one", "a");
    check_saved("sessions", 2, "b"); check_saved("sessions:o/one", 0, NULL); check_saved("sessions:o/two", 1, "b");
    Json *transcript = cache_value(g_store.cache, "transcript:a"); CHECK(transcript == NULL); json_free(transcript);
    answer_status = 503; answer = "{\"error\":\"Unavailable\"}";
    SessionsScreen *s = open_sessions(NULL); CHECK_INT(s->count, 2); drain(1);
    CHECK_STR(session_id(&s->sessions[0]), "b");
    finish();
}

static void test_hidden_and_destroyed_all_discard_completions(void) {
    reset(); SessionsScreen *s = open_sessions(NULL); drain(1);
    entered = CreateEventW(NULL, TRUE, FALSE, NULL); resume = CreateEventW(NULL, TRUE, FALSE, NULL);
    answer = "{\"sessions\":[]}";
    s->base.vt->refresh(&s->base); CHECK_INT(WaitForSingleObject(entered, 5000), WAIT_OBJECT_0);
    s->base.vt->visible(&s->base, false); SetEvent(resume); drain(1);
    CHECK_INT(s->count, 3); check_saved("sessions", 3, "a");
    ResetEvent(entered); ResetEvent(resume);
    s->base.vt->refresh(&s->base); CHECK_INT(WaitForSingleObject(entered, 5000), WAIT_OBJECT_0);
    pane_set_root(pane, NULL); SetEvent(resume); drain(1); check_saved("sessions", 3, "a");
    CloseHandle(entered); CloseHandle(resume); entered = resume = NULL;
    finish();
}

void app_projects_tests(void) {
    theme_init();
    HWND parent = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 10, 10, NULL, NULL, GetModuleHandleW(NULL), NULL);
    pane = pane_create(parent, true);
    wchar_t dir[MAX_PATH]; GetTempPathW(MAX_PATH, dir); wcscat(dir, L"briareus-projects-tests");
    memset(&g_store, 0, sizeof g_store); g_store.hwnd = parent; g_store.cache = cache_new(dir);
    g_store.has_device = true; g_store.server = "https://test.example"; g_store.active = true;
    ServerAddress address; CHECK(server_address_parse(g_store.server, &address));
    ApiError error; api_error_init(&error);
    g_store.client = api_client_new(&address, "brm_aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", &error);
    api_client_set_transport(g_store.client, transport, NULL);
    // Allocate the pane's retained stack before the per-test leak checkpoints.
    pane_set_root(pane, projects_screen_new()); pane_push(pane, sessions_screen_new(NULL)); pane_set_root(pane, NULL);
    test_run("all and project sessions use distinct request and cache scopes", test_all_and_project_requests_use_separate_scopes);
    test_run("projects opens all sessions and gates it by capability", test_projects_all_navigation_and_capability);
    test_run("all sessions restores cached rows and handles errors and empty results", test_all_restores_cache_preserves_errors_and_accepts_empty);
    test_run("deleting sessions clears aggregate and project caches", test_delete_updates_aggregate_and_project_caches);
    test_run("all sessions discards hidden and destroyed request completions", test_hidden_and_destroyed_all_discard_completions);
    pane_destroy(pane); DestroyWindow(parent);
    cache_remove_all(g_store.cache); cache_free(g_store.cache); RemoveDirectoryW(dir);
    api_client_release(g_store.client); server_address_free(&address); api_error_clear(&error);
    memset(&g_store, 0, sizeof g_store);
}
