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
    int read_status, write_status, blocked_read;
    const char *retry_after, *write_retry_after;
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
        if (stub->write_retry_after) *retry = xstrdup(stub->write_retry_after);
    } else if (strstr(url, "/projects")) answer = stub->projects ? stub->projects : "[{\"repo\":\"one/repo\",\"label\":\"One\"}]";
    else if (strstr(url, "/sessions/")) {
        detail = true; InterlockedIncrement(&stub->reads); set_string(&stub->read_url, url);
        answer = stub->answer ? stub->answer : question; if (stub->read_status) *status = stub->read_status;
        if (stub->retry_after) *retry = xstrdup(stub->retry_after);
    } else answer = stub->sessions ? stub->sessions : lists;
    *response = xstrdup(answer); *len = strlen(answer);
    bool block_detail = detail && (!stub->blocked_read || stub->reads == stub->blocked_read);
    LeaveCriticalSection(&stub->lock);
    if (stub->entered && (stub->block_sessions ? !write && !detail && !strstr(url, "/projects") : stub->block_write ? write : block_detail)) { SetEvent(stub->entered); WaitForSingleObject(stub->resume, 10000); }
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
    g_store.status_detail_retry_until = 0; g_store.status_message_retry_until = 0;
    pane_set_root(pane, NULL); json_free(g_store.status_uncertain); g_store.status_uncertain = NULL; api_client_release(g_store.client); g_store.client = NULL;
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
static intptr_t card_action_arg(StatusScreen *s, Doc *doc, int action, size_t index) {
    for (size_t i = 0; i < doc->count; i++) {
        Item *item = &doc->items[i];
        if (item->action != action || item->arg < s->action_first) continue;
        size_t offset = (size_t)(item->arg - s->action_first);
        if (offset < s->action_count && s->actions[offset].card == index) return item->arg;
    }
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
static void test_blank_reply_and_long_option(void) {
    const char *label = "Apply the proposed changes to the preview and export, including the extended description, then keep the remaining work in separate pull requests. Preserve punctuation: ≥ 70%, max. 10.";
    Json *payload = json_parsez(question);
    Json *choice = json_object(); json_set_str(choice, "label", label);
    Json *options = json_array(); json_array_push(options, choice);
    Json *event = json_clone(json_at(json_get(payload, "events"), 0));
    json_object_set(event, "options", options);
    Json *events = json_array(); json_array_push(events, event); json_object_set(payload, "events", events);
    char *answer = json_serialize(payload, false); json_free(payload);
    Stub stub = {.answer = answer}; StatusScreen *s = setup(&stub, "manage", 4); pump(3);
    StatusCard *c = s->cards[card(s, "wait")];
    set_string(&c->draft, "  \t  ");
    Doc doc; doc_init(&doc); draw(s, &doc);
    for (size_t i = 0; i < doc.count; i++) CHECK(doc.items[i].action != STATUS_SEND);
    status_send(s, card(s, "wait"), c->draft); CHECK(s->check_read == NULL); CHECK_INT(stub.writes, 0);
    doc_begin(&doc, NULL, 360); s->base.vt->layout(&s->base, &doc); doc_end(&doc);
    intptr_t option = item_arg(&doc, STATUS_OPTION, label);
    s->base.vt->action(&s->base, STATUS_OPTION, option, (POINT){0}); pump(5);
    Json *body = json_parsez(stub.body); CHECK_STR(json_str(json_get(body, "text")), label); json_free(body);
    doc_free(&doc); cleanup(&stub); free(answer);
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
    Doc doc; doc_init(&doc); draw(s, &doc); intptr_t old = card_action_arg(s, &doc, STATUS_SEND, index); draw(s, &doc);
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
        CHECK_INT(status_uncertain(s, s->cards[index]), code == 500);
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
    s->base.vt->visible(&s->base, false); CHECK(status_uncertain(s, s->cards[index])); SetEvent(stub.resume); pump(1);
    CHECK_INT(stub.writes, 1); cleanup(&stub);

    memset(&stub, 0, sizeof stub); s = setup(&stub, "manage", 4); pump(3);
    ApiClient *client = g_store.client;
    ServerAddress address; ApiError error; api_error_init(&error);
    CHECK(server_address_parse("https://other.example.com", &address));
    g_store.client = api_client_new(&address, "brm_aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", &error);
    server_address_free(&address); api_error_clear(&error); CHECK(g_store.client != NULL);
    CHECK(!status_current(s));
    status_set_uncertain(s, s->cards[card(s, "wait")], true); CHECK(g_store.status_uncertain == NULL);
    status_send(s, card(s, "wait"), "wrong server"); CHECK_INT(stub.writes, 0);
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
    block(&stub, false); stub.blocked_read = 2; status_send(s, card(s, "wait"), "across poll");
    CHECK_INT(WaitForSingleObject(stub.entered, 10000), WAIT_OBJECT_0);
    Request *check = s->check_read;
    // Let the poll's background detail finish before releasing the preflight. Otherwise its
    // cancellation races scan completion and the following poll can need one fewer read.
    status_load(s); pump(3); CHECK(s->check_read == check); CHECK(s->question_read == NULL);
    SetEvent(stub.resume); pump(5); CHECK_INT(stub.writes, 1);
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
static void test_detail_retry_after(void) {
    Stub stub = {0}; stub.read_status = 429; stub.retry_after = "120";
    stub.sessions = "[{\"id\":\"wait\",\"repo\":\"one/repo\",\"status\":\"idle\",\"awaitingAnswer\":true},"
        "{\"id\":\"later\",\"repo\":\"one/repo\",\"status\":\"idle\",\"awaitingAnswer\":true}]";
    StatusScreen *s = setup(&stub, "manage", 4); pump(3);
    CHECK(g_store.status_detail_retry_until > (double)GetTickCount64() + 119000); CHECK(s->question_read == NULL);
    double deadline = g_store.status_detail_retry_until;
    status_load(s); pump(2); CHECK(s->question_read == NULL); CHECK_INT(stub.reads, 1);
    CHECK(g_store.status_detail_retry_until == deadline);
    s->base.vt->refresh(&s->base); pump(2); CHECK_INT(stub.reads, 1);
    pane_set_root(pane, NULL);
    s = (StatusScreen *)status_screen_new(); pane_set_root(pane, &s->base); pump(2);
    CHECK(g_store.status_detail_retry_until == deadline); CHECK(s->question_read == NULL); CHECK_INT(stub.reads, 1);
    status_send(s, card(s, "wait"), "blocked after reopening"); CHECK(s->check_read == NULL); CHECK_INT(stub.writes, 0);
    stub.read_status = 0; stub.retry_after = NULL; g_store.status_detail_retry_until = (double)GetTickCount64();
    status_questions_next(s); pump(2); CHECK_INT(stub.reads, 3);
    // A throttled send preflight also holds both manual sends and the background scan.
    stub.read_status = 429; stub.retry_after = "120";
    status_send(s, card(s, "wait"), "retain"); pump(1);
    CHECK_INT(stub.writes, 0); CHECK(g_store.status_detail_retry_until > (double)GetTickCount64() + 119000);
    status_send(s, card(s, "wait"), "retain"); CHECK(s->check_read == NULL);
    status_load(s); pump(2); CHECK(s->question_read == NULL); CHECK_INT(stub.reads, 4);
    deadline = g_store.status_detail_retry_until;
    pane_set_root(pane, NULL);
    s = (StatusScreen *)status_screen_new(); pane_set_root(pane, &s->base); pump(2);
    CHECK(g_store.status_detail_retry_until == deadline); CHECK(s->question_read == NULL); CHECK_INT(stub.reads, 4);
    status_send(s, card(s, "wait"), "blocked after preflight refusal"); CHECK(s->check_read == NULL); CHECK_INT(stub.writes, 0);
    cleanup(&stub);
}
static void test_message_retry_after(void) {
    Stub stub = {0}; stub.write_status = 429; stub.write_retry_after = "120";
    StatusScreen *s = setup(&stub, "manage", 4); pump(3);
    size_t index = card(s, "wait"); StatusCard *c = s->cards[index];
    set_string(&c->draft, "retain reply");
    status_send(s, index, c->draft); pump(5);
    CHECK_INT(stub.writes, 1); CHECK(!status_uncertain(s, c)); CHECK(c->error != NULL);
    CHECK_STR(c->draft, "retain reply"); CHECK(c->confirmed);
    CHECK(g_store.status_message_retry_until > (double)GetTickCount64() + 119000);
    double deadline = g_store.status_message_retry_until;
    CHECK(!status_can_answer(s, c));
    status_send(s, index, c->draft); CHECK(s->check_read == NULL);
    status_load(s); pump(3);
    s->base.vt->refresh(&s->base); pump(3);
    s->base.vt->visible(&s->base, false); s->base.vt->visible(&s->base, true); pump(3);
    CHECK(g_store.status_message_retry_until == deadline); CHECK(c->confirmed);
    CHECK(!status_can_answer(s, c)); CHECK_STR(c->draft, "retain reply");
    status_send(s, index, c->draft); CHECK(s->check_read == NULL); CHECK_INT(stub.writes, 1);
    pane_set_root(pane, NULL);
    s = (StatusScreen *)status_screen_new(); pane_set_root(pane, &s->base); pump(3);
    index = card(s, "wait"); c = s->cards[index]; set_string(&c->draft, "explicit retry");
    CHECK(g_store.status_message_retry_until == deadline); CHECK(c->confirmed); CHECK(!status_uncertain(s, c));
    CHECK(!status_can_answer(s, c));
    status_send(s, index, c->draft); CHECK(s->check_read == NULL); CHECK_INT(stub.writes, 1);
    // Expiring the deadline permits an explicit retry, without automatically resending.
    stub.write_status = 0; stub.write_retry_after = NULL;
    g_store.status_message_retry_until = (double)GetTickCount64();
    CHECK(status_can_answer(s, c)); CHECK_INT(stub.writes, 1);
    status_send(s, index, c->draft); pump(5);
    CHECK_INT(stub.writes, 2); CHECK(c->answered); CHECK_STR(c->draft, "");
    cleanup(&stub);
}
static void test_delayed_detail_preserves_shared_events(void) {
    Stub stub = {0}; StatusScreen *s = setup(&stub, "manage", 4); pump(3);
    block(&stub, false); s->next_question = 0; status_questions_next(s);
    CHECK_INT(WaitForSingleObject(stub.entered, 10000), WAIT_OBJECT_0);
    Json *newer = json_parsez("[{\"seq\":2,\"kind\":\"user\",\"text\":\"Concurrent answer\"}]");
    CHECK(cache_append(g_store.cache, newer, "transcript:wait")); json_free(newer);
    SetEvent(stub.resume); pump(1);
    Json *saved = cache_lines(g_store.cache, "transcript:wait");
    Transcript reopened = {0}; transcript_append(&reopened, saved); json_free(saved);
    CHECK_INT(reopened.count, 2); CHECK_INT(reopened.cursor, 2);
    CHECK_STR(reopened.events[1].text, "Concurrent answer");
    CHECK(transcript_pending_question(&reopened) == NULL);
    transcript_free(&reopened); cleanup(&stub);
}
static void test_transcript_persistence_retry(void) {
    for (int empty = 0; empty < 2; empty++) {
        Stub stub = {0}; StatusScreen *s = setup(&stub, "manage", 4); pump(3);
        wchar_t directory[MAX_PATH]; GetTempPathW(MAX_PATH, directory); wcscat(directory, L"briareus-status-tests");
        char *name = cache_file_name("transcript:wait");
        wchar_t *wide = utf8_to_wide(name); free(name);
        wchar_t path[MAX_PATH]; swprintf(path, MAX_PATH, L"%ls\\%ls", directory, wide); free(wide);
        HANDLE lock = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        CHECK(lock != INVALID_HANDLE_VALUE);
        stub.answer = "{\"session\":{\"id\":\"wait\",\"repo\":\"one/repo\",\"status\":\"idle\",\"awaitingAnswer\":true},"
            "\"events\":[{\"seq\":2,\"kind\":\"user\",\"text\":\"First answer\"},{\"seq\":3,\"kind\":\"ask\",\"text\":\"Next ask\"}]}";
        s->next_question = 0; status_questions_next(s); pump(1);
        CHECK_INT(s->cards[card(s, "wait")]->transcript.cursor, 3);
        Json *saved = cache_lines(g_store.cache, "transcript:wait"); CHECK_INT(json_count(saved), 1); json_free(saved);
        stub.answer = "{\"session\":{\"id\":\"wait\",\"repo\":\"one/repo\",\"status\":\"idle\",\"awaitingAnswer\":true},\"events\":[]}";
        s->next_question = 0; status_questions_next(s); pump(1);
        CHECK_STR(stub.read_url, "https://example.com/api/v1/sessions/wait?since=3");
        CloseHandle(lock);
        // Recovery must retain a newer event written by another view as well as the failed batch.
        Json *newer = json_parsez("[{\"seq\":6,\"kind\":\"user\",\"text\":\"Concurrent answer\"}]");
        CHECK(cache_append(g_store.cache, newer, "transcript:wait")); json_free(newer);
        if (!empty) stub.answer = "{\"session\":{\"id\":\"wait\",\"repo\":\"one/repo\",\"status\":\"idle\",\"awaitingAnswer\":true},"
            "\"events\":[{\"seq\":4,\"kind\":\"user\",\"text\":\"Later answer\"},{\"seq\":5,\"kind\":\"ask\",\"text\":\"Later ask\"}]}";
        s->next_question = 0; status_questions_next(s); pump(1);
        saved = cache_lines(g_store.cache, "transcript:wait");
        Transcript reopened = {0}; transcript_append(&reopened, saved); json_free(saved);
        CHECK_INT(reopened.count, empty ? 4 : 6); CHECK_INT(reopened.cursor, 6);
        CHECK_INT(reopened.events[1].seq, 2); CHECK_STR(reopened.events[1].text, "First answer");
        CHECK_INT(reopened.events[2].seq, 3); CHECK_STR(reopened.events[2].text, "Next ask");
        CHECK_STR(reopened.events[reopened.count - 1].text, "Concurrent answer");
        if (!empty) { CHECK_INT(reopened.events[3].seq, 4); CHECK_INT(reopened.events[4].seq, 5); }
        CHECK(transcript_pending_question(&reopened) == NULL);
        transcript_free(&reopened); cleanup(&stub);
    }
}
static Item *card_item(StatusScreen *s, Doc *doc, int action, size_t index) {
    intptr_t arg = card_action_arg(s, doc, action, index);
    for (size_t i = 0; i < doc->count; i++) if (doc->items[i].arg == arg && doc->items[i].action == action) return &doc->items[i];
    return NULL;
}
static void test_responsive_layout_and_controls(void) {
    Stub stub = {0}; StatusScreen *s = setup(&stub, "manage", 4); pump(3);
    ShowWindow(dispatcher, SW_SHOWNA);
    StatusCard *c = s->cards[card(s, "wait")];
    set_string(&c->draft, "Retain this draft across reflow");
    const char *long_title = "Review the complete release plan, including keyboard navigation, documentation and accessibility improvements across the desktop application";
    json_set_str(c->session.raw, "title", long_title);
    const char *long_option = "Use the complete proposed release plan, including the extended documentation and the accessibility improvements, and preserve all of this answer when the layout wraps across multiple lines in a split pane.";
    Json *event = json_clone(transcript_pending_question(&c->transcript)->raw);
    Json *option_data = json_object(); json_set_str(option_data, "label", long_option);
    Json *options = json_array(); json_array_push(options, option_data);
    json_object_set(event, "options", options);
    Json *events = json_array(); json_array_push(events, event);
    transcript_free(&c->transcript); transcript_append(&c->transcript, events); json_free(events);
    Doc doc; doc_init(&doc); int dpi = theme_dpi();
    for (int scale = 1; scale <= 2; scale++) {
        theme_set_dpi(dpi * scale);
        int wide_option_height = 0, wide_option_width = 0, wide_title_height = 0;
        for (int mode = 0; mode < 3; mode++) {
            bool narrow = mode == 1;
            int width = px(mode == 2 ? 2400 : narrow ? 390 : 1440);
            RECT bounds = {0, 0, width + px(36), px(1200)}; pane_set_bounds(pane, &bounds);
            doc_begin(&doc, NULL, width); status_layout(&s->base, &doc); doc_end(&doc);
            Item *work = card_item(s, &doc, STATUS_OPEN, card(s, "work"));
            Item *wait = card_item(s, &doc, STATUS_OPEN, card(s, "wait"));
            CHECK(work != NULL); CHECK(wait != NULL);
            if (!work || !wait) continue;
            CHECK_STR(wait->text, long_title);
            int title_height = wait->rc.bottom - wait->rc.top;
            if (narrow) CHECK(title_height > wide_title_height);
            else if (!mode) wide_title_height = title_height;
            if (narrow) CHECK(work->rc.top > c->rect.bottom);
            else { CHECK(work->rc.left > c->rect.right); CHECK(work->rc.top < c->rect.top); }
            CHECK(doc_height(&doc) >= work->rc.bottom); CHECK(doc_height(&doc) >= c->rect.bottom);
            Item *option = NULL;
            for (size_t i = 0; i < doc.count; i++) if (str_eq(doc.items[i].text, long_option)) option = &doc.items[i];
            CHECK(option != NULL);
            if (!option) continue;
            int option_height = option->rc.bottom - option->rc.top;
            if (narrow) CHECK(option_height > wide_option_height);
            else if (!mode) { wide_option_height = option_height; wide_option_width = option->rc.right - option->rc.left; }
            else {
                CHECK_INT(option_height, wide_option_height);
                CHECK_INT(option->rc.right - option->rc.left, wide_option_width);
                CHECK(option->rc.left > px(100));
            }
            CHECK(option->rc.right <= width);
            int hit = doc_hit(&doc, option->rc.left + 1, option->rc.bottom - 1);
            CHECK(hit >= 0);
            if (hit < 0) continue;
            CHECK_INT(doc.items[hit].action, STATUS_OPTION);
            CHECK_STR(doc.items[hit].text, long_option);
            RECT content = pane_content_rect(pane);
            status_place(&s->base, &content, 0); CHECK(c->edit != NULL);
            HWND edit = c->edit; SetFocus(edit);
            wchar_t text[100]; GetWindowTextW(edit, text, 100); CHECK(wcscmp(text, L"Retain this draft across reflow") == 0);
            RECT before; GetWindowRect(edit, &before); MapWindowPoints(NULL, pane_hwnd(pane), (POINT *)&before, 2);
            int margin = (bounds.right - pane_content_width(pane)) / 2;
            CHECK_INT(before.left, c->rect.left + content.left + margin);
            CHECK_INT(before.top, c->rect.top + content.top);
            CHECK_INT(before.right - before.left, c->rect.right - c->rect.left);
            status_place(&s->base, &content, px(12));
            RECT after; GetWindowRect(edit, &after);
            MapWindowPoints(NULL, pane_hwnd(pane), (POINT *)&after, 2);
            CHECK_INT(after.top, before.top - px(12)); CHECK_INT(after.left, before.left);
            CHECK(c->edit == edit); CHECK(GetFocus() == edit);
            RECT label; GetWindowRect(c->label, &label);
            MapWindowPoints(NULL, pane_hwnd(pane), (POINT *)&label, 2);
            CHECK(label.bottom <= after.top);
            CHECK_INT(label.left, c->label_rect.left + content.left + margin);
            CHECK(GetWindow(c->edit, GW_HWNDPREV) == c->label);
            CHECK_STR(c->draft, "Retain this draft across reflow");
        }
    }
    theme_set_dpi(dpi); doc_free(&doc); cleanup(&stub); ShowWindow(dispatcher, SW_HIDE);
}
static void test_reply_keyboard(void) {
    Stub stub = {0}; StatusScreen *s = setup(&stub, "manage", 4); pump(3);
    ShowWindow(dispatcher, SW_SHOWNA);
    RECT bounds = {0, 0, 700, 1200}; pane_set_bounds(pane, &bounds);
    Doc doc; doc_init(&doc); draw(s, &doc);
    RECT content = pane_content_rect(pane); status_place(&s->base, &content, 0);
    StatusCard *c = s->cards[card(s, "wait")]; CHECK(c->edit != NULL);
    HWND edit = c->edit; int dpi = theme_dpi();
    theme_set_dpi(dpi * 2); status_place(&s->base, &content, 0);
    CHECK(c->edit == edit); CHECK((HFONT)SendMessageW(edit, WM_GETFONT, 0, 0) == font(FONT_BODY));
    theme_set_dpi(dpi); status_place(&s->base, &content, 0);
    CHECK((HFONT)SendMessageW(edit, WM_GETFONT, 0, 0) == font(FONT_BODY));
    SetWindowTextW(c->edit, L"Keyboard fixture");
    SendMessageW(c->edit, WM_KEYDOWN, VK_TAB, 0); CHECK(GetFocus() == pane_hwnd(pane));
    SetFocus(c->edit); SendMessageW(c->edit, WM_KEYDOWN, VK_ESCAPE, 0); CHECK(GetFocus() == pane_hwnd(pane));
    SendMessageW(c->edit, WM_KEYDOWN, VK_RETURN, 0);
    SendMessageW(c->edit, WM_CHAR, VK_RETURN, 0); pump(5); CHECK_INT(stub.writes, 1);
    Json *body = json_parsez(stub.body); CHECK_STR(json_str(json_get(body, "text")), "Keyboard fixture"); json_free(body);
    SendMessageW(c->edit, WM_KEYDOWN, VK_F5, 0); CHECK(s->projects_read != NULL); pump(3);
    doc_free(&doc); cleanup(&stub); ShowWindow(dispatcher, SW_HIDE);
}
static void test_keyboard_rejects_unpainted_ask(void) {
    Stub stub = {0}; StatusScreen *s = setup(&stub, "manage", 4); pump(3);
    RECT bounds = {0, 0, 700, 1200}; pane_set_bounds(pane, &bounds);
    Doc doc; doc_init(&doc); draw(s, &doc);
    RECT content = pane_content_rect(pane); status_place(&s->base, &content, 0);
    StatusCard *c = s->cards[card(s, "wait")]; CHECK(c->edit != NULL);
    SetWindowTextW(c->edit, L"Draft for original ask"); SetFocus(c->edit);
    stub.answer = "{\"session\":{\"id\":\"wait\",\"repo\":\"one/repo\",\"status\":\"idle\",\"awaitingAnswer\":true},"
        "\"events\":[{\"seq\":2,\"kind\":\"ask\",\"text\":\"Replacement fixture\"}]}";
    s->next_question = 0; status_questions_next(s); pump(1);
    CHECK_INT(transcript_pending_question(&c->transcript)->seq, 2);
    CHECK(status_can_answer(s, c));
    // Dispatch Enter before the deferred replacement layout.
    SendMessageW(c->edit, WM_KEYDOWN, VK_RETURN, 0);
    CHECK(s->check_read == NULL); CHECK_INT(stub.writes, 0);
    CHECK_STR(c->draft, "Draft for original ask");
    draw(s, &doc); status_place(&s->base, &content, 0);
    SendMessageW(c->edit, WM_KEYDOWN, VK_RETURN, 0); pump(5);
    CHECK_INT(stub.writes, 1);
    Json *body = json_parsez(stub.body); CHECK_STR(json_str(json_get(body, "text")), "Draft for original ask"); json_free(body);
    doc_free(&doc); cleanup(&stub);
}
static void test_uncertainty_survives_reopen(void) {
    for (int cancelled = 0; cancelled < 2; cancelled++) {
        Stub stub = {0}; StatusScreen *s = setup(&stub, "manage", 4); pump(3);
        if (cancelled) block(&stub, true);
        else stub.write_status = 500;
        status_send(s, card(s, "wait"), "Original reply");
        if (cancelled) {
            pump(1); CHECK_INT(WaitForSingleObject(stub.entered, 10000), WAIT_OBJECT_0);
        } else pump(5);
        // Replace the root, freeing the card and locally cancelling any outstanding POST.
        pane_set_root(pane, NULL);
        s = (StatusScreen *)status_screen_new(); pane_set_root(pane, &s->base); pump(3);
        size_t index = card(s, "wait"); StatusCard *c = s->cards[index];
        CHECK(c->confirmed); CHECK(status_uncertain(s, c)); CHECK(!status_can_answer(s, c));
        CHECK(!status_uncertain(s, s->cards[card(s, "held")]));
        status_send(s, index, "Duplicate"); CHECK(s->check_read == NULL); CHECK_INT(stub.writes, 1);
        if (cancelled) {
            SetEvent(stub.resume); pump(1); // discarded completion must not silently clear the interlock
            CHECK(status_uncertain(s, c));
        }
        s->base.vt->refresh(&s->base); pump(3); CHECK(!status_can_answer(s, c));
        Doc doc; doc_init(&doc); draw(s, &doc);
        intptr_t resolve = card_action_arg(s, &doc, STATUS_RESOLVE, index);
        s->base.vt->action(&s->base, STATUS_RESOLVE, resolve, (POINT){0}); pump(3);
        CHECK(!status_uncertain(s, c)); CHECK(status_can_answer(s, c)); CHECK_INT(stub.writes, 1);
        stub.write_status = 0;
        status_send(s, index, "Explicit retry"); pump(5); CHECK_INT(stub.writes, 2);
        doc_free(&doc); cleanup(&stub);
    }
}
static void test_held_navigation_and_teardown(void) {
    Stub stub = {0}; StatusScreen *s = setup(&stub, "manage", 4); pump(3);
    Screen *moved = pane_take_root(pane);
    Pane *window = detached_open(moved, NULL, false); CHECK(window != NULL); pump(3);
    Doc doc; doc_init(&doc); draw(s, &doc);
    intptr_t open = card_action_arg(s, &doc, STATUS_OPEN, card(s, "held"));
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
    test_run("Status blank replies stay disabled and long option values survive rendering", test_blank_reply_and_long_option);
    test_run("Status option/text sends use message contract and suppress duplicates", test_option_and_text_wire);
    test_run("Status respects catalog/token and rejects changed questions and stale actions", test_permissions_and_changed_ask);
    test_run("Status refreshes counts, isolates badges and rejects pre-paint stale actions", test_refresh_and_cached_questions);
    test_run("Status retains drafts on refusal and blocks uncertain sends", test_failures_and_uncertainty);
    test_run("Status cancels obsolete reads and isolates server/project snapshots", test_cancel_and_isolation);
    test_run("Status polling preserves slow reads, scan progress and send preflights", test_poll_preserves_reads_and_scan);
    test_run("Status polling cancels reads for changed identities", test_poll_invalidates_changed_reads);
    test_run("Status live snapshots survive failed cache writes and remain scoped", test_live_snapshot_without_cache_write);
    test_run("Status findings replace an answered question preview", test_findings_replace_question_preview);
    test_run("Status detail Retry-After pauses scans and preflights across polls and reopening", test_detail_retry_after);
    test_run("Status message Retry-After blocks replies across refresh and reopening until expiry", test_message_retry_after);
    test_run("Status delayed detail preserves newer shared cached events", test_delayed_detail_preserves_shared_events);
    test_run("Status retries failed transcript batches with later or empty reads", test_transcript_persistence_retry);
    test_run("Status responsive columns preserve wrapped actions and native edit offsets", test_responsive_layout_and_controls);
    test_run("Status reply edit handles DPI changes, submit, focus escape, tab and refresh", test_reply_keyboard);
    test_run("Status keyboard rejects replacement asks until rendered", test_keyboard_rejects_unpainted_ask);
    test_run("Status unknown send interlocks survive root replacement and refresh", test_uncertainty_survives_reopen);
    test_run("Status held findings open their conversation and teardown cancels reads", test_held_navigation_and_teardown);
    pane_destroy(pane); DestroyWindow(dispatcher); cache_remove_all(g_store.cache); cache_free(g_store.cache); RemoveDirectoryW(directory); memset(&g_store, 0, sizeof g_store);
}
