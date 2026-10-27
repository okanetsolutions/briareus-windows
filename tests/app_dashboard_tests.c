#include "screen_dashboard_internal.h"
#include "dialogs.h"
#include "resource.h"
#include "suites.h"
#include "test.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static Route ROUTES[] = {
    { "GET", "/usage/all", "admin" },
    { "GET", "/settings/providers", "admin" },
    { "GET", "/settings/providers/{id}/status", "admin" },
};
static const char *LEDGER = "{\"turns\":0,\"period\":\"month\",\"month\":\"2026-10\"}";
static const char *ACCOUNTS = "{\"providers\":[{\"id\":7,\"label\":\"Work\",\"binary\":\"codex\"}]}";
static const char *STATUS = "{\"status\":{\"auth\":{\"email\":\"work@example.com\"},\"usage\":{\"windows\":[{\"label\":\"fixture-window\",\"usedPct\":25}]}}}";
enum { LEDGER_READ, ACCOUNTS_READ, STATUS_READ, READ_COUNT, RECORD_CAP = 64 };
typedef struct {
    const char *ledger, *accounts, *status, *second_status, *retry, *second_retry, *block_path;
    int accounts_code, status_code, second_code;
    volatile LONG count[READ_COUNT], recorded;
    char *urls[RECORD_CAP];
    bool reads[RECORD_CAP];
    int block_kind;
    HANDLE entered, resume;
} DashboardStub;
static HWND dispatcher;
static Pane *pane;
static UINT poll_timer;
static int delivered;

