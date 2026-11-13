#include "screen_status_internal.h"
#include "suites.h"
#include "test.h"

// ◉ Status reads the pull requests waiting on a person's approval from the attention inbox, each with its own review
// outcome, and approves them in place.
static Route routes[] = {
    {"GET", "/projects", "read"}, {"GET", "/sessions", "read"}, {"GET", "/pulls/{number}/code-reviews", "read"},
    {"POST", "/pulls/{number}/approvals", "manage"}, {"GET", "/attention", "read"},
};
#define INBOX "{\"items\":[{\"id\":\"a1\",\"kind\":\"review-approval\",\"sessionId\":null,\"repo\":\"one/repo\",\"prNumber\":12," \
    "\"title\":\"Fix the guard\",\"summary\":\"High risk: 1 more approval\"}," \
    "{\"id\":\"q\",\"kind\":\"question\",\"sessionId\":\"s\",\"repo\":\"one/repo\",\"title\":\"Which?\"}]}"
#define REVIEW "{\"codeReviews\":[],\"reviewOutcome\":{\"outcome\":\"passed\",\"security\":{\"policy\":\"required\",\"result\":\"passed\"}," \
    "\"approvals\":{\"enabled\":true,\"result\":\"pending\",\"headSha\":\"abcdef1234567890\",\"tier\":\"high\",\"required\":1,\"valid\":0," \
    "\"missing\":1,\"reviewCurrent\":true,\"risk\":{\"reasons\":[{\"code\":\"sensitive_path\",\"tier\":\"high\",\"message\":\"migrations matched\"}]}," \
    "\"approvals\":[]}},\"reviewRounds\":{\"limit\":3,\"used\":1}}"

