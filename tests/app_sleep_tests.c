#include "../app/sleep_internal.h"
#include "screen_fixture.h"
#include "suites.h"
#include "test.h"
#include <stdlib.h>

static ULONGLONG fake_time;
static ULONGLONG WINAPI sleep_test_clock(void) { return fake_time; }
static Json *queued;
static int followed, cancelled, released, saved_minutes = 5;
struct SleepFollow { int unused; };
static SleepFollow fake_follow;
static SleepFollow *test_follow_new(void) { followed++; return &fake_follow; }
static void test_follow_cancel(SleepFollow *f) { if (f) cancelled++; }
static void test_follow_release(SleepFollow *f) { if (f) released++; }
static Json *test_follow_drain(SleepFollow *f, bool *ended, int *delay) {
    (void)f; *ended = false; *delay = 3000; Json *j = queued; queued = NULL; return j ? j : json_array();
}
static int test_read_sleep(void) { return saved_minutes; }
static void test_write_sleep(int minutes) { saved_minutes = minutes; }
// Compile the controller against a deterministic clock and fake stream, retaining real store request delivery.
#define GetTickCount64 sleep_test_clock
#define settings_read_sleep test_read_sleep
#define settings_write_sleep test_write_sleep
#define sleep_follow_new test_follow_new
#define sleep_follow_cancel test_follow_cancel
#define sleep_follow_release test_follow_release
#define sleep_follow_drain test_follow_drain
#define sleep_start test_sleep_start
#define sleep_stop test_sleep_stop
#define sleep_scope_changed test_sleep_scope_changed
#define sleep_deactivated test_sleep_deactivated
#define sleep_modal test_sleep_modal
#define sleep_after test_sleep_after
#define sleep_set_after test_sleep_set_after
#define sleep_settings_action test_sleep_settings_action
#define sleep_pulse test_sleep_pulse
void test_sleep_start(HWND main);
void test_sleep_stop(void);
void test_sleep_scope_changed(void);
void test_sleep_modal(bool begin);
void test_sleep_deactivated(void);
int test_sleep_after(void);
void test_sleep_set_after(int minutes);
void test_sleep_settings_action(void);
void test_sleep_pulse(uint64_t now);
#include "../app/sleep.c"
#undef GetTickCount64