static bool transport(void *ctx, const char *method, const char *url, const char *const *headers,
    const void *body, size_t bytes, int timeout, int *status, char **type, char **retry,
    char **response, size_t *len, char **error) {
    (void)headers; (void)body; (void)timeout;
    DashboardStub *stub = ctx;
    int kind = strstr(url, "/usage/all") ? LEDGER_READ : strstr(url, "/status") ? STATUS_READ : ACCOUNTS_READ;
    LONG sequence = InterlockedIncrement(&stub->count[kind]);
    LONG record = InterlockedIncrement(&stub->recorded) - 1;
    if (record < RECORD_CAP) {
        stub->urls[record] = xstrdup(url);
        stub->reads[record] = str_eq(method, "GET") && bytes == 0;
    }
    const char *answer = stub->ledger ? stub->ledger : LEDGER;
    *status = 200; *error = NULL;
    if (kind == ACCOUNTS_READ) { answer = stub->accounts ? stub->accounts : ACCOUNTS; *status = stub->accounts_code ? stub->accounts_code : 200; }
    if (kind == STATUS_READ) {
        bool second = strstr(url, "/8/status") != NULL;
        answer = second && stub->second_status ? stub->second_status : stub->status ? stub->status : STATUS;
        int code = second ? stub->second_code : stub->status_code;
        if (code) *status = code;
    }
    *type = xstrdup("application/json");
    const char *delay = strstr(url, "/8/status") && stub->second_retry ? stub->second_retry : stub->retry ? stub->retry : "300";
    *retry = kind != LEDGER_READ && *status == 429 ? xstrdup(delay) : NULL;
    // Own the response before blocking: the next generation may replace the fixture on the UI thread.
    *response = xstrdup(answer); *len = strlen(*response);
    bool held = stub->block_path ? strstr(url, stub->block_path) != NULL : sequence == 1;
    if (stub->entered && kind == stub->block_kind && held) {
        SetEvent(stub->entered); WaitForSingleObject(stub->resume, 10000);
    }
    return true;
}
static void pump(int expected) {
    int received = 0;
    ULONGLONG end = GetTickCount64() + 10000;
    while (received < expected && GetTickCount64() < end) {
        MSG msg;
        while (PeekMessageW(&msg, dispatcher, WM_APP_REQUEST_DONE, WM_APP_REQUEST_DONE, PM_REMOVE)) {
            store_handle_message(msg.message, msg.wParam, msg.lParam); received++; delivered++;
        }
        if (received < expected) Sleep(1);
    }
    CHECK_INT(received, expected);
}
static void poll(Screen *screen) { screen->vt->timer(screen, poll_timer); }
static void first_poll(Screen *screen) {
    ULONGLONG end = GetTickCount64() + 10000;
    MSG msg;
    while (GetTickCount64() < end) {
        if (PeekMessageW(&msg, pane_hwnd(pane), WM_TIMER, WM_TIMER, PM_REMOVE)) {
            poll_timer = (UINT)msg.wParam; poll(screen); return;
        }
        Sleep(1);
    }
    CHECK(false);
}
static Screen *setup(DashboardStub *stub, const char *permission, size_t route_count) {
    cache_remove_all(g_store.cache);
    g_store.has_device = true; g_store.device.permission = (char *)permission;
    g_store.routes = ROUTES; g_store.route_count = route_count; g_store.active = true;
    delivered = 0;
    screen_dashboard_preference = (UsageWindow){0};
    ServerAddress address; ApiError error; api_error_init(&error);
    CHECK(server_address_parse("https://example.com", &address));
    g_store.client = api_client_new(&address, "brm_aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", &error);
    CHECK(g_store.client != NULL); server_address_free(&address); api_error_clear(&error);
    api_client_set_transport(g_store.client, transport, stub);
    Screen *screen = dashboard_screen_new(); pane_set_root(pane, screen); first_poll(screen);
    return screen;
}
static void block(DashboardStub *stub, int kind) {
    stub->block_kind = kind;
    stub->entered = CreateEventW(NULL, TRUE, FALSE, NULL); stub->resume = CreateEventW(NULL, TRUE, FALSE, NULL);
    CHECK(stub->entered != NULL); CHECK(stub->resume != NULL);
}
static void await_block(DashboardStub *stub) { CHECK_INT(WaitForSingleObject(stub->entered, 10000), WAIT_OBJECT_0); }
static void draw(Screen *screen, Doc *doc, int width) {
    doc_begin(doc, NULL, width); screen->vt->layout(screen, doc); doc_end(doc);
}
static const char *text_with(const Doc *doc, const char *fixture) {
    for (size_t i = 0; i < doc->count; i++) if (doc->items[i].text && strstr(doc->items[i].text, fixture)) return doc->items[i].text;
    return NULL;
}
static void check_percent(const Doc *doc, const char *fixture, int expected) {
    const char *text = text_with(doc, fixture); CHECK(text != NULL);
    if (!text) return;
    const char *percent = strchr(text, '%');
    if (expected < 0) { CHECK(percent == NULL); return; }
    CHECK(percent != NULL);
    if (!percent) return;
    const char *number = percent;
    while (number > text && isdigit((unsigned char)number[-1])) number--;
    CHECK(number < percent); CHECK_INT(atoi(number), expected);
}
static void check_url(const DashboardStub *stub, const char *url) {
    int matches = 0;
    for (LONG i = 0; i < stub->recorded && i < RECORD_CAP; i++) if (str_eq(stub->urls[i], url)) matches++;
    CHECK_INT(matches, 1);
}
static void cleanup(DashboardStub *stub, Doc *doc) {
    pane_set_root(pane, NULL); doc_free(doc);
    CHECK_INT(delivered, stub->recorded);
    for (LONG i = 0; i < stub->recorded && i < RECORD_CAP; i++) { CHECK(stub->reads[i]); free(stub->urls[i]); }
    CHECK(stub->recorded <= RECORD_CAP);
    api_client_release(g_store.client); g_store.client = NULL;
    if (stub->entered) CloseHandle(stub->entered);
    if (stub->resume) CloseHandle(stub->resume);
}
static void test_remaining_values(void) {
    DashboardStub stub = {0};
    stub.status = "{\"status\":{\"usage\":{\"windows\":["
        "{\"label\":\"partial-fixture\",\"usedPct\":25},{\"label\":\"exhausted-fixture\",\"usedPct\":100},"
        "{\"label\":\"unused-fixture\",\"usedPct\":0},{\"label\":\"over-fixture\",\"usedPct\":120},"
        "{\"label\":\"under-fixture\",\"usedPct\":-10},{\"label\":\"null-fixture\",\"usedPct\":null},"
        "{\"label\":\"string-fixture\",\"usedPct\":\"25\"},{\"label\":\"missing-fixture\"}]}}}";
    Screen *screen = setup(&stub, "admin", 3); pump(3); Doc doc; doc_init(&doc); draw(screen, &doc, 600);
    check_percent(&doc, "partial-fixture", 75); check_percent(&doc, "exhausted-fixture", 0);
    check_percent(&doc, "unused-fixture", 100); check_percent(&doc, "over-fixture", 0);
    check_percent(&doc, "under-fixture", 100); check_percent(&doc, "null-fixture", -1);
    check_percent(&doc, "string-fixture", -1); check_percent(&doc, "missing-fixture", -1);
    cleanup(&stub, &doc);
}
static void test_account_loading(void) {
    DashboardStub stub = {0}; block(&stub, ACCOUNTS_READ);
    stub.accounts = "{\"providers\":[{\"id\":7,\"label\":\"Work\"},{\"id\":8,\"label\":\"Personal\",\"active\":false},{\"id\":0}]}";
    stub.second_code = 404;
    Screen *screen = setup(&stub, "admin", 3); await_block(&stub); pump(1);
    poll(screen); pump(1); poll(screen); pump(1); CHECK_INT(stub.count[ACCOUNTS_READ], 1);
    SetEvent(stub.resume); pump(3);
    CHECK_INT(stub.count[STATUS_READ], 2);
    check_url(&stub, "https://example.com/api/v1/settings/providers");
    check_url(&stub, "https://example.com/api/v1/settings/providers/7/status");
    check_url(&stub, "https://example.com/api/v1/settings/providers/8/status");
    Doc doc; doc_init(&doc);
    for (int width = 360; width <= 1600; width += 620) {
        draw(screen, &doc, width); CHECK(text_with(&doc, "work@example.com") != NULL);
        CHECK(text_with(&doc, "Personal") != NULL); check_percent(&doc, "fixture-window", 75);
    }
    cleanup(&stub, &doc);
}
static void test_unavailable_and_permissions(void) {
    for (int restricted = 0; restricted < 2; restricted++) {
        DashboardStub stub = {0}; Screen *screen = setup(&stub, restricted ? "admin" : "read", restricted ? 2 : 3);
        if (restricted) pump(1);
        CHECK_INT(stub.count[ACCOUNTS_READ], 0); CHECK_INT(stub.count[STATUS_READ], 0);
        CHECK_INT(stub.count[LEDGER_READ], restricted);
        Doc doc; doc_init(&doc); draw(screen, &doc, 600); CHECK(text_with(&doc, "work@example.com") == NULL);
        cleanup(&stub, &doc);
    }
    DashboardStub stub = {0}; stub.accounts = "{}";
    Screen *screen = setup(&stub, "admin", 3); pump(2); CHECK_INT(stub.count[STATUS_READ], 0);
    stub.accounts = ACCOUNTS; stub.status = "{\"status\":{\"auth\":{\"email\":\"missing@example.com\"},\"usage\":null}}";
    poll(screen); pump(3); Doc doc; doc_init(&doc); draw(screen, &doc, 600);
    CHECK(text_with(&doc, "missing@example.com") != NULL);
    const char *quota = text_with(&doc, "Quota:"); CHECK(!quota || strchr(quota, '%') == NULL);
    const char *invalid_status[] = { "{}", "{\"status\":{\"usage\":{\"windows\":[{\"usedPct\":1e999}]}}}" };
    for (size_t i = 0; i < sizeof invalid_status / sizeof *invalid_status; i++) {
        stub.status = invalid_status[i]; poll(screen); pump(3); draw(screen, &doc, 600);
        CHECK(text_with(&doc, "Work") != NULL);
        quota = text_with(&doc, "Quota:"); CHECK(!quota || strchr(quota, '%') == NULL);
    }
    stub.accounts = "{\"providers\":[]}"; poll(screen); pump(2); draw(screen, &doc, 600);
    CHECK_INT(stub.count[STATUS_READ], 3); CHECK(text_with(&doc, "missing@example.com") == NULL);
    cleanup(&stub, &doc);
}
static void test_hidden_cancels_reads(void) {
    for (int kind = ACCOUNTS_READ; kind <= STATUS_READ; kind++) {
        DashboardStub stub = {0}; block(&stub, kind);
        Screen *screen = setup(&stub, "admin", 3); pump(kind == ACCOUNTS_READ ? 1 : 2); await_block(&stub);
        screen->vt->visible(screen, false); SetEvent(stub.resume); pump(1);
        CHECK_INT(stub.count[STATUS_READ], kind == STATUS_READ);
        Doc doc; doc_init(&doc); draw(screen, &doc, 600); CHECK(text_with(&doc, "work@example.com") == NULL);
        poll(screen); CHECK_INT(stub.count[LEDGER_READ], 1);
        screen->vt->visible(screen, true); first_poll(screen); pump(3); draw(screen, &doc, 600);
        CHECK(text_with(&doc, "work@example.com") != NULL); check_percent(&doc, "fixture-window", 75);
        cleanup(&stub, &doc);
    }
}
static void test_fresh_refresh_during_reads(void) {
    for (int kind = ACCOUNTS_READ; kind <= STATUS_READ; kind++) {
        DashboardStub stub = {0}; block(&stub, kind);
        Screen *screen = setup(&stub, "admin", 3); pump(kind == ACCOUNTS_READ ? 1 : 2); await_block(&stub);
        stub.accounts = "{\"providers\":[{\"id\":3}]}";
        stub.status = "{\"status\":{\"auth\":{\"email\":\"replacement@example.com\"},\"usage\":{\"windows\":[{\"label\":\"replacement-window\",\"usedPct\":10}]}}}";
        screen->vt->refresh(screen); pump(3);
        check_url(&stub, "https://example.com/api/v1/settings/providers/3/status?fresh=1");
        SetEvent(stub.resume); pump(1);
        CHECK_INT(stub.count[STATUS_READ], kind == ACCOUNTS_READ ? 1 : 2);
        Doc doc; doc_init(&doc); draw(screen, &doc, 600);
        CHECK(text_with(&doc, "replacement@example.com") != NULL); CHECK(text_with(&doc, "work@example.com") == NULL);
        check_percent(&doc, "replacement-window", 90);
        cleanup(&stub, &doc);
    }
}
static void test_quota_retry_after(void) {
    for (int status_failure = 0; status_failure < 3; status_failure++) {
        DashboardStub stub = {0};
        if (status_failure) {
            block(&stub, STATUS_READ); stub.block_path = "/8/status";
            stub.accounts = "{\"providers\":[{\"id\":7},{\"id\":8}]}";
            stub.status_code = 429; stub.second_status = STATUS;
            if (status_failure == 2) { stub.second_code = 429; stub.second_retry = "0.25"; }
        } else stub.accounts_code = 429;
        Screen *screen = setup(&stub, "admin", 3);
        if (status_failure) { pump(3); await_block(&stub); SetEvent(stub.resume); pump(1); }
        else pump(2);
        if (status_failure == 2) Sleep(300); // A shorter later failure must not replace the first deadline.
        LONG accounts = stub.count[ACCOUNTS_READ], statuses = stub.count[STATUS_READ];
        for (int i = 0; i < 3; i++) { poll(screen); pump(1); }
        CHECK_INT(stub.count[ACCOUNTS_READ], accounts); CHECK_INT(stub.count[STATUS_READ], statuses);
        screen->vt->visible(screen, false); screen->vt->visible(screen, true); first_poll(screen); pump(1);
        CHECK_INT(stub.count[ACCOUNTS_READ], accounts);
        // Explicit refresh can request fresh quotas despite a periodic-read cooldown.
        stub.accounts_code = 0; stub.status_code = 0; stub.second_code = 0; stub.block_path = NULL; screen->vt->refresh(screen); pump(status_failure ? 4 : 3);
        Doc doc; doc_init(&doc); draw(screen, &doc, 600); CHECK(text_with(&doc, "work@example.com") != NULL);
        cleanup(&stub, &doc);
    }
}
static void test_quota_cooldown_expiry(void) {
    DashboardStub stub = {0}; stub.accounts_code = 429; stub.retry = "0.25";
    Screen *screen = setup(&stub, "admin", 3); pump(2);
    // Use the real clock, with a short transport deadline, rather than changing screen internals.
    Sleep(300); stub.accounts_code = 0; poll(screen); pump(3);
    CHECK_INT(stub.count[ACCOUNTS_READ], 2); CHECK_INT(stub.count[STATUS_READ], 1);
    Doc doc; doc_init(&doc); draw(screen, &doc, 600); check_percent(&doc, "fixture-window", 75);
    cleanup(&stub, &doc);
}
static void test_custom_ranges_lifecycle(void) {
    DashboardStub stub = {0};
    Screen *screen = setup(&stub, "admin", 1); pump(1);
    DashboardScreen *s = (DashboardScreen *)screen;
    UsageRange a = { "2026-03-07", "2026-03-09" }, b = { "2024-02-29", "2024-02-29" };
    Json *saved = cache_value(g_store.cache, "usage:month"); CHECK(saved != NULL); json_free(saved);
    LONG reads = stub.count[LEDGER_READ];
    CHECK(!screen_dashboard_apply_range(s, NULL)); CHECK_INT(stub.count[LEDGER_READ], reads);
    CHECK_INT(s->window.period, 0); CHECK(screen_dashboard_shown(s) != NULL);
    block(&stub, LEDGER_READ); stub.block_path = "from=2026-03-07";
    stub.ledger = "{\"turns\":111,\"period\":\"custom\"}";
    CHECK(screen_dashboard_apply_range(s, &a)); await_block(&stub); CHECK(screen_dashboard_shown(s) == NULL);
    stub.ledger = "{\"turns\":222,\"period\":\"custom\",\"comparison\":{\"totalTokens\":1}}";
    CHECK(screen_dashboard_apply_range(s, &b)); pump(1);
    CHECK_INT(json_int_or(json_get(screen_dashboard_shown(s), "turns"), 0), 222);
    SetEvent(stub.resume); pump(1);
    CHECK_INT(json_int_or(json_get(screen_dashboard_shown(s), "turns"), 0), 222);
    CHECK(usage_comparison(screen_dashboard_shown(s)).present);
    check_url(&stub, "https://example.com/api/v1/usage/all?period=custom&from=2026-03-07&to=2026-03-09");
    check_url(&stub, "https://example.com/api/v1/usage/all?period=custom&from=2024-02-29&to=2024-02-29");
    saved = cache_value(g_store.cache, "usage:month"); CHECK_INT(json_int_or(json_get(saved, "turns"), -1), 0); json_free(saved);
    saved = cache_value(g_store.cache, "usage:custom"); CHECK(saved == NULL); json_free(saved);
    CHECK(screen_dashboard_apply_range(s, &b)); pump(1); CHECK_INT(stub.count[LEDGER_READ], reads + 3);
    screen_dashboard_pick_only(s, F_ACCOUNT, "7"); screen_dashboard_repick(s); pump(1);
    check_url(&stub, "https://example.com/api/v1/usage/all?period=custom&from=2024-02-29&to=2024-02-29&account=7");
    Doc doc; doc_init(&doc);
    // A newly opened view remembers the days but must not restore a custom payload.
    pane_set_root(pane, NULL); screen = dashboard_screen_new(); s = (DashboardScreen *)screen;
    pane_set_root(pane, screen); CHECK(screen_dashboard_shown(s) == NULL); first_poll(screen); pump(1);
    CHECK_INT(s->window.period, USAGE_CUSTOM); CHECK_STR(s->window.range.from, "2024-02-29");
    // Navigation destroys a pending range owner; its late answer must be discarded.
    ResetEvent(stub.entered); ResetEvent(stub.resume);
    CHECK(screen_dashboard_apply_range(s, &a)); await_block(&stub);
    pane_set_root(pane, NULL); SetEvent(stub.resume); pump(1);
    cleanup(&stub, &doc); screen_dashboard_preference = (UsageWindow){0};
}

