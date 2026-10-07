// Exercise the real screen refresh/visibility/activation and request completion paths.
#include "screens.h"
#include "str.h"
#include "suites.h"
#include "test.h"
#include <stdlib.h>
#include <string.h>

static Pane *pane;
static volatile LONG list_calls, session_calls, detail_calls, replies;
static int list_status;
static const char *list_body, *retry_header;
static bool network_fail, reject_fresh;
static HANDLE hold, entered;
static Route routes[] = {
    { "GET", "/pulls", "read" }, { "GET", "/sessions", "read" }, { "GET", "/issues/{number}", "read" }
};
static bool transport(void *ctx, const char *method, const char *url, const char *const *headers,
                      const void *body, size_t body_len, int timeout_ms, int *status, char **content_type,
                      char **retry_after, char **response, size_t *response_len, char **error_message) {
    const char *payload;
    if (strstr(url, "/pulls?")) {
        InterlockedIncrement(&list_calls); *status = list_status; payload = list_body;
        if (reject_fresh && strstr(url, "fresh=1")) { *status = 400; payload = "{\"error\":\"Unknown fresh argument\"}"; }
        if (retry_header) *retry_after = xstrdup(retry_header);
        if (hold) { SetEvent(entered); WaitForSingleObject(hold, 5000); }
        if (network_fail) { *error_message = xstrdup("Connection failed"); InterlockedIncrement(&replies); return false; }
    } else if (strstr(url, "/issues/")) {
        InterlockedIncrement(&detail_calls); *status = 200;
        payload = "{\"issue\":{\"number\":101,\"title\":\"Outside page\",\"state\":\"open\",\"body\":\"Still accessible\"}}";
    } else {
        InterlockedIncrement(&session_calls); *status = 200; payload = "{\"sessions\":[]}";
    }
    *content_type = xstrdup("application/json"); *response = xstrdup(payload); *response_len = strlen(payload);
    InterlockedIncrement(&replies);
    return true;
}
static void drain(int expected) {
    DWORD start = GetTickCount();
    int done = 0;
    while (done < expected && GetTickCount() - start < 5000) {
        MSG msg;
        while (PeekMessageW(&msg, NULL, WM_APP_REQUEST_DONE, WM_APP_REQUEST_DONE, PM_REMOVE)) {
            store_handle_message(msg.message, msg.wParam, msg.lParam); done++;
        }
        if (done < expected) Sleep(1);
    }
    CHECK_INT(done, expected);
}
static Project project(void) { Project p; memset(&p, 0, sizeof p); p.repo = "o/r"; p.label = "Test"; return p; }
static Screen *board(void) { Project p = project(); Screen *s = pulls_screen_new(&p); pane_set_root(pane, s); return s; }
static void reset(void) {
    pane_set_root(pane, NULL); cache_remove_all(g_store.cache);
    list_calls = session_calls = detail_calls = replies = 0;
    list_status = 200; retry_header = NULL; network_fail = reject_fresh = false;
    list_body = "{\"pulls\":[{\"number\":1,\"title\":\"Keep this row\"}],\"issues\":[],\"syncedAt\":\"2026-10-07T12:00:00.000Z\"}";
    g_store.route_count = 2; g_store.device.id = "d1";
}
static void expire(void) {
    Json *j = json_object(); json_set_num(j, "until", 0);
    cache_store(g_store.cache, j, "pulls-retry:https://test.example:d1:o/r"); json_free(j);
}
static HeaderInfo header(Screen *s) { HeaderInfo h; memset(&h, 0, sizeof h); s->vt->header(s, &h); return h; }
static bool layout_contains(Screen *s, const char *text) {
    Doc d; doc_init(&d); doc_begin(&d, NULL, 800); s->vt->layout(s, &d);
    bool found = false;
    for (size_t i = 0; i < d.count; i++) if (d.items[i].text && strstr(d.items[i].text, text)) found = true;
    doc_free(&d); return found;
}
static void test_cached_sync_and_error_recovery_transitions(void) {
    reset(); Screen *s = board(); s->vt->refresh(s); drain(2);
    HeaderInfo before = header(s);
    time_t synced; CHECK(board_date_parse("2026-10-07T12:00:00Z", &synced));
    char *age = format_relative(synced); CHECK(strstr(before.subtitle, age)); free(age);
    s->vt->refresh(s); drain(2);
    HeaderInfo cached = header(s); CHECK_STR(before.subtitle, cached.subtitle);
    list_status = 429; list_body = "{\"error\":\"Allowance spent\"}"; retry_header = "3600";
    s->vt->refresh(s); drain(2);
    CHECK(layout_contains(s, "Keep this row"));
    CHECK(layout_contains(s, "rate limiting requests"));
    HeaderInfo limited = header(s); CHECK(strstr(limited.subtitle, "retry after"));
    LONG sent = list_calls;
    s->vt->refresh(s); drain(1);
    s->vt->visible(s, false); s->vt->visible(s, true); s->vt->timer(s, 1); drain(1);
    s->vt->activated(s, true); s->vt->timer(s, 1); drain(1);
    CHECK_INT(list_calls, sent); CHECK_INT(session_calls, 6);
    // A newly opened board shares the deadline and restores both row and server freshness.
    s = board(); s->vt->refresh(s); drain(1);
    CHECK(layout_contains(s, "Keep this row")); CHECK_INT(list_calls, sent);
    HeaderInfo restored = header(s); age = format_relative(synced); CHECK(strstr(restored.subtitle, age)); free(age);
    expire(); list_status = 200; retry_header = NULL;
    list_body = "{\"pulls\":[{\"number\":2,\"title\":\"Recovered\"}],\"syncedAt\":\"2026-10-07T13:00:00Z\"}";
    s->vt->refresh(s); drain(2); CHECK_INT(list_calls, sent + 1);
    CHECK(layout_contains(s, "Recovered")); CHECK(!layout_contains(s, "rate limiting requests"));
    pane_set_root(pane, NULL);
}
static void test_truncated_issue_is_not_gone_and_reads_detail(void) {
    reset(); g_store.route_count = 3;
    list_body = "{\"pulls\":[],\"issues\":[],\"issuesTruncated\":true}";
    Project p = project(); IssueSummary issue; memset(&issue, 0, sizeof issue);
    issue.number = 101; issue.title = "Outside page";
    Screen *s = issue_detail_screen_new(&p, &issue); pane_set_root(pane, s);
    s->vt->refresh(s); drain(3);
    CHECK_INT(detail_calls, 1); CHECK(layout_contains(s, "Still accessible"));
    CHECK(!layout_contains(s, "This issue is no longer on the board"));
    // Older servers without detail must still preserve the supplied row on an incomplete list.
    pane_set_root(pane, NULL); cache_remove_all(g_store.cache); g_store.route_count = 2;
    s = issue_detail_screen_new(&p, &issue); pane_set_root(pane, s); s->vt->refresh(s); drain(2);
    CHECK(!layout_contains(s, "This issue is no longer on the board"));
    pane_set_root(pane, NULL);
}
static void test_legacy_freshness_and_catalog_account_changes(void) {
    reset(); list_body = "{\"pulls\":[],\"issues\":[]}";
    Screen *s = board(); s->vt->refresh(s); drain(2);
    HeaderInfo h = header(s); CHECK(strstr(h.subtitle, "synced"));
    s = board(); s->vt->refresh(s); drain(2); h = header(s); CHECK(strstr(h.subtitle, "synced"));
    Json *legacy = cache_value(g_store.cache, "pulls:o/r");
    time_t received = time(NULL) - 86400; json_set_num(legacy, "_receivedAt", (double)received);
    cache_store(g_store.cache, legacy, "pulls:o/r"); json_free(legacy);
    ApiError e; api_error_init(&e); api_error_set(&e, API_HTTP, 503, "Unavailable", 3600);
    pulls_note_failure("o/r", &e, time(NULL));
    s = board(); s->vt->refresh(s); drain(1); LONG sent = list_calls;
    h = header(s); char *age = format_relative(received); CHECK(strstr(h.subtitle, age)); free(age);
    g_store.device.id = "d2"; s->vt->refresh(s); drain(2); CHECK_INT(list_calls, sent + 1);
    g_store.route_count = 0; s->vt->refresh(s); CHECK_INT(list_calls, sent + 1);
    api_error_clear(&e); pane_set_root(pane, NULL);
}
static void test_old_fresh_rejection_and_network_error_preserve_board(void) {
    reset(); reject_fresh = true;
    Screen *s = board(); s->vt->refresh(s); drain(3); CHECK_INT(list_calls, 2);
    CHECK(layout_contains(s, "Keep this row"));
    reject_fresh = false; network_fail = true;
    s->vt->refresh(s); drain(2);
    CHECK(layout_contains(s, "Keep this row")); CHECK(layout_contains(s, "Connection failed"));
    expire(); network_fail = false; s->vt->refresh(s); drain(2);
    CHECK(!layout_contains(s, "Connection failed")); pane_set_root(pane, NULL);
}
static void test_http_failures_keep_rows_and_permission_gates_stop_reads(void) {
    reset(); Screen *s = board(); s->vt->refresh(s); drain(2);
    const int statuses[] = { 403, 404, 409, 502, 503 };
    for (size_t i = 0; i < sizeof statuses / sizeof *statuses; i++) {
        expire(); list_status = statuses[i]; list_body = "{\"error\":\"Read refused\"}";
        s->vt->refresh(s); drain(2); CHECK(layout_contains(s, "Keep this row"));
        CHECK(layout_contains(s, "Read refused"));
    }
    LONG sent = list_calls; g_store.device.permission = "unknown";
    s->vt->refresh(s); CHECK_INT(list_calls, sent);
    g_store.device.permission = "read"; pane_set_root(pane, NULL);
}
static void test_hidden_and_replaced_client_discard_stale_completions(void) {
    reset(); Screen *s = board(); s->vt->refresh(s); drain(2);
    hold = CreateEventW(NULL, TRUE, FALSE, NULL); entered = CreateEventW(NULL, TRUE, FALSE, NULL);
    list_body = "{\"pulls\":[{\"number\":3,\"title\":\"Stale row\"}]}";
    s->vt->refresh(s); CHECK_INT(WaitForSingleObject(entered, 5000), WAIT_OBJECT_0);
    s->vt->refresh(s); // An in-flight read is retained rather than replaced by F5.
    CHECK_INT(list_calls, 2);
    s->vt->visible(s, false); SetEvent(hold); drain(2);
    CHECK(layout_contains(s, "Keep this row")); CHECK(!layout_contains(s, "Stale row"));
    ResetEvent(hold); ResetEvent(entered); s->vt->visible(s, true); s->vt->refresh(s);
    CHECK_INT(WaitForSingleObject(entered, 5000), WAIT_OBJECT_0);
    ApiClient *previous = g_store.client;
    ServerAddress address; CHECK(server_address_parse("https://other.example", &address));
    ApiError e; api_error_init(&e);
    g_store.client = api_client_new(&address, "brm_bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb", &e);
    api_client_set_transport(g_store.client, transport, NULL);
    SetEvent(hold); drain(2);
    CHECK(!layout_contains(s, "Stale row"));
    api_client_release(previous); server_address_free(&address); api_error_clear(&e);
    CloseHandle(hold); CloseHandle(entered); hold = entered = NULL;
    pane_set_root(pane, NULL);
}
void app_pulls_tests(void) {
    theme_init();
    HWND parent = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 10, 10, NULL, NULL, GetModuleHandleW(NULL), NULL);
    pane = pane_create(parent, false);
    wchar_t dir[MAX_PATH]; GetTempPathW(MAX_PATH, dir); wcscat(dir, L"briareus-pulls-tests");
    memset(&g_store, 0, sizeof g_store); g_store.hwnd = parent; g_store.cache = cache_new(dir);
    g_store.has_device = true; g_store.device.permission = "read"; g_store.routes = routes;
    g_store.server = "https://test.example"; g_store.active = true;
    ServerAddress address; CHECK(server_address_parse(g_store.server, &address));
    ApiError e; api_error_init(&e);
    g_store.client = api_client_new(&address, "brm_aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", &e);
    api_client_set_transport(g_store.client, transport, NULL);
    // The pane retains its screen-stack capacity between tests; allocate that fixture before leak checkpoints.
    board(); pane_set_root(pane, NULL);
    test_run("pulls cached freshness, cooldown and recovery transitions", test_cached_sync_and_error_recovery_transitions);
    test_run("pulls truncated issue detail and legacy transitions", test_truncated_issue_is_not_gone_and_reads_detail);
    test_run("pulls legacy freshness, catalog and account transitions", test_legacy_freshness_and_catalog_account_changes);
    test_run("pulls older fresh argument and network recovery", test_old_fresh_rejection_and_network_error_preserve_board);
    test_run("pulls HTTP failures and permission gates", test_http_failures_keep_rows_and_permission_gates_stop_reads);
    test_run("pulls hidden and replaced-client stale completions", test_hidden_and_replaced_client_discard_stale_completions);
    pane_destroy(pane); DestroyWindow(parent);
    cache_remove_all(g_store.cache); cache_free(g_store.cache); RemoveDirectoryW(dir);
    api_client_release(g_store.client); server_address_free(&address); api_error_clear(&e);
    memset(&g_store, 0, sizeof g_store);
}