static Route routes[] = {{"GET", "/events", "read"}, {"GET", "/repo/file", "read"}, {"GET", "/sessions/{id}", "read"}};
static void init(ScreenFixture *f) {
    fake_time = 100; followed = cancelled = released = 0; saved_minutes = 5;
    screen_fixture_init(f, routes, 1); // no snapshot/history/file capability until explicitly enabled
    f->parent = CreateWindowExW(0, L"STATIC", L"Sleep test", WS_POPUP, 0, 0, 900, 700, NULL, NULL, GetModuleHandleW(NULL), NULL);
    test_sleep_start(f->parent); CHECK(g_sleep.window != NULL);
}
static void pulse(ULONGLONG time) { fake_time = time; test_sleep_pulse(time); }
static void event(const char *type, const char *data) {
    if (!queued) queued = json_array();
    Json *item = json_object(); json_set_str(item, "type", type); json_object_set(item, "data", json_parsez(data)); json_array_push(queued, item);
}
static void clear(ScreenFixture *f) {
    test_sleep_stop(); json_free(queued); queued = NULL; screen_fixture_clear(f);
}
static void idle_modal_wake_and_reset(void) {
    ScreenFixture f; init(&f);
    HWND edit = CreateWindowExW(0, L"EDIT", L"unsent draft", WS_CHILD, 0, 0, 100, 30, f.parent, NULL, GetModuleHandleW(NULL), NULL);
    pulse(300099); CHECK_INT(followed, 0); CHECK(!g_sleep.idle.asleep);
    test_sleep_modal(true); pulse(700000); CHECK(!g_sleep.idle.asleep);
    test_sleep_modal(false); pulse(999999); CHECK(!g_sleep.idle.asleep); pulse(1000000); CHECK(g_sleep.idle.asleep); CHECK_INT(followed, 1);
    CHECK_INT(f.reads, 0); // fake stream only; no unsupported reads
    PostMessageW(edit, WM_KEYDOWN, 'A', 0); MSG msg;
    CHECK(PeekMessageW(&msg, edit, WM_KEYDOWN, WM_KEYDOWN, PM_REMOVE)); CHECK_INT(msg.message, WM_NULL);
    CHECK(!g_sleep.idle.asleep); CHECK_INT(cancelled, 1);
    PostMessageW(edit, WM_CHAR, 'a', 0); CHECK(PeekMessageW(&msg, edit, WM_CHAR, WM_CHAR, PM_REMOVE)); CHECK_INT(msg.message, WM_NULL);
    fake_time += 1000; PostMessageW(edit, WM_KEYUP, 'A', 0); CHECK(PeekMessageW(&msg, edit, WM_KEYUP, WM_KEYUP, PM_REMOVE)); CHECK_INT(msg.message, WM_NULL);
    wchar_t text[32]; GetWindowTextW(edit, text, 32); CHECK(!wcscmp(text, L"unsent draft"));
    pulse(fake_time + 300000); CHECK(g_sleep.idle.asleep);
    PostMessageW(edit, WM_LBUTTONDOWN, 0, 0); CHECK(PeekMessageW(&msg, edit, WM_LBUTTONDOWN, WM_LBUTTONDOWN, PM_REMOVE)); CHECK_INT(msg.message, WM_NULL);
    fake_time += 1000; PostMessageW(edit, WM_LBUTTONUP, 0, 0); CHECK(PeekMessageW(&msg, edit, WM_LBUTTONUP, WM_LBUTTONUP, PM_REMOVE)); CHECK_INT(msg.message, WM_NULL);
    test_sleep_set_after(0); CHECK_INT(saved_minutes, 0); pulse(fake_time + 9999999); CHECK(!g_sleep.idle.asleep);
    test_sleep_set_after(1); pulse(fake_time + 60000); CHECK(g_sleep.idle.asleep);
    test_sleep_modal(true); CHECK(!g_sleep.idle.asleep); test_sleep_modal(false);
    clear(&f); CHECK_INT(cancelled, released);
}
static void transitions_and_stale_requests(void) {
    ScreenFixture f; init(&f); pulse(300100);
    event("session", "{\"id\":\"s\",\"repo\":\"o/r\",\"status\":\"running\",\"branch\":\"dev\",\"updatedAt\":\"2026-10-29T10:00:00Z\"}");
    event("transcript", "{\"sessionId\":\"s\",\"event\":{\"seq\":2,\"kind\":\"tool\",\"name\":\"Edit\",\"summary\":\"a.c\"}}");
    pulse(300180); int i = -1; for (int k = 0; k < SLEEP_PANES; k++) if (str_eq(session_id(&g_sleep.wall.panes[k].session), "s")) i = k; CHECK(i >= 0); if (i < 0) { clear(&f); return; } CHECK(g_sleep.wall.panes[i].loaded); // unsupported file is quiet unavailable
    event("transcript", "{\"sessionId\":\"s\",\"event\":{\"seq\":1,\"kind\":\"cmd\",\"text\":\"stale\"}}");
    event("session", "{\"id\":\"s\",\"repo\":\"o/r\",\"status\":\"idle\",\"branch\":\"old\",\"updatedAt\":\"2026-10-29T09:00:00Z\"}");
    pulse(300260); CHECK_INT(g_sleep.wall.commands, 0); CHECK_STR(json_str(json_get(g_sleep.wall.panes[i].session.raw, "branch")), "dev");
    g_store.route_count = 2; f.block_read = true;
    event("transcript", "{\"sessionId\":\"s\",\"event\":{\"seq\":3,\"kind\":\"tool\",\"name\":\"Edit\",\"summary\":\"b.c\"}}");
    pulse(300340); CHECK_INT(WaitForSingleObject(f.entered, 10000), WAIT_OBJECT_0);
    g_store.device.id = "new-device"; test_sleep_scope_changed(); CHECK(!g_sleep.idle.asleep);
    f.read_response = "{\"path\":\"b.c\",\"content\":\"stale\"}"; SetEvent(f.resume); screen_fixture_pump(&f, 1);
    CHECK(g_sleep.wall.panes[i].session.raw == NULL);
    pulse(600340); CHECK(g_sleep.idle.asleep);
    g_store.route_count = 1;
    event("session", "{\"id\":\"new-session\",\"repo\":\"o/r\",\"status\":\"running\"}");
    event("transcript", "{\"sessionId\":\"new-session\",\"event\":{\"seq\":1,\"kind\":\"cmd\",\"text\":\"fresh\"}}");
    pulse(600420); CHECK_INT(g_sleep.wall.commands, 1); CHECK_STR(g_sleep.wall.ticker[0].text, "fresh");
    g_store.client = NULL; test_sleep_scope_changed(); CHECK(!g_sleep.idle.asleep); g_store.client = f.client;
    clear(&f);
}
typedef struct { int files, snapshots, histories; bool missing; char *urls[8]; } SleepHttp;
static bool sleep_http(void *ctx, const char *method, const char *url, const char *const *headers,
    const void *body, size_t bytes, int timeout, int *status, char **type, char **retry,
    char **response, size_t *len, char **error) {
    (void)headers; (void)body; (void)bytes; (void)timeout;
    SleepHttp *http = ctx; CHECK_STR(method, "GET"); *type = xstrdup("application/json"); *retry = NULL; *error = NULL; *status = 200;
    const char *answer;
    if (strstr(url, "/repo/file?")) {
        if (http->files < 8) http->urls[http->files] = xstrdup(url);
        http->files++; *status = strstr(url, "ref=") || http->missing ? 404 : 200;
        answer = *status == 404 ? "{\"error\":\"missing\"}" : "{\"path\":\"a.c\",\"content\":\"int value;\"}";
    } else if (strstr(url, "/sessions/s?")) {
        http->histories++; answer = "{\"events\":[{\"seq\":1,\"kind\":\"cmd\",\"text\":\"make test\"},{\"seq\":2,\"kind\":\"tool\",\"name\":\"Edit\",\"summary\":\"a.c\"}]}";
    } else {
        http->snapshots++; answer = "{\"sessions\":[{\"id\":\"s\",\"repo\":\"o/r\",\"status\":\"idle\",\"branch\":\"dev/branch\"}]}";
    }
    *response = xstrdup(answer); *len = strlen(answer); return true;
}
static void replay_file_fallback_and_live_takeover(void) {
    ScreenFixture f; init(&f); SleepHttp http = {0};
    Route all[] = {{"GET", "/events", "read"}, {"GET", "/repo/file", "read"}, {"GET", "/sessions/{id}", "read"}, {"GET", "/sessions", "read"}};
    g_store.routes = all; g_store.route_count = 4; api_client_set_transport(f.client, sleep_http, &http);
    pulse(300100); screen_fixture_pump(&f, 1); CHECK_INT(http.snapshots, 1);
    pulse(300180); screen_fixture_pump(&f, 1); CHECK_INT(http.histories, 1);
    pulse(302500); CHECK_INT(g_sleep.wall.commands, 1); CHECK(g_sleep.wall.ticker[0].replay); CHECK_STR(g_sleep.wall.ticker[0].text, "make test");
    pulse(304900); pulse(304980); screen_fixture_pump(&f, 2); CHECK_INT(http.files, 2);
    CHECK(strstr(http.urls[0], "ref=dev%2Fbranch") != NULL); CHECK(strstr(http.urls[1], "ref=") == NULL);
    int i = -1; for (int k = 0; k < 4; k++) if (g_sleep.wall.panes[k].session.raw) i = k;
    CHECK(i >= 0); if (i < 0) { clear(&f); return; } CHECK_STR(g_sleep.wall.panes[i].content, "int value;");
    event("transcript", "{\"sessionId\":\"s\",\"event\":{\"seq\":1,\"kind\":\"cmd\",\"text\":\"duplicate\"}}");
    pulse(305000); CHECK(g_sleep.wall.panes[i].replay); CHECK_INT(g_sleep.wall.commands, 1);
    event("transcript", "{\"sessionId\":\"s\",\"event\":{\"seq\":3,\"kind\":\"cmd\",\"text\":\"live test\"}}");
    pulse(305060); screen_fixture_pump(&f, 1); CHECK(!g_sleep.wall.panes[i].replay); CHECK_STR(g_sleep.wall.panes[i].command, "live test");
    // Changing the branch invalidates the current file and history identity.
    event("session", "{\"id\":\"s\",\"repo\":\"o/r\",\"status\":\"running\",\"branch\":\"new\"}");
    pulse(305140); screen_fixture_pump(&f, 1); CHECK(g_sleep.wall.panes[i].path != NULL);
    http.missing = true; pulse(305220); screen_fixture_pump(&f, 2); CHECK_INT(http.files, 4); CHECK(g_sleep.wall.panes[i].content == NULL); CHECK(g_sleep.wall.panes[i].loaded);
    pulse(305300); CHECK_INT(http.files, 4); // missing default is terminal for this file
    clear(&f); for (int k = 0; k < 8; k++) free(http.urls[k]);
}