static bool dialog_apply;
static int dialog_seen;
static VOID CALLBACK range_dialog_tick(HWND window, UINT msg, UINT_PTR timer, DWORD time) {
    (void)window; (void)msg; (void)time;
    HWND dialog = GetActiveWindow();
    if (!GetDlgItem(dialog, IDC_USAGE_FROM)) return;
    KillTimer(NULL, timer); dialog_seen++;
    SetDlgItemTextW(dialog, IDC_USAGE_FROM, L"2024-02-30");
    CHECK(!IsWindowEnabled(GetDlgItem(dialog, IDOK)));
    SetDlgItemTextW(dialog, IDC_USAGE_FROM, L"2024-12-31");
    SetDlgItemTextW(dialog, IDC_USAGE_TO, L"2024-01-01");
    CHECK(IsWindowEnabled(GetDlgItem(dialog, IDOK)));
    PostMessageW(dialog, WM_COMMAND, dialog_apply ? IDOK : IDCANCEL, 0);
}
static void test_range_dialog(void) {
    for (int apply = 0; apply < 2; apply++) {
        UsageRange range = { "2026-08-03", "2026-08-09" };
        dialog_apply = apply != 0; dialog_seen = 0;
        UINT_PTR timer = SetTimer(NULL, 0, 10, range_dialog_tick); CHECK(timer != 0);
        bool accepted = dialog_usage_range(NULL, &range); KillTimer(NULL, timer);
        CHECK_INT(dialog_seen, 1); CHECK_INT(accepted, apply);
        CHECK_STR(range.from, apply ? "2024-01-01" : "2026-08-03");
        CHECK_STR(range.to, apply ? "2024-12-31" : "2026-08-09");
    }
}

