#include "screen_projects_internal.h"
#include "suites.h"
#include "test.h"

static Pane *pane;
static Route routes[] = { { "GET", "/projects", "read" }, { "GET", "/sessions", "read" }, { "GET", "/pulls", "read" } };
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

static const Project one = { .repo = "o/one", .label = "One" };

static void test_project_sessions_request_scope_and_board(void) {
    reset();
    SessionsScreen *s = open_sessions(&one); drain(1);
    // The sessions route filters its aggregate response in core/api.c.
    CHECK_STR(last_url, "https://test.example/api/v1/sessions");
    CHECK_INT(s->count, 1); CHECK_STR(session_id(&s->sessions[0]), "a"); CHECK_STR(s->base.id, "sessions:o/one");
    Doc doc; doc_init(&doc); doc_begin(&doc, NULL, 320); s->base.vt->layout(&s->base, &doc);
    size_t board_rows = 0, open_rows = 0;
    for (size_t i = 0; i < doc.count; i++) {
        if (doc.items[i].action == ACT_BOARD) board_rows++;
        if (doc.items[i].action == ACT_OPEN_SESSION) open_rows++;
    }
    CHECK_INT(board_rows, 1); CHECK_INT(open_rows, 1); doc_free(&doc);
    s->select_mode = true;
    s->base.vt->action(&s->base, ACT_PICK, 0, (POINT){0});
    CHECK_INT(s->picked_count, 1); CHECK_STR(s->picked[0], "a");
    CHECK(s->base.vt->key(&s->base, VK_ESCAPE, false, false)); CHECK_INT(s->picked_count, 0);
    check_saved("sessions:o/one", 1, "a");
    finish();
}

static void test_projects_lists_only_projects(void) {
    reset();
    Screen *root = projects_screen_new(); pane_set_root(pane, root);
    root->vt->refresh(root); drain(2);
    check_saved("sessions:o/one", 1, "a"); check_saved("sessions:o/two", 1, "b");
    Json *aggregate = cache_value(g_store.cache, "sessions"); CHECK(aggregate == NULL); json_free(aggregate);
    g_store.route_count = 3;
    Doc doc; doc_init(&doc); doc_begin(&doc, NULL, 320); root->vt->layout(root, &doc);
    size_t listed = 0;
    for (size_t i = 0; i < doc.count; i++) if (doc.items[i].action == ACT_OPEN_PROJECT) listed++;
    // Only the projects themselves, without cross-project session or pull request entries.
    CHECK_INT(listed, 2); doc_free(&doc);
    root->vt->action(root, ACT_OPEN_PROJECT, 1, (POINT){0}); CHECK_INT(pane_depth(pane), 2);
    Screen *project = pane_top(pane); CHECK_STR(project->id, "sessions:o/two");
    project->vt->action(project, ACT_BACK, 0, (POINT){0}); CHECK(pane_top(pane) == root);
    finish();
}

static void test_project_counts_save_live_scope_when_project_cache_is_stale(void) {
    reset(); save("projects", "[]");
    char *name = cache_file_name("projects"); wchar_t *wide = utf8_to_wide(name); free(name);
    wchar_t dir[MAX_PATH], path[MAX_PATH]; GetTempPathW(MAX_PATH, dir);
    swprintf(path, MAX_PATH, L"%lsbriareus-projects-tests\\%ls", dir, wide); free(wide);
    HANDLE lock = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    CHECK(lock != INVALID_HANDLE_VALUE);
    Screen *root = projects_screen_new(); pane_set_root(pane, root);
    root->vt->refresh(root); drain(2);
    Json *saved = cache_value(g_store.cache, "projects"); CHECK_INT(json_count(saved), 0); json_free(saved);
    check_saved("sessions:o/one", 1, "a"); check_saved("sessions:o/two", 1, "b");
    screen_projects_recount(pane);
    CloseHandle(lock); finish();
}

static void test_project_restores_cache_preserves_errors_and_accepts_empty(void) {
    reset();
    SessionsScreen *s = open_sessions(&one); drain(1);
    answer_status = 503; answer = "{\"error\":\"Unavailable\"}";
    s = open_sessions(&one);
    CHECK_INT(s->count, 1); CHECK(s->loaded); drain(1);
    CHECK_INT(s->count, 1); CHECK(s->error != NULL);
    check_saved("sessions:o/one", 1, "a");
    answer_status = 200; answer = "{\"sessions\":[]}";
    s->base.vt->refresh(&s->base); drain(1);
    CHECK_INT(s->count, 0); CHECK(s->error == NULL);
    check_saved("sessions:o/one", 0, NULL);
    finish();
}

static void test_delete_updates_project_cache(void) {
    reset(); save("sessions:o/two", "[{\"id\":\"b\",\"repo\":\"o/two\"}]");
    open_sessions(&one); drain(1);
    save("transcript:a", "{\"messages\":[]}");
    sessions_forget("o/one", "a");
    check_saved("sessions:o/one", 0, NULL); check_saved("sessions:o/two", 1, "b");
    Json *transcript = cache_value(g_store.cache, "transcript:a"); CHECK(transcript == NULL); json_free(transcript);
    answer_status = 503; answer = "{\"error\":\"Unavailable\"}";
    SessionsScreen *s = open_sessions(&one); CHECK_INT(s->count, 0); drain(1);
    finish();
}

static void test_hidden_and_destroyed_project_discard_completions(void) {
    reset(); SessionsScreen *s = open_sessions(&one); drain(1);
    entered = CreateEventW(NULL, TRUE, FALSE, NULL); resume = CreateEventW(NULL, TRUE, FALSE, NULL);
    answer = "{\"sessions\":[]}";
    s->base.vt->refresh(&s->base); CHECK_INT(WaitForSingleObject(entered, 5000), WAIT_OBJECT_0);
    s->base.vt->visible(&s->base, false); SetEvent(resume); drain(1);
    CHECK_INT(s->count, 1); check_saved("sessions:o/one", 1, "a");
    ResetEvent(entered); ResetEvent(resume);
    s->base.vt->refresh(&s->base); CHECK_INT(WaitForSingleObject(entered, 5000), WAIT_OBJECT_0);
    pane_set_root(pane, NULL); SetEvent(resume); drain(1); check_saved("sessions:o/one", 1, "a");
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
    pane_set_root(pane, projects_screen_new()); pane_push(pane, sessions_screen_new(&one)); pane_set_root(pane, NULL);
    test_run("project sessions request their scope and open the project board", test_project_sessions_request_scope_and_board);
    test_run("projects lists only projects and opens a project's sessions", test_projects_lists_only_projects);
    test_run("project counts save live scope when project cache is stale", test_project_counts_save_live_scope_when_project_cache_is_stale);
    test_run("project sessions restore cached rows and handle errors and empty results", test_project_restores_cache_preserves_errors_and_accepts_empty);
    test_run("deleting a session clears its project cache", test_delete_updates_project_cache);
    test_run("project sessions discard hidden and destroyed request completions", test_hidden_and_destroyed_project_discard_completions);
    pane_destroy(pane); DestroyWindow(parent);
    cache_remove_all(g_store.cache); cache_free(g_store.cache); RemoveDirectoryW(dir);
    api_client_release(g_store.client); server_address_free(&address); api_error_clear(&error);
    memset(&g_store, 0, sizeof g_store);
}
