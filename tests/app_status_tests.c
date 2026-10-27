#include "screen_status_internal.h"
#include "suites.h"
#include "test.h"

static Route routes[] = {
    { "GET", "/projects", "read" }, { "GET", "/sessions", "read" },
    { "GET", "/sessions/{sessionId}", "read" }, { "POST", "/sessions/{sessionId}/messages", "manage" },
};
static const char *lists = "["
    "{\"id\":\"wait\",\"repo\":\"one/repo\",\"status\":\"idle\",\"awaitingAnswer\":true},"
    "{\"id\":\"held\",\"repo\":\"one/repo\",\"status\":\"idle\",\"reviewTriage\":{\"findings\":[]}},"
    "{\"id\":\"work\",\"repo\":\"one/repo\",\"status\":\"running\"},"
    "{\"id\":\"child\",\"repo\":\"one/repo\",\"parentId\":\"work\",\"status\":\"idle\",\"awaitingAnswer\":true},"
    "{\"id\":\"other\",\"repo\":\"hidden/repo\",\"status\":\"idle\",\"awaitingAnswer\":true}]";
static const char *question = "{\"session\":{\"id\":\"wait\",\"repo\":\"one/repo\",\"status\":\"idle\",\"awaitingAnswer\":true},"
    "\"events\":[{\"seq\":1,\"kind\":\"ask\",\"question\":\"Which fixture?\",\"options\":[{\"label\":\"First fixture\"},{\"label\":\"Second fixture\"}]}]}";