static void record_delete_then_add(void) {
    ScreenFixture f; init(&f); pulse(300100);
    event("session", "{\"id\":\"a\",\"repo\":\"o/r\",\"status\":\"idle\"}");
    event("session", "{\"id\":\"b\",\"repo\":\"o/r\",\"status\":\"idle\"}");
    event("session.deleted", "{\"id\":\"a\"}");
    event("session", "{\"id\":\"c\",\"repo\":\"o/r\",\"status\":\"idle\"}");
    event("transcript", "{\"sessionId\":\"b\",\"event\":{\"seq\":1,\"kind\":\"cmd\",\"text\":\"kept\"}}");
    pulse(300180); CHECK_INT(g_sleep.record_count, 2); CHECK_INT(g_sleep.wall.commands, 1); CHECK_STR(g_sleep.wall.ticker[0].text, "kept"); clear(&f);
}
void app_sleep_tests(void) {
    test_run("sleep record deletion transfers ownership before reuse", record_delete_then_add);
    test_run("sleep replay branch fallback live takeover and missing files", replay_file_fallback_and_live_takeover);
    test_run("sleep native wake suppression settings modal lifecycle", idle_modal_wake_and_reset);
    test_run("sleep stale events device transitions and cancelled callbacks", transitions_and_stale_requests);
}
