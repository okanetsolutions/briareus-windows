#include "screen_security_internal.h"
#include "suites.h"
#include "test.h"

// Two projects: o/a has a pull request whose security pass failed, one that passed and one whose project asks for no
// pass; o/b's pull requests cannot be read.
static Route routes[] = {
    { "GET", "/projects", "read" }, { "GET", "/pulls", "read" }, { "GET", "/pulls/{number}/code-review-rounds", "read" },
};
#define OUTCOME(result, policy) "{\"reviewRounds\":{\"used\":1},\"reviewOutcome\":{\"outcome\":\"pending\",\"security\":{\"policy\":\"" policy "\",\"result\":\"" result "\"," \
    "\"reasons\":[{\"code\":\"fix_required\",\"effect\":\"failed\",\"message\":\"A security finding stands as a required fix.\"}],\"counts\":{\"findings\":1,\"fix\":1}}}}"
// `hold` keeps the next outcome read waiting in the transport until `resume`, after signalling `entered`.
typedef struct { volatile LONG reads; char *last; volatile LONG hold; HANDLE entered, resume; } Stub;
static HWND dispatcher;
static bool transport(void *ctx, const char *method, const char *url, const char *const *headers, const void *body, size_t bytes,
    int timeout, int *status, char **type, char **retry, char **response, size_t *len, char **error) {
    (void)method; (void)headers; (void)body; (void)bytes; (void)timeout;
    Stub *stub = ctx; InterlockedIncrement(&stub->reads); free(stub->last); stub->last = xstrdup(url);
    const char *answer = "{}"; *status = 200;
    if (strstr(url, "/projects")) answer = "[{\"repo\":\"o/a\",\"label\":\"A\"},{\"repo\":\"o/b\",\"label\":\"B\"}]";
    else if (strstr(url, "/pulls/1/")) answer = OUTCOME("failed", "required");
    else if (strstr(url, "/pulls/2/")) answer = OUTCOME("passed", "advisory");
    else if (strstr(url, "/pulls/3/")) answer = "{\"reviewOutcome\":{\"outcome\":\"passed\",\"security\":{\"policy\":\"off\",\"result\":\"not_required\"}}}";
    else if (strstr(url, "o%2Fb") || strstr(url, "o/b")) { *status = 500; answer = "{\"error\":\"Board unavailable\"}"; }
    else answer = "{\"repo\":\"o/a\",\"pulls\":[{\"number\":2,\"title\":\"Docs\"},{\"number\":1,\"title\":\"Token in the log\"},{\"number\":3,\"title\":\"Tidy\"}],\"issues\":[]}";
    *type = xstrdup("application/json"); *retry = NULL; *error = NULL;
    *response = xstrdup(answer); *len = strlen(answer);
    if (strstr(url, "code-review-rounds") && InterlockedExchange(&stub->hold, 0)) { SetEvent(stub->entered); WaitForSingleObject(stub->resume, 10000); }
    return true;
}
static void pump(int expected) {
    int received = 0; ULONGLONG until = GetTickCount64() + 10000;
    while (received < expected && GetTickCount64() < until) {
        MSG msg;
        while (PeekMessageW(&msg, dispatcher, WM_APP_REQUEST_DONE, WM_APP_REQUEST_DONE, PM_REMOVE)) {
            store_handle_message(msg.message, msg.wParam, msg.lParam); received++;
        }
        if (received < expected) Sleep(1);
    }
    CHECK_INT(received, expected);
}
static void setup(Stub *stub, size_t route_count) {
    memset(&g_store, 0, sizeof g_store); g_store.hwnd = dispatcher;
    g_store.device.permission = "read"; g_store.device.id = "device";
    g_store.has_device = g_store.active = true; g_store.routes = routes; g_store.route_count = route_count;
    ApiError error; api_error_init(&error); ServerAddress address;
    CHECK(server_address_parse("https://example.com", &address));
    g_store.client = api_client_new(&address, "brm_aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", &error);
    server_address_free(&address); api_error_clear(&error);
    api_client_set_transport(g_store.client, transport, stub);
}
static void cleanup(Stub *stub) {
    api_client_release(g_store.client); free(stub->last); memset(&g_store, 0, sizeof g_store);
    if (stub->entered) CloseHandle(stub->entered);
    if (stub->resume) CloseHandle(stub->resume);
}
static void draw(Screen *s, Doc *d) { doc_begin(d, NULL, 900); s->vt->layout(s, d); doc_end(d); }
static int find(Doc *d, const char *text) {
    for (size_t i = 0; i < d->count; i++) if (d->items[i].text && strstr(d->items[i].text, text)) return (int)i;
    return -1;
}
static intptr_t open_arg(Doc *d, size_t nth) {
    for (size_t i = 0; i < d->count; i++) if (d->items[i].action == SECURITY_OPEN && nth-- == 0) return d->items[i].arg;
    return -1;
}