typedef struct {
    const char *inbox, *review, *write;
    int write_status;
    volatile LONG inbox_reads, review_reads, writes;
    char *url; Json *body;
    CRITICAL_SECTION lock;
} Stub;
static HWND dispatcher;
static Pane *pane;
static bool transport(void *ctx, const char *method, const char *url, const char *const *headers, const void *body, size_t bytes,
    int timeout, int *status, char **type, char **retry, char **response, size_t *len, char **error) {
    (void)headers; (void)timeout;
    Stub *stub = ctx; const char *answer = "[]";
    EnterCriticalSection(&stub->lock);
    *status = 200; *type = xstrdup("application/json"); *retry = NULL; *error = NULL;
    if (str_eq(method, "POST")) {
        InterlockedIncrement(&stub->writes); set_string(&stub->url, url);
        char *text = xstrndup(body, bytes); json_free(stub->body); stub->body = json_parsez(text); free(text);
        if (stub->write_status) *status = stub->write_status;
        answer = stub->write ? stub->write : "{}";
    } else if (strstr(url, "/attention")) { InterlockedIncrement(&stub->inbox_reads); answer = stub->inbox ? stub->inbox : INBOX; }
    else if (strstr(url, "/code-reviews")) { InterlockedIncrement(&stub->review_reads); answer = stub->review ? stub->review : REVIEW; }
    else if (strstr(url, "/projects")) answer = "[{\"repo\":\"one/repo\",\"label\":\"One\"}]";
    *response = xstrdup(answer); *len = strlen(answer);
    LeaveCriticalSection(&stub->lock);
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
static StatusScreen *setup(Stub *stub, const char *permission, size_t route_count, int64_t me) {
    InitializeCriticalSection(&stub->lock);
    cache_remove_all(g_store.cache); g_store.device.permission = (char *)permission; g_store.device.user_id = me;
    g_store.has_device = true; g_store.routes = routes; g_store.route_count = route_count; g_store.active = true;
    ApiError error; api_error_init(&error); ServerAddress address;
    CHECK(server_address_parse("https://example.com", &address));
    g_store.client = api_client_new(&address, "brm_aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", &error);
    server_address_free(&address); api_error_clear(&error); CHECK(g_store.client != NULL);
    api_client_set_transport(g_store.client, transport, stub);
    Screen *screen = status_screen_new(); pane_set_root(pane, screen); return (StatusScreen *)screen;
}
static void cleanup(Stub *stub) {
    pane_set_root(pane, NULL); api_client_release(g_store.client); g_store.client = NULL; g_store.device.user_id = 0;
    free(stub->url); json_free(stub->body); DeleteCriticalSection(&stub->lock);
}
static void draw(StatusScreen *s, Doc *doc) { doc_begin(doc, NULL, 900); s->base.vt->layout(&s->base, doc); doc_end(doc); }
static bool contains(Doc *d, const char *text) {
    for (size_t i = 0; i < d->count; i++) if (d->items[i].text && strstr(d->items[i].text, text)) return true;
    return false;
}
static int find(Doc *d, int action, intptr_t arg) {
    for (size_t i = 0; i < d->count; i++) if (d->items[i].action == action && d->items[i].arg == arg) return (int)i;
    return -1;
}
static void act(StatusScreen *s, int action, intptr_t arg) { s->base.vt->action(&s->base, action, arg, (POINT){0}); }

static void a_pull_waiting_on_approval_shows_its_review_and_risk(void) {
    // The inbox, projects and sessions are read on showing; then the pull request's own review.
    Stub stub = {0}; StatusScreen *s = setup(&stub, "manage", 5, 9); pump(4);
    CHECK_INT(stub.inbox_reads, 1); CHECK_INT(stub.review_reads, 1); CHECK_INT(s->approval_count, 1);
    Doc doc; doc_init(&doc); draw(s, &doc);
    CHECK(contains(&doc, "Approval needed")); CHECK(contains(&doc, "one/repo #12 \xC2\xB7 Fix the guard")); CHECK(contains(&doc, "High risk: 1 more approval"));
    CHECK(contains(&doc, "Review outcome: Passed")); CHECK(contains(&doc, "0 of 1 approval"));
    // What set the risk starts open here, and Approve is offered in place.
    CHECK(contains(&doc, "migrations matched")); CHECK(find(&doc, STATUS_APPROVALS_BASE + APPROVAL_ACT_APPROVE, 0) >= 0);
    // The title opens the pull request of a project the token sees.
    CHECK(find(&doc, STATUS_APPROVAL_PULL, 0) >= 0);
    // A poll reads the inbox again and leaves the unchanged review as it was.
    status_approvals_load(s, false); pump(1); CHECK_INT(stub.inbox_reads, 2); CHECK_INT(stub.review_reads, 1);
    // A changed summary reads the review again; F5 reads every one again.
    stub.inbox = "{\"items\":[{\"id\":\"a1\",\"kind\":\"review-approval\",\"repo\":\"one/repo\",\"prNumber\":12,\"summary\":\"Moved\"}]}";
    status_approvals_load(s, false); pump(2); CHECK_INT(stub.review_reads, 2);
    status_approvals_load(s, true); pump(2); CHECK_INT(stub.review_reads, 3);
    doc_free(&doc); cleanup(&stub);
}
static void approving_in_place_reads_the_inbox_again(void) {
    Stub stub = {0}; StatusScreen *s = setup(&stub, "manage", 5, 9); pump(4);
    Doc doc; doc_init(&doc);
    act(s, STATUS_APPROVALS_BASE + APPROVAL_ACT_APPROVE, 0); draw(s, &doc);
    CHECK(find(&doc, STATUS_APPROVALS_BASE + APPROVAL_ACT_SEND, 0) >= 0);
    stub.write = "{\"approval\":{\"id\":5},\"approvals\":{\"enabled\":true,\"result\":\"satisfied\",\"required\":1,\"valid\":1,\"missing\":0}}";
    stub.inbox = "{\"items\":[]}";
    act(s, STATUS_APPROVALS_BASE + APPROVAL_ACT_SEND, 0); pump(2);
    CHECK_INT(stub.writes, 1); CHECK(strstr(stub.url, "/api/v1/pulls/12/approvals") != NULL);
    CHECK_STR(json_str(json_get(stub.body, "headSha")), "abcdef1234567890"); CHECK_STR(json_str(json_get(stub.body, "repo")), "one/repo");
    // Once the write landed the inbox was read again, and the pull request with its approvals left the list.
    CHECK_INT(stub.inbox_reads, 2); CHECK_INT(s->approval_count, 0);
    draw(s, &doc); CHECK(!contains(&doc, "Approval needed"));
    doc_free(&doc); cleanup(&stub);
}
static void a_server_without_the_inbox_or_a_read_token_approves_nothing(void) {
    Stub stub = {0}; StatusScreen *s = setup(&stub, "manage", 4, 9); pump(2);
    CHECK_INT(stub.inbox_reads, 0); CHECK_INT(s->approval_count, 0);
    Doc doc; doc_init(&doc); draw(s, &doc); CHECK(!contains(&doc, "Approval needed"));
    doc_free(&doc); cleanup(&stub);
    memset(&stub, 0, sizeof stub); s = setup(&stub, "read", 5, 9); pump(4);
    doc_init(&doc); draw(s, &doc);
    CHECK(contains(&doc, "0 of 1 approval")); CHECK_INT(find(&doc, STATUS_APPROVALS_BASE + APPROVAL_ACT_APPROVE, 0), -1);
    doc_free(&doc); cleanup(&stub);
}
static void hiding_cancels_the_reads(void) {
    Stub stub = {0}; StatusScreen *s = setup(&stub, "manage", 5, 9); pump(4);
    status_approvals_load(s, true); CHECK(s->attention_read != NULL);
    s->base.vt->visible(&s->base, false);
    CHECK(s->attention_read == NULL); CHECK(s->approvals[0]->req == NULL);
    cleanup(&stub);
}
void app_status_approvals_tests(void) {
    if (!theme.canvas) theme_init();
    dispatcher = CreateWindowExW(0, L"STATIC", L"Status approvals dispatcher", 0, 0, 0, 10, 10, NULL, NULL, GetModuleHandleW(NULL), NULL);
    pane = pane_create(dispatcher, false);
    wchar_t directory[MAX_PATH]; GetTempPathW(MAX_PATH, directory); wcscat(directory, L"briareus-status-approvals-tests");
    memset(&g_store, 0, sizeof g_store); g_store.hwnd = dispatcher; g_store.cache = cache_new(directory);
    pane_set_root(pane, status_screen_new()); pane_set_root(pane, NULL);
    test_run("Status shows a pull request waiting on approval with its review, security pass and risk", a_pull_waiting_on_approval_shows_its_review_and_risk);
    test_run("Status approves in place and reads the inbox again", approving_in_place_reads_the_inbox_again);
    test_run("Status without the inbox route or a manage token approves nothing", a_server_without_the_inbox_or_a_read_token_approves_nothing);
    test_run("Status hiding cancels the inbox and review reads", hiding_cancels_the_reads);
    pane_destroy(pane); DestroyWindow(dispatcher); cache_remove_all(g_store.cache); cache_free(g_store.cache); RemoveDirectoryW(directory); memset(&g_store, 0, sizeof g_store);
}