void app_dashboard_tests(void) {
    if (!theme.canvas) theme_init();
    dispatcher = CreateWindowExW(0, L"STATIC", L"Dashboard dispatcher", 0, 0, 0, 10, 10,
        NULL, NULL, GetModuleHandleW(NULL), NULL);
    CHECK(dispatcher != NULL); pane = pane_create(dispatcher, false); CHECK(pane_hwnd(pane) != NULL);
    wchar_t directory[MAX_PATH]; GetTempPathW(MAX_PATH, directory); wcscat(directory, L"briareus-dashboard-tests");
    memset(&g_store, 0, sizeof g_store); g_store.hwnd = dispatcher; g_store.cache = cache_new(directory);
    test_run("Usage date dialog validates days and preserves Cancel", test_range_dialog);
    test_run("Dashboard custom ranges read fresh, isolate caches and discard stale or destroyed owners", test_custom_ranges_lifecycle);
    test_run("Dashboard quota percentages distinguish missing data from full allowance", test_remaining_values);
    test_run("Dashboard reads each valid account once while ledger polls continue", test_account_loading);
    test_run("Dashboard quota reads respect permissions and recover from malformed or empty accounts", test_unavailable_and_permissions);
    test_run("Dashboard hiding discards in-flight account and status responses", test_hidden_cancels_reads);
    test_run("Dashboard fresh refresh replaces pending account and status reads", test_fresh_refresh_during_reads);
    test_run("Dashboard quota cooldown survives successful ledger reads and visibility changes", test_quota_retry_after);
    test_run("Dashboard quota reads resume when Retry-After expires", test_quota_cooldown_expiry);
    pane_destroy(pane); DestroyWindow(dispatcher);
    cache_remove_all(g_store.cache); cache_free(g_store.cache); RemoveDirectoryW(directory);
    memset(&g_store, 0, sizeof g_store);
}