static void open_pull_requests_show_worst_first(void) {
    Stub stub = {0}; setup(&stub, 3);
    Screen *screen = security_screen_new(); SecurityScreen *s = (SecurityScreen *)screen;
    Pane *window = detached_open(screen, NULL, false); CHECK(window != NULL);
    // The projects, each project's pull requests, then each pull request's outcome, one at a time.
    pump(1 + 2 + 3); CHECK_INT(stub.reads, 6); CHECK(security_done(s)); CHECK_INT(security_read_count(s), 3);
    Doc d; doc_init(&d); draw(screen, &d);
    CHECK(find(&d, "1 failed \xC2\xB7 1 passed \xC2\xB7 1 not asked") >= 0); CHECK(find(&d, "o/b could not be read") >= 0);
    // The failed one heads the list with why; the one whose project asks for no pass is only counted.
    CHECK(find(&d, "Failed \xC2\xB7 required") >= 0); CHECK(find(&d, "1 finding \xC2\xB7 1 to fix") >= 0);
    CHECK(find(&d, "A security finding stands as a required fix.") >= 0); CHECK_INT(find(&d, "Tidy"), -1);
    CHECK(find(&d, "o/a #1 \xC2\xB7 Token in the log") < find(&d, "o/a #2 \xC2\xB7 Docs"));
    // A row opens its pull request on the Code reviews tab.
    intptr_t first = open_arg(&d, 0);
    screen->vt->action(screen, SECURITY_OPEN, first, (POINT){0});
    CHECK_STR(pane_top(window)->id, "pull:o/a#1");
    pane_pop(window); CHECK(pane_top(window) == screen);
    // A refresh reads everything again, and a click laid out for the old list does nothing.
    screen->vt->refresh(screen); pump(6); CHECK_INT(stub.reads, 12);
    screen->vt->action(screen, SECURITY_OPEN, first, (POINT){0}); CHECK(pane_top(window) == screen);
    doc_free(&d); DestroyWindow(GetAncestor(pane_hwnd(window), GA_ROOT)); cleanup(&stub);
}
static void hiding_pauses_the_scan_and_routes_gate_it(void) {
    Stub stub = {0}; setup(&stub, 3);
    stub.hold = 1; stub.entered = CreateEventW(NULL, TRUE, FALSE, NULL); stub.resume = CreateEventW(NULL, TRUE, FALSE, NULL);
    Screen *screen = security_screen_new(); SecurityScreen *s = (SecurityScreen *)screen;
    Pane *window = detached_open(screen, NULL, false); CHECK(window != NULL);
    // The projects and both lists of pull requests, then the first outcome read waits in the transport.
    pump(3); CHECK_INT(WaitForSingleObject(stub.entered, 10000), WAIT_OBJECT_0);
    CHECK_INT(s->pull_count, 3); CHECK_INT(s->next_pull, 0); CHECK(s->read != NULL);
    // Hidden, the outcome read under way is dropped and its pull request stays owed; shown again, the scan resumes there.
    screen->vt->visible(screen, false); CHECK(s->read == NULL); CHECK_INT(s->next_pull, 0);
    SetEvent(stub.resume);
    // The dropped read still completes, discarded; then the three outcomes, one after another.
    screen->vt->visible(screen, true); pump(4); CHECK(security_done(s)); CHECK_INT(security_read_count(s), 3);
    DestroyWindow(GetAncestor(pane_hwnd(window), GA_ROOT)); cleanup(&stub);
    // A server without the rounds route offers nothing and reads nothing.
    memset(&stub, 0, sizeof stub); setup(&stub, 2);
    screen = security_screen_new(); window = detached_open(screen, NULL, false); CHECK(window != NULL);
    Doc d; doc_init(&d); draw(screen, &d); CHECK(find(&d, "Reopen Security") >= 0); CHECK_INT(stub.reads, 0);
    doc_free(&d); DestroyWindow(GetAncestor(pane_hwnd(window), GA_ROOT)); cleanup(&stub);
}
void app_security_tests(void) {
    if (!theme.canvas) theme_init();
    dispatcher = CreateWindowExW(0, L"STATIC", L"Security dispatcher", 0, 0, 0, 10, 10, NULL, NULL, GetModuleHandleW(NULL), NULL);
    test_run("Security lists open pull requests worst first and opens one on Code reviews", open_pull_requests_show_worst_first);
    test_run("Security pauses its scan while hidden and needs the pulls and rounds routes", hiding_pauses_the_scan_and_routes_gate_it);
    DestroyWindow(dispatcher);
}