typedef struct {
    const char *sessions, *answer, *projects;
    int read_status, write_status;
    volatile LONG reads, writes;
    char *body, *url, *method, *read_url;
    HANDLE entered, resume;
    CRITICAL_SECTION lock;
    bool block_write, block_sessions;
} Stub;
static HWND dispatcher;
static Pane *pane;
static bool transport(void *ctx, const char *method, const char *url, const char *const *headers, const void *body, size_t bytes,
    int timeout, int *status, char **type, char **retry, char **response, size_t *len, char **error) {
    (void)headers; (void)timeout;
    Stub *stub = ctx; const char *answer = "{}"; bool detail = false, write = str_eq(method, "POST");
    EnterCriticalSection(&stub->lock);
    *status = 200; *type = xstrdup("application/json"); *retry = NULL; *error = NULL;
    if (write) {
        InterlockedIncrement(&stub->writes); free(stub->body); stub->body = xstrndup(body, bytes);
        set_string(&stub->url, url); set_string(&stub->method, method); if (stub->write_status) *status = stub->write_status;
    } else if (strstr(url, "/projects")) answer = stub->projects ? stub->projects : "[{\"repo\":\"one/repo\",\"label\":\"One\"}]";
    else if (strstr(url, "/sessions/")) {
        detail = true; InterlockedIncrement(&stub->reads); set_string(&stub->read_url, url);
        answer = stub->answer ? stub->answer : question; if (stub->read_status) *status = stub->read_status;
    } else answer = stub->sessions ? stub->sessions : lists;
    *response = xstrdup(answer); *len = strlen(answer);
    LeaveCriticalSection(&stub->lock);
    if (stub->entered && (stub->block_sessions ? !write && !detail && !strstr(url, "/projects") : stub->block_write ? write : detail)) { SetEvent(stub->entered); WaitForSingleObject(stub->resume, 10000); }
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
static StatusScreen *setup(Stub *stub, const char *permission, size_t route_count) {
    InitializeCriticalSection(&stub->lock);
    cache_remove_all(g_store.cache); g_store.device.permission = (char *)permission;
    g_store.has_device = true; g_store.routes = routes; g_store.route_count = route_count; g_store.active = true;
    ApiError error; api_error_init(&error); ServerAddress address;
    CHECK(server_address_parse("https://example.com", &address));
    g_store.client = api_client_new(&address, "brm_aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", &error);
    server_address_free(&address); api_error_clear(&error); CHECK(g_store.client != NULL);
    api_client_set_transport(g_store.client, transport, stub);
    Screen *screen = status_screen_new(); pane_set_root(pane, screen); return (StatusScreen *)screen;
}
static void cleanup(Stub *stub) {
    pane_set_root(pane, NULL); api_client_release(g_store.client); g_store.client = NULL;
    free(stub->body); free(stub->url); free(stub->method); free(stub->read_url);
    if (stub->entered) CloseHandle(stub->entered);
    if (stub->resume) CloseHandle(stub->resume);
    DeleteCriticalSection(&stub->lock);
}
static void block(Stub *stub, bool write) {
    stub->block_write = write; stub->entered = CreateEventW(NULL, TRUE, FALSE, NULL); stub->resume = CreateEventW(NULL, TRUE, FALSE, NULL);
}
static size_t card(StatusScreen *s, const char *id) {
    for (size_t i = 0; i < s->card_count; i++) if (str_eq(session_id(&s->cards[i]->session), id)) return i;
    CHECK(false); return 0;
}
static void draw(StatusScreen *s, Doc *doc) { doc_begin(doc, NULL, 600); s->base.vt->layout(&s->base, doc); doc_end(doc); }
static intptr_t item_arg(Doc *doc, int action, const char *text) {
    for (size_t i = 0; i < doc->count; i++) if (doc->items[i].action == action && str_eq(doc->items[i].text, text)) return doc->items[i].arg;
    CHECK(false); return -1;
}
static void test_option_and_text_wire(void) {
    Stub stub = {0}; StatusScreen *s = setup(&stub, "manage", 4); pump(3);
    CHECK_INT(s->overview.waiting_count, 2); CHECK_INT(s->overview.working_count, 1); CHECK_INT(stub.reads, 1);
    CHECK_STR(stub.read_url, "https://example.com/api/v1/sessions/wait?since=0");
    Doc doc; doc_init(&doc); draw(s, &doc);
    intptr_t option = item_arg(&doc, STATUS_OPTION, "Second fixture");
    s->base.vt->action(&s->base, STATUS_OPTION, option, (POINT){0});
    s->base.vt->action(&s->base, STATUS_OPTION, option, (POINT){0}); // duplicate while preflight pending
    pump(5); CHECK_INT(stub.writes, 1); CHECK_STR(stub.method, "POST");
    CHECK_STR(stub.url, "https://example.com/api/v1/sessions/wait/messages");
    Json *body = json_parsez(stub.body); CHECK_STR(json_str(json_get(body, "text")), "Second fixture"); CHECK_INT(json_count(body), 1); json_free(body);
    CHECK(s->cards[card(s, "wait")]->answered);
    // Optional session timestamps may be absent: a different ask still resets successful-send suppression.
    stub.answer = "{\"session\":{\"id\":\"wait\",\"repo\":\"one/repo\",\"status\":\"idle\",\"awaitingAnswer\":true},\"events\":[{\"seq\":2,\"kind\":\"user\",\"text\":\"Second fixture\"},{\"seq\":3,\"kind\":\"ask\",\"text\":\"Next fixture\"}]}";
    s->next_question = 0; status_questions_next(s); pump(1);
    CHECK(!s->cards[card(s, "wait")]->answered); CHECK(status_can_answer(s, s->cards[card(s, "wait")]));
    doc_free(&doc); cleanup(&stub);

    memset(&stub, 0, sizeof stub); s = setup(&stub, "manage", 4); pump(3);
    size_t index = card(s, "wait"); set_string(&s->cards[index]->draft, "  Typed fixture  ");
    status_send(s, index, s->cards[index]->draft); pump(5);
    body = json_parsez(stub.body); CHECK_STR(json_str(json_get(body, "text")), "Typed fixture"); json_free(body);
    CHECK_STR(s->cards[index]->draft, ""); cleanup(&stub);
}
static void test_permissions_and_changed_ask(void) {
    for (int i = 0; i < 2; i++) {
        Stub stub = {0}; StatusScreen *s = setup(&stub, i ? "manage" : "read", i ? 3 : 4); pump(3);
        size_t index = card(s, "wait"); CHECK(!status_can_answer(s, s->cards[index])); status_send(s, index, "no"); CHECK(s->check_read == NULL); CHECK_INT(stub.writes, 0); cleanup(&stub);
    }
    Stub stub = {0}; StatusScreen *s = setup(&stub, "manage", 4); pump(3);
    size_t index = card(s, "wait"); set_string(&s->cards[index]->draft, "keep me");
    stub.answer = "{\"session\":{\"id\":\"wait\",\"repo\":\"one/repo\",\"status\":\"idle\",\"awaitingAnswer\":true},\"events\":[{\"seq\":2,\"kind\":\"user\",\"text\":\"answered\"},{\"seq\":3,\"kind\":\"ask\",\"text\":\"New fixture\"}]}";
    status_send(s, index, s->cards[index]->draft); pump(1); CHECK_INT(stub.writes, 0); CHECK_STR(s->cards[index]->draft, "keep me"); CHECK(s->cards[index]->error != NULL);
    CHECK_INT(transcript_pending_question(&s->cards[index]->transcript)->seq, 3);
    // A queued action from the previous layout cannot address the replacement ask.
    Doc doc; doc_init(&doc); draw(s, &doc); intptr_t old = item_arg(&doc, STATUS_SEND, "Send reply"); draw(s, &doc);
    s->base.vt->action(&s->base, STATUS_SEND, old, (POINT){0}); CHECK(s->check_read == NULL);
    doc_free(&doc); cleanup(&stub);
}
static void test_refresh_and_cached_questions(void) {
    Stub stub = {0}; StatusScreen *s = setup(&stub, "manage", 4); pump(3);
    CHECK_INT(status_waiting(s->projects, s->project_count), 2);
    CHECK_INT(findings_waiting(s->projects, s->project_count), 1);
    size_t index = card(s, "wait");
    Doc doc; doc_init(&doc); draw(s, &doc);
    intptr_t old_option = item_arg(&doc, STATUS_OPTION, "First fixture");
    // A changed ask arrives before a deferred repaint; the still-rendered old button must do nothing.
    stub.answer = "{\"session\":{\"id\":\"wait\",\"repo\":\"one/repo\",\"status\":\"idle\",\"awaitingAnswer\":true},\"events\":[{\"seq\":2,\"kind\":\"ask\",\"question\":\"Replacement fixture\",\"options\":[{\"label\":\"Different fixture\"}]}]}";
    s->next_question = 0; status_questions_next(s); pump(1);
    s->base.vt->action(&s->base, STATUS_OPTION, old_option, (POINT){0});
    CHECK(s->check_read == NULL); CHECK_INT(stub.writes, 0);
    CHECK_INT(transcript_pending_question(&s->cards[index]->transcript)->seq, 2);
    CHECK_STR(stub.read_url, "https://example.com/api/v1/sessions/wait?since=1");
    stub.sessions = "[{\"id\":\"wait\",\"repo\":\"one/repo\",\"status\":\"running\"}]";
    s->base.vt->refresh(&s->base); pump(2);
    CHECK_INT(s->overview.waiting_count, 0); CHECK_INT(s->overview.working_count, 1); CHECK_INT(s->overview.ready, 0);
    CHECK_INT(status_waiting(s->projects, s->project_count), 0); CHECK_INT(findings_waiting(s->projects, s->project_count), 0);
    CHECK(!s->cards[index]->confirmed);
    stub.sessions = "[{\"id\":\"wait\",\"repo\":\"one/repo\",\"status\":\"idle\"}]";
    s->base.vt->refresh(&s->base); pump(2);
    CHECK_INT(s->overview.working_count, 0); CHECK_INT(s->overview.ready, 1);
    // A missing project list is partial rather than an authoritative empty list.
    char *key = xstrdup("sessions:one/repo"); cache_remove(g_store.cache, key); free(key);
    status_rebuild(s); CHECK_INT(s->missing, 1); CHECK_INT(s->count, 0);
    doc_free(&doc); cleanup(&stub);
}
static void test_failures_and_uncertainty(void) {
    for (int code = 403; code <= 500; code += 97) {
        Stub stub = {0}; stub.write_status = code; StatusScreen *s = setup(&stub, "manage", 4); pump(3);
        size_t index = card(s, "wait"); set_string(&s->cards[index]->draft, "retain draft");
        status_send(s, index, s->cards[index]->draft); pump(5);
        CHECK_STR(s->cards[index]->draft, "retain draft"); CHECK(s->cards[index]->error != NULL);
        CHECK_INT(s->cards[index]->uncertain, code == 500);
        if (code == 500) { status_send(s, index, "retry"); CHECK_INT(stub.writes, 1); CHECK(s->check_read == NULL); }
        cleanup(&stub);
    }
    Stub stub = {0}; stub.read_status = 500; StatusScreen *s = setup(&stub, "manage", 4); pump(3);
    CHECK(!status_can_answer(s, s->cards[card(s, "wait")])); CHECK(s->cards[card(s, "wait")]->read_error != NULL); cleanup(&stub);
}
static void test_cancel_and_isolation(void) {
    Stub stub = {0}; block(&stub, false); StatusScreen *s = setup(&stub, "manage", 4); pump(2);
    CHECK_INT(WaitForSingleObject(stub.entered, 10000), WAIT_OBJECT_0);
    size_t index = card(s, "wait"); s->base.vt->visible(&s->base, false); SetEvent(stub.resume); pump(1);
    CHECK(!s->cards[index]->confirmed); CHECK_INT(s->cards[index]->transcript.count, 0);
    cleanup(&stub);

    memset(&stub, 0, sizeof stub); s = setup(&stub, "manage", 4); pump(3);
    index = card(s, "wait"); block(&stub, true); status_send(s, index, "once"); pump(1);
    CHECK_INT(WaitForSingleObject(stub.entered, 10000), WAIT_OBJECT_0);
    s->base.vt->visible(&s->base, false); CHECK(s->cards[index]->uncertain); SetEvent(stub.resume); pump(1);
    CHECK_INT(stub.writes, 1); cleanup(&stub);

    memset(&stub, 0, sizeof stub); s = setup(&stub, "manage", 4); pump(3);
    ApiClient *client = g_store.client;
    ServerAddress address; ApiError error; api_error_init(&error);
    CHECK(server_address_parse("https://other.example.com", &address));
    g_store.client = api_client_new(&address, "brm_aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", &error);
    server_address_free(&address); api_error_clear(&error); CHECK(g_store.client != NULL);
    CHECK(!status_current(s)); status_send(s, card(s, "wait"), "wrong server"); CHECK_INT(stub.writes, 0);
    Doc doc; doc_init(&doc); draw(s, &doc); CHECK_INT(s->action_count, 0); doc_free(&doc);
    api_client_release(g_store.client); g_store.client = client;
    stub.projects = "[]"; s->base.vt->refresh(&s->base); pump(2);
    CHECK_INT(s->overview.waiting_count, 0); CHECK_INT(s->overview.working_count, 0); CHECK(!s->cards[card(s, "wait")]->present);
    cleanup(&stub);
}
static void test_poll_preserves_reads_and_scan(void) {
    Stub stub = {0}; block(&stub, false);
    stub.sessions = "[{\"id\":\"wait\",\"repo\":\"one/repo\",\"status\":\"idle\",\"awaitingAnswer\":true},"
        "{\"id\":\"later\",\"repo\":\"one/repo\",\"status\":\"idle\",\"awaitingAnswer\":true}]";
    StatusScreen *s = setup(&stub, "manage", 4); pump(2);
    CHECK_INT(WaitForSingleObject(stub.entered, 10000), WAIT_OBJECT_0);
    Request *read = s->question_read;
    for (int i = 0; i < 2; i++) {
        status_load(s); pump(2);
        CHECK(s->question_read == read); CHECK_INT(s->next_question, 1); CHECK_INT(stub.reads, 1);
    }
    SetEvent(stub.resume); pump(2);
    CHECK(s->cards[card(s, "wait")]->confirmed);
    CHECK_INT(stub.reads, 2); CHECK_STR(stub.read_url, "https://example.com/api/v1/sessions/later?since=0");
    cleanup(&stub);

    memset(&stub, 0, sizeof stub); s = setup(&stub, "manage", 4); pump(3);
    block(&stub, false); status_send(s, card(s, "wait"), "across poll");
    CHECK_INT(WaitForSingleObject(stub.entered, 10000), WAIT_OBJECT_0);
    Request *check = s->check_read;
    status_load(s); pump(2); CHECK(s->check_read == check);
    SetEvent(stub.resume); pump(6); CHECK_INT(stub.writes, 1);
    cleanup(&stub);
}
static void test_poll_invalidates_changed_reads(void) {
    for (int preflight = 0; preflight < 2; preflight++) {
        Stub stub = {0}; StatusScreen *s;
        if (preflight) {
            s = setup(&stub, "manage", 4); pump(3); block(&stub, false);
            status_send(s, card(s, "wait"), "retain");
        } else { block(&stub, false); s = setup(&stub, "manage", 4); pump(2); }
        CHECK_INT(WaitForSingleObject(stub.entered, 10000), WAIT_OBJECT_0);
        stub.sessions = "[{\"id\":\"wait\",\"repo\":\"one/repo\",\"status\":\"running\"}]";
        status_load(s); pump(2);
        CHECK(s->question_read == NULL); CHECK(s->check_read == NULL);
        CHECK(!s->cards[card(s, "wait")]->confirmed);
        if (preflight) CHECK(s->cards[card(s, "wait")]->error != NULL);
        SetEvent(stub.resume); pump(1); CHECK_INT(stub.writes, 0); cleanup(&stub);
    }
}
static void test_live_snapshot_without_cache_write(void) {
    Stub stub = {0}; StatusScreen *s = setup(&stub, "manage", 4); pump(3);
    wchar_t directory[MAX_PATH]; GetTempPathW(MAX_PATH, directory); wcscat(directory, L"briareus-status-tests");
    char *name = cache_file_name("sessions:one/repo");
    wchar_t *wide = utf8_to_wide(name); free(name);
    wchar_t path[MAX_PATH]; swprintf(path, MAX_PATH, L"%ls\\%ls", directory, wide); free(wide);
    // Keep the old snapshot readable while denying replacement of its file.
    HANDLE lock = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    CHECK(lock != INVALID_HANDLE_VALUE);
    stub.sessions = "[{\"id\":\"wait\",\"repo\":\"one/repo\",\"status\":\"running\"},"
        "{\"id\":\"hidden\",\"repo\":\"hidden/repo\",\"status\":\"running\"}]";
    status_load(s); pump(2);
    CHECK_INT(s->overview.waiting_count, 0); CHECK_INT(s->overview.working_count, 1); CHECK_INT(s->count, 1);
    Json *saved = cache_value(g_store.cache, "sessions:one/repo");
    CHECK_INT(json_count(saved), 4); json_free(saved);
    // The projects half of the next poll must also retain the live snapshot.
    block(&stub, false); stub.block_sessions = true; status_load(s); pump(1);
    CHECK_INT(WaitForSingleObject(stub.entered, 10000), WAIT_OBJECT_0);
    CHECK_INT(s->overview.waiting_count, 0); CHECK_INT(s->overview.working_count, 1);
    SetEvent(stub.resume); pump(1);
    CloseHandle(lock); cleanup(&stub);
}
static void test_findings_replace_question_preview(void) {
    Stub stub = {0}; StatusScreen *s = setup(&stub, "manage", 4); pump(3);
    stub.sessions = "[{\"id\":\"wait\",\"repo\":\"one/repo\",\"status\":\"idle\","
        "\"reviewTriage\":{\"findings\":[]},\"lastText\":\"Current findings fixture\"}]";
    status_load(s); pump(2);
    Doc doc; doc_init(&doc); draw(s, &doc);
    bool current = false, obsolete = false;
    for (size_t i = 0; i < doc.count; i++) {
        if (str_eq(doc.items[i].text, "Current findings fixture")) current = true;
        if (str_eq(doc.items[i].text, "Which fixture?")) obsolete = true;
    }
    CHECK(current); CHECK(!obsolete); doc_free(&doc); cleanup(&stub);
}
static void test_held_navigation_and_teardown(void) {
    Stub stub = {0}; StatusScreen *s = setup(&stub, "manage", 4); pump(3);
    Screen *moved = pane_take_root(pane);
    Pane *window = detached_open(moved, NULL, false); CHECK(window != NULL); pump(3);
    Doc doc; doc_init(&doc); draw(s, &doc);
    intptr_t open = item_arg(&doc, STATUS_OPEN, "Decide findings →");
    s->base.vt->action(&s->base, STATUS_OPEN, open, (POINT){0});
    Screen *top = pane_top(window); CHECK(top != &s->base); CHECK_STR(top->id, "conversation:held");
    // The conversation's normal read is cancelled when its owning pane is destroyed.
    doc_free(&doc); DestroyWindow(GetAncestor(pane_hwnd(window), GA_ROOT)); cleanup(&stub);

    memset(&stub, 0, sizeof stub); block(&stub, false); s = setup(&stub, "manage", 4); pump(2);
    CHECK_INT(WaitForSingleObject(stub.entered, 10000), WAIT_OBJECT_0); pane_set_root(pane, NULL); SetEvent(stub.resume); pump(1); cleanup(&stub);
}
void app_status_tests(void) {
    if (!theme.canvas) theme_init();
    dispatcher = CreateWindowExW(0, L"STATIC", L"Status dispatcher", 0, 0, 0, 10, 10, NULL, NULL, GetModuleHandleW(NULL), NULL);
    pane = pane_create(dispatcher, false);
    wchar_t directory[MAX_PATH]; GetTempPathW(MAX_PATH, directory); wcscat(directory, L"briareus-status-tests");
    memset(&g_store, 0, sizeof g_store); g_store.hwnd = dispatcher; g_store.cache = cache_new(directory);
    // Pane stack capacity and the detached-window registry outlive individual tests, as in the pull-screen fixture.
    pane_set_root(pane, status_screen_new()); pane_set_root(pane, NULL);
    Pane *warm = detached_open(status_screen_new(), NULL, false); CHECK(warm != NULL);
    if (warm) DestroyWindow(GetAncestor(pane_hwnd(warm), GA_ROOT));
    test_run("Status option/text sends use message contract and suppress duplicates", test_option_and_text_wire);
    test_run("Status respects catalog/token and rejects changed questions and stale actions", test_permissions_and_changed_ask);
    test_run("Status refreshes counts, isolates badges and rejects pre-paint stale actions", test_refresh_and_cached_questions);
    test_run("Status retains drafts on refusal and blocks uncertain sends", test_failures_and_uncertainty);
    test_run("Status cancels obsolete reads and isolates server/project snapshots", test_cancel_and_isolation);
    test_run("Status polling preserves slow reads, scan progress and send preflights", test_poll_preserves_reads_and_scan);
    test_run("Status polling cancels reads for changed identities", test_poll_invalidates_changed_reads);
    test_run("Status live snapshots survive failed cache writes and remain scoped", test_live_snapshot_without_cache_write);
    test_run("Status findings replace an answered question preview", test_findings_replace_question_preview);
    test_run("Status held findings open their conversation and teardown cancels reads", test_held_navigation_and_teardown);
    pane_destroy(pane); DestroyWindow(dispatcher); cache_remove_all(g_store.cache); cache_free(g_store.cache); RemoveDirectoryW(directory); memset(&g_store, 0, sizeof g_store);
}
