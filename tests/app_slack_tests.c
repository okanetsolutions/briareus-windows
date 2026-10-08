// Exercise actual inbox completions and navigation without provider calls or interactive windows.
#include "slack_inbox.h"
#include "slack_fixtures.h"
#include "str.h"
#include "suites.h"
#include "test.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

static Route routes[] = {
    { "GET", "/slack/workspaces", "admin" },
    { "GET", "/slack/workspaces/{id}/events", "admin" },
    { "GET", "/slack/workspaces/{id}/conversations", "admin" },
    { "GET", "/slack/workspaces/{id}/people", "admin" },
    { "GET", "/slack/workspaces/{id}/conversations/{channel}", "admin" },
    { "GET", "/slack/workspaces/{id}/conversations/{channel}/messages", "admin" },
    { "POST", "/slack/workspaces/{id}/direct-messages", "admin" },
    { "POST", "/slack/workspaces/{id}/conversations/{channel}/messages", "admin" },
    { "POST", "/slack/workspaces/{id}/conversations/{channel}/read", "admin" },
    { "GET", "/slack/workspaces/{id}/conversations/{channel}/threads/{ts}", "admin" },
};
static void setup(void) {
    memset(&g_store, 0, sizeof g_store);
    g_store.has_device = true; g_store.device.id = "admin-1"; g_store.device.permission = "admin";
    g_store.routes = routes; g_store.route_count = sizeof routes / sizeof *routes;
    ServerAddress address; CHECK(server_address_parse("https://slack.example", &address));
    ApiError error; api_error_init(&error);
    g_store.client = api_client_new(&address, "brm_aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", &error);
    CHECK(g_store.client != NULL); api_error_clear(&error); server_address_free(&address);
}
static void teardown(SlackScreen *s) { s->base.vt->destroy(&s->base); api_client_release(g_store.client); memset(&g_store, 0, sizeof g_store); }
static void apply(SlackScreen *s, int tag, const char *operation, const char *source, uint64_t generation, int status) {
    Request req = {0}; req.tag = tag; req.operation = (char *)operation; req.client = g_store.client; req.arg = (intptr_t)generation;
    req.args = json_object(); json_set_str(req.args, "id", s->workspace); json_set_str(req.args, "channel", s->channel);
    if (s->thread) json_set_str(req.args, "threadTs", s->thread);
    json_set_str(req.args, "text", "human reply"); json_set_str(req.args, "ts", "1712345678.000001");
    req.result = json_parsez(source ? source : "{}"); req.ok = status == 0; api_error_init(&req.error);
    if (status) api_error_set(&req.error, API_HTTP, status, status == 502 ? "Slack token lacks im:read scope; reinstall the app" : "Refused", status == 429 ? 60 : -1);
    slack_inbox_done(s, &req); json_free(req.args); json_free(req.result); api_error_clear(&req.error);
}
static void choose(SlackScreen *s, int action, intptr_t arg) { s->base.vt->action(&s->base, action, arg, (POINT){0}); }
static void load_navigation(SlackScreen *s) {
    apply(s, TAG_WORKSPACES, "slack_workspaces", SLACK_WORKSPACES, s->state.generation, 0); choose(s, ACT_WORKSPACE, 0);
    CHECK_STR(s->workspace, "1727000000002");
    apply(s, TAG_CONVERSATIONS, "slack_conversations", SLACK_CONVERSATIONS, s->state.generation, 0);
    apply(s, TAG_PEOPLE, "slack_people", SLACK_PEOPLE, s->state.generation, 0);
    choose(s, ACT_CHANNEL, 0); CHECK_STR(s->channel, "C1");
}
static void permissions(void) {
    setup(); CHECK(slack_inbox_offered());
    for (size_t i = 0; i < sizeof routes / sizeof *routes; i++) {
        g_store.device.permission = "read"; CHECK(!slack_inbox_offered()); CHECK(!slack_inbox_supports("slack_send"));
        g_store.device.permission = "manage"; CHECK(!slack_inbox_offered());
        g_store.device.permission = "admin";
    }
    g_store.routes = NULL; g_store.route_count = 0; CHECK(!slack_inbox_offered()); CHECK(!slack_inbox_supports("slack_send"));
    // Even a catalog accidentally labeling inbox routes as read cannot grant private access.
    Route weak[] = {{ "GET", "/slack/workspaces", "read" }};
    g_store.routes = weak; g_store.route_count = 1; g_store.device.permission = "manage"; CHECK(!slack_inbox_offered());
    g_store.device.permission = "admin"; CHECK(slack_inbox_offered()); CHECK(!slack_inbox_supports("slack_history"));
    g_store.has_device = false; CHECK(!slack_inbox_offered()); g_store.has_device = true;
    SlackScreen *s = (SlackScreen *)slack_screen_new(); teardown(s);
}
static void workspace_team_label(void) {
    setup(); SlackScreen *s = (SlackScreen *)slack_screen_new();
    apply(s, TAG_WORKSPACES, "slack_workspaces", SLACK_WORKSPACES, s->state.generation, 0);
    if (!theme.canvas) theme_init();
    Doc doc; doc_init(&doc); doc_begin(&doc, NULL, 600); s->base.vt->layout(&s->base, &doc); doc_end(&doc);
    bool found = false;
    for (size_t i = 0; i < doc.count; i++) if (doc.items[i].action == ACT_WORKSPACE) {
        CHECK_STR(doc.items[i].text, "Business · Example"); found = true;
    }
    CHECK(found); doc_free(&doc); teardown(s);
}
static void recovery_revalidates_draft(void) {
    setup(); SlackScreen *s = (SlackScreen *)slack_screen_new(); load_navigation(s);
    Json *destination = json_object(); json_set_str(destination, "id", s->workspace); json_set_str(destination, "channel", s->channel);
    SlackDraft *d = slack_draft(&s->state, s->workspace, s->channel, NULL); d->uncertain = true;
    set_string(&d->text, "retained draft"); set_string(&s->error, "Check history before resending");
    uint64_t generation = s->state.generation;
    // Another draft can relocate the array while the confirmation is open.
    slack_draft(&s->state, s->workspace, "C2", NULL);
    slack_inbox_recover_confirmed(s, destination, generation);
    d = slack_draft(&s->state, s->workspace, s->channel, NULL);
    CHECK(!d->uncertain); CHECK_STR(d->text, "retained draft"); CHECK(!s->error);
    d->uncertain = true;
    json_set_str(destination, "channel", "C2"); slack_inbox_recover_confirmed(s, destination, generation); CHECK(d->uncertain);
    json_set_str(destination, "channel", "C1"); json_set_str(destination, "threadTs", "1.1");
    slack_inbox_recover_confirmed(s, destination, generation); CHECK(d->uncertain); json_object_remove(destination, "threadTs");
    choose(s, ACT_REFRESH, 0); slack_inbox_recover_confirmed(s, destination, generation); CHECK(d->uncertain);
    const int statuses[] = {403, 404, 409};
    for (size_t i = 0; i < sizeof statuses / sizeof *statuses; i++) {
        generation = s->state.generation;
        apply(s, TAG_HISTORY, "slack_history", NULL, generation, statuses[i]);
        slack_inbox_recover_confirmed(s, destination, generation);
        CHECK_INT(s->state.draft_count, 0); CHECK(!s->workspace); CHECK(s->error != NULL);
        // Reloading the same destination must not authorize its new draft with the old confirmation.
        load_navigation(s); d = slack_draft(&s->state, s->workspace, s->channel, NULL); d->uncertain = true;
        slack_inbox_recover_confirmed(s, destination, generation); CHECK(d->uncertain);
    }
    generation = s->state.generation; g_store.device.id = "admin-2";
    slack_inbox_recover_confirmed(s, destination, generation); CHECK_INT(s->state.draft_count, 0); CHECK(!s->workspace);
    load_navigation(s); d = slack_draft(&s->state, s->workspace, s->channel, NULL); d->uncertain = true;
    generation = s->state.generation; g_store.device.permission = "manage";
    slack_inbox_recover_confirmed(s, destination, generation); CHECK_INT(s->state.draft_count, 0);
    g_store.device.permission = "admin"; slack_inbox_store_changed(); load_navigation(s);
    slack_state_clear(&s->state); generation = s->state.generation;
    slack_inbox_recover_confirmed(s, destination, generation); CHECK_INT(s->state.draft_count, 0);
    teardown(s); slack_inbox_recover_confirmed(s, destination, generation); json_free(destination);
}
static void find_revalidates_screen(void) {
    setup(); SlackScreen *s = (SlackScreen *)slack_screen_new(); load_navigation(s); choose(s, ACT_PEOPLE, 0);
    char *workspace = xstrdup(s->workspace); uint64_t generation = s->state.generation;
    slack_inbox_find_confirmed(s, workspace, generation, "Alice"); CHECK_STR(s->search, "Alice");
    slack_inbox_find_confirmed(s, workspace, generation, NULL); CHECK_STR(s->search, "Alice");
    slack_inbox_find_confirmed(s, "another workspace", generation, "Bob"); CHECK_STR(s->search, "Alice");
    choose(s, ACT_PEOPLE, 0);
    slack_inbox_find_confirmed(s, workspace, generation, "Bob"); CHECK_STR(s->search, "Alice");
    choose(s, ACT_PEOPLE, 0); choose(s, ACT_REFRESH, 0);
    slack_inbox_find_confirmed(s, workspace, generation, "Bob"); CHECK_STR(s->search, "Alice");
    generation = s->state.generation;
    slack_inbox_find_confirmed(s, workspace, generation, ""); CHECK_STR(s->search, "");
    const int statuses[] = {403, 404, 409};
    for (size_t i = 0; i < sizeof statuses / sizeof *statuses; i++) {
        generation = s->state.generation;
        apply(s, TAG_PEOPLE, "slack_people", NULL, generation, statuses[i]);
        slack_inbox_find_confirmed(s, workspace, generation, "Bob"); CHECK(!s->search); CHECK(!s->workspace);
        load_navigation(s); choose(s, ACT_PEOPLE, 0);
        slack_inbox_find_confirmed(s, workspace, generation, "Bob"); CHECK(!s->search);
    }
    generation = s->state.generation; g_store.device.id = "admin-2";
    slack_inbox_find_confirmed(s, workspace, generation, "Bob"); CHECK(!s->search); CHECK(!s->workspace);
    load_navigation(s); choose(s, ACT_PEOPLE, 0);
    generation = s->state.generation; g_store.device.permission = "manage";
    slack_inbox_find_confirmed(s, workspace, generation, "Bob"); CHECK(!s->search); CHECK(!s->workspace);
    // Credential invalidation can clear private state and then destroy the screen during the dialog.
    g_store.has_device = false; slack_inbox_store_changed();
    teardown(s); slack_inbox_find_confirmed(s, workspace, generation, "Bob"); free(workspace);
}
static void rate_limit(SlackScreen *s, int tag, double delay) {
    Request req = {0}; req.tag = tag; req.operation = tag == TAG_PEOPLE ? "slack_people" : "slack_conversations";
    req.client = g_store.client; req.arg = (intptr_t)s->state.generation;
    api_error_init(&req.error); api_error_set(&req.error, API_HTTP, 429, "Rate limited", delay);
    slack_inbox_done(s, &req); api_error_clear(&req.error);
}
static void cooldown_keeps_longest_wait(void) {
    setup(); SlackScreen *s = (SlackScreen *)slack_screen_new(); load_navigation(s);
    rate_limit(s, TAG_CONVERSATIONS, 60); uint64_t deadline = s->cooldown;
    CHECK(deadline > GetTickCount64()); CHECK(strstr(s->error, "60 seconds") != NULL);
    rate_limit(s, TAG_PEOPLE, 5); CHECK(s->cooldown == deadline);
    unsigned wait = 0;
    CHECK_INT(sscanf(s->error, "Slack is rate limiting this workspace. Wait %u seconds", &wait), 1);
    CHECK(wait > 5); CHECK(wait <= 60);
    uint64_t generation = s->state.generation; choose(s, ACT_REFRESH, 0); CHECK_INT(s->state.generation, generation);
    rate_limit(s, TAG_PEOPLE, 120); CHECK(s->cooldown > deadline); CHECK(strstr(s->error, "120 seconds") != NULL);
    s->cooldown = GetTickCount64() - 1;
    rate_limit(s, TAG_CONVERSATIONS, 5); CHECK(s->cooldown > GetTickCount64()); CHECK(strstr(s->error, "5 seconds") != NULL);
    teardown(s);
}
static void navigation_and_stale(void) {
    setup(); SlackScreen *s = (SlackScreen *)slack_screen_new(); load_navigation(s);
    uint64_t old = s->state.generation;
    apply(s, TAG_HISTORY, "slack_history", SLACK_HISTORY, old, 0); CHECK_INT(s->state.message_count, 2);
    SlackDraft *d = slack_draft(&s->state, s->workspace, s->channel, NULL); free(d->text); d->text = xstrdup("channel draft");
    choose(s, ACT_THREAD, 0); CHECK_STR(s->thread, "1712345678.000001");
    apply(s, TAG_HISTORY, "slack_history", "{\"messages\":[{\"ts\":\"9999.1\"}],\"nextCursor\":\"\",\"hasMore\":false}", old, 0);
    CHECK_INT(s->state.message_count, 2);
    apply(s, TAG_HISTORY, "slack_thread", SLACK_THREAD, s->state.generation, 0); CHECK_INT(s->state.message_count, 3);
    d = slack_draft(&s->state, s->workspace, s->channel, s->thread); free(d->text); d->text = xstrdup("thread draft");
    choose(s, ACT_BACK, 0); CHECK(!s->thread); CHECK_STR(slack_draft(&s->state, s->workspace, s->channel, NULL)->text, "channel draft");
    choose(s, ACT_BACK, 0); choose(s, ACT_CHANNEL, 1); CHECK_STR(s->channel, "G1");
    choose(s, ACT_BACK, 0); choose(s, ACT_CHANNEL, 2); CHECK_STR(s->channel, "D1");
    choose(s, ACT_BACK, 0); choose(s, ACT_CHANNEL, 3); CHECK_STR(s->channel, "G2");
    choose(s, ACT_BACK, 0); choose(s, ACT_PEOPLE, 0); CHECK(s->directory);
    apply(s, TAG_DM, "slack_open_dm", "{\"conversation\":{\"id\":\"D9\",\"is_im\":true,\"user\":\"U1\"}}", s->state.generation, 0); CHECK_STR(s->channel, "D9"); CHECK(!s->directory);
    choose(s, ACT_BACK, 0); choose(s, ACT_CHANNEL, 0); choose(s, ACT_THREAD, 0);
    CHECK_STR(slack_draft(&s->state, s->workspace, s->channel, s->thread)->text, "thread draft");
    teardown(s);
}
static void sends_and_access_clear(void) {
    setup(); SlackScreen *s = (SlackScreen *)slack_screen_new(); load_navigation(s);
    SlackDraft *d = slack_draft(&s->state, s->workspace, s->channel, NULL); free(d->text); d->text = xstrdup("human reply"); CHECK(slack_draft_begin(d));
    apply(s, TAG_SEND, "slack_send", NULL, s->state.generation, 503); CHECK(d->uncertain); CHECK_STR(d->text, "human reply");
    CHECK(!slack_draft_begin(d)); CHECK(strstr(s->error, "503") != NULL);
    d->uncertain = false; CHECK(slack_draft_begin(d)); apply(s, TAG_SEND, "slack_send", SLACK_RECEIPT, s->state.generation, 0);
    CHECK_STR(d->text, ""); CHECK_INT(s->state.message_count, 1); CHECK_STR(s->state.messages[0].workspace, "1727000000002");
    free(d->text); d->text = xstrdup("human reply"); CHECK(slack_draft_begin(d));
    apply(s, TAG_SEND, "slack_send", SLACK_CHANGED_RECEIPT, s->state.generation, 0);
    CHECK_INT(s->state.draft_count, 0); CHECK_INT(s->state.message_count, 0); CHECK(strstr(s->error, "confirmed sent") != NULL); CHECK(!s->workspace);
    load_navigation(s); d = slack_draft(&s->state, s->workspace, s->channel, NULL); free(d->text); d->text = xstrdup("private draft");
    g_store.device.permission = "manage"; slack_inbox_store_changed(); CHECK(!s->workspace); CHECK_INT(s->state.draft_count, 0); CHECK_INT(json_count(s->people), 0);
    g_store.device.permission = "admin"; slack_inbox_store_changed(); load_navigation(s);
    g_store.device.id = "admin-2"; slack_inbox_store_changed(); CHECK_INT(json_count(s->workspaces), 0);
    load_navigation(s); g_store.route_count = 1; slack_inbox_store_changed(); CHECK(!s->workspace); CHECK_INT(json_count(s->conversations), 0);
    teardown(s);
}
static void errors_and_read_marks(void) {
    setup(); SlackScreen *s = (SlackScreen *)slack_screen_new(); load_navigation(s);
    apply(s, TAG_HISTORY, "slack_history", NULL, s->state.generation, 429); CHECK(s->cooldown > GetTickCount64()); CHECK(strstr(s->error, "60 seconds") != NULL);
    CHECK(strstr(s->error, "retry the read or action yourself") != NULL);
    uint64_t generation = s->state.generation; choose(s, ACT_REFRESH, 0); CHECK_INT(s->state.generation, generation);
    s->cooldown = 0; apply(s, TAG_READ, "slack_read", NULL, generation, 502); CHECK(s->read_failed); CHECK(strstr(s->error, "im:read") != NULL);
    SlackRead *r = slack_read(&s->state, s->workspace, s->channel); slack_read_viewed(r, "1712345678.000001", 0);
    apply(s, TAG_READ, "slack_read", "{\"ok\":true}", generation, 0); CHECK_STR(r->marked, "1712345678.000001");
    apply(s, TAG_HISTORY, "slack_history", NULL, generation, 409); CHECK(!s->workspace); CHECK_INT(s->state.read_count, 0);
    load_navigation(s); apply(s, TAG_HISTORY, "slack_history", NULL, s->state.generation, 404); CHECK(!s->workspace);
    load_navigation(s); apply(s, TAG_HISTORY, "slack_history", NULL, s->state.generation, 403); CHECK(!s->workspace);
    load_navigation(s); apply(s, TAG_HISTORY, "slack_history", "{}", s->state.generation, 0); CHECK(strstr(s->error, "unexpected") != NULL);
    SlackDraft *d = slack_draft(&s->state, s->workspace, s->channel, NULL);
    free(d->text); d->text = xstrdup("private draft"); CHECK(slack_draft_begin(d));
    apply(s, TAG_SEND, "slack_send", NULL, s->state.generation, 403);
    CHECK(!s->workspace); CHECK_INT(s->state.draft_count, 0); CHECK_INT(json_count(s->people), 0);
    CHECK(strstr(s->error, "denied") != NULL);
    teardown(s);
}
static void viewed_messages_only(void) {
    setup(); SlackScreen *s = (SlackScreen *)slack_screen_new(); load_navigation(s);
    apply(s, TAG_HISTORY, "slack_history", SLACK_HISTORY, s->state.generation, 0);
    apply(s, TAG_DETAIL, "slack_conversation", "{\"conversation\":{\"id\":\"C1\"}}", s->state.generation, 0);
    if (!theme.canvas) theme_init();
    Doc doc; doc_init(&doc); doc_begin(&doc, NULL, 600); s->base.vt->layout(&s->base, &doc); doc_end(&doc);
    CHECK_INT(s->view_count, 2);
    RECT viewport = {0, 0, 600, 100};
    s->base.vt->place(&s->base, &viewport, s->view[0].rc.bottom - 90);
    CHECK_INT(s->state.read_count, 0); // Hidden: loading messages is not viewing them.
    s->shown = true; g_store.active = false;
    s->base.vt->place(&s->base, &viewport, s->view[0].rc.bottom - 90); CHECK_INT(s->state.read_count, 0);
    g_store.active = true;
    s->base.vt->place(&s->base, &viewport, s->view[0].rc.bottom - 90);
    SlackRead *r = slack_read(&s->state, s->workspace, s->channel); CHECK_STR(r->viewed, "1712345678.000001");
    CHECK(slack_read_due(r, GetTickCount64()) == NULL);
    s->base.vt->place(&s->base, &viewport, s->view[1].rc.bottom - 90); CHECK_STR(r->viewed, "1712345678.000002");
    // A newer reply in an old thread must never clear unseen channel messages.
    free(r->viewed); r->viewed = xstrdup("1712345678.000001");
    s->thread = xstrdup("1712345678.000001");
    s->base.vt->place(&s->base, &viewport, s->view[1].rc.bottom - 90); CHECK_STR(r->viewed, "1712345678.000001");
    free(s->thread); s->thread = NULL;
    free(r->viewed); r->viewed = xstrdup("1712345678.000002");
    s->directory = true; r->due = 0;
    s->base.vt->place(&s->base, &viewport, s->view[0].rc.bottom - 90); CHECK_STR(r->viewed, "1712345678.000002");
    s->shown = false; doc_free(&doc); teardown(s);
}

static void cancellation_and_thread_send(void) {
    setup(); SlackScreen *s = (SlackScreen *)slack_screen_new(); load_navigation(s);
    apply(s, TAG_HISTORY, "slack_history", SLACK_HISTORY, s->state.generation, 0);
    choose(s, ACT_THREAD, 0);
    SlackDraft *d = slack_draft(&s->state, s->workspace, s->channel, s->thread);
    free(d->text); d->text = xstrdup("human reply"); CHECK(slack_draft_begin(d));
    apply(s, TAG_SEND, "slack_send", SLACK_RECEIPT, s->state.generation, 0);
    CHECK_STR(d->text, ""); CHECK_STR(json_str(json_get(s->state.messages[2].raw, "thread_ts")), "1712345678.000001");
    free(d->text); d->text = xstrdup("pending thread reply"); CHECK(slack_draft_begin(d));
    Request *req = xcalloc(1, sizeof *req); api_error_init(&req->error);
    req->owner = s; req->done = slack_inbox_done; req->operation = xstrdup("slack_send");
    req->client = api_client_retain(g_store.client); req->slot = &s->requests[TAG_SEND]; s->requests[TAG_SEND] = req;
    uint64_t generation = s->state.generation;
    s->base.vt->visible(&s->base, false);
    CHECK(s->requests[TAG_SEND] == NULL); CHECK(req->cancelled); CHECK(d->uncertain); CHECK(!d->sending);
    CHECK(!slack_state_current(&s->state, generation)); CHECK_STR(d->text, "pending thread reply");
    // The discarded completion can arrive after the screen has been destroyed.
    teardown(s); store_handle_message(WM_APP_REQUEST_DONE, 0, (LPARAM)req);
}

static void live(SlackScreen *s, const char *name, const char *source) {
    slack_inbox_event(s, s->stream_generation, name, source, strlen(source));
}
#define LIVE_READY "{\"workspaceId\":1727000000002,\"userId\":\"U1\",\"refresh\":true}"
#define LIVE_MESSAGE "{\"workspaceId\":1727000000002,\"eventId\":\"ev-own\",\"event\":{\"channel\":\"C1\",\"ts\":\"1712345678.000003\",\"text\":\"own event\",\"user\":\"U1\"}}"
#define LIVE_EDIT "{\"workspaceId\":1727000000002,\"event\":{\"channel\":\"C1\",\"message\":{\"ts\":\"1712345678.000003\",\"text\":\"edited own\"}}}"
#define LIVE_DELETE "{\"workspaceId\":1727000000002,\"event\":{\"channel\":\"C1\",\"deleted_ts\":\"1712345678.000003\"}}"
static const SlackMessage *find_message(SlackScreen *s, const char *ts) {
    for (size_t i = 0; i < s->state.message_count; i++) if (str_eq(ts, s->state.messages[i].ts)) return &s->state.messages[i];
    return NULL;
}
static void snapshots(SlackScreen *s, const char *history) {
    apply(s, TAG_CONVERSATIONS, "slack_conversations", SLACK_CONVERSATIONS, s->state.generation, 0);
    apply(s, TAG_DETAIL, "slack_conversation", "{\"conversation\":{\"id\":\"C1\",\"last_read\":\"1712345678.000001\"}}", s->state.generation, 0);
    apply(s, TAG_SNAPSHOT_CHANNEL, "slack_history", history, s->state.generation, 0);
}
static Request *pending(SlackScreen *s, int tag, const char *operation, const char *answer) {
    Request *req = xcalloc(1, sizeof *req); api_error_init(&req->error);
    req->owner = s; req->done = slack_inbox_done; req->tag = tag; req->operation = xstrdup(operation);
    req->client = api_client_retain(g_store.client); req->arg = (intptr_t)s->state.generation;
    req->args = json_object(); json_set_str(req->args, "id", s->workspace); json_set_str(req->args, "channel", s->channel);
    json_set_str(req->args, "text", "human reply"); json_set_str(req->args, "ts", "1712345678.000001");
    req->result = json_parsez(answer ? answer : "{}"); req->ok = true;
    req->slot = &s->requests[tag]; s->requests[tag] = req; return req;
}
static void complete(Request *req) { store_handle_message(WM_APP_REQUEST_DONE, 0, (LPARAM)req); }
static bool inbox_transport(void *ctx, const char *method, const char *url, const char *const *headers, const void *body, size_t body_len,
                            int timeout_ms, int *status, char **content_type, char **retry_after, char **response, size_t *response_len,
                            char **error_message) {
    const char *answer = !strstr(url, "/workspaces/") ? SLACK_WORKSPACES : strstr(url, "/people") ? SLACK_PEOPLE : strstr(url, "/threads/") ? SLACK_THREAD : strstr(url, "/messages") ? SLACK_HISTORY :
        strstr(url, "/conversations/C1") ? "{\"conversation\":{\"id\":\"C1\"}}" : SLACK_CONVERSATIONS;
    *status = 200; *content_type = xstrdup("application/json"); *response = xstrdup(answer); *response_len = strlen(answer);
    return true;
}
static HWND inbox_hwnd;
static Pane *inbox_pane;
static void request_pane(SlackScreen *s) {
    g_store.hwnd = inbox_hwnd;
    api_client_set_transport(g_store.client, inbox_transport, NULL);
    s->base.pane = inbox_pane; s->shown = true;
}
static void deliver_inbox_requests(int expected) {
    int count = 0; uint64_t deadline = GetTickCount64() + 5000;
    while (count < expected && GetTickCount64() < deadline) {
        MSG msg;
        if (PeekMessageW(&msg, g_store.hwnd, WM_APP_REQUEST_DONE, WM_APP_REQUEST_DONE, PM_REMOVE)) {
            store_handle_message(msg.message, msg.wParam, msg.lParam); count++;
        } else Sleep(1);
    }
    CHECK_INT(count, expected);
}
static void snapshot_rate_limit_resumes_progress(void) {
    setup(); SlackScreen *s = (SlackScreen *)slack_screen_new(); load_navigation(s);
    apply(s, TAG_HISTORY, "slack_history", SLACK_HISTORY, s->state.generation, 0);
    set_string(&s->thread, "1712345678.000001");
    live(s, "ready", LIVE_READY);
    apply(s, TAG_CONVERSATIONS, "slack_conversations", "{\"conversations\":[{\"id\":\"FIRST1\"}],\"nextCursor\":\"page2\"}", s->state.generation, 0);
    apply(s, TAG_CONVERSATIONS, "slack_conversations", "{\"conversations\":[{\"id\":\"FIRST2\"}],\"nextCursor\":\"page3\"}", s->state.generation, 0);
    apply(s, TAG_SNAPSHOT_CHANNEL, "slack_history", "{\"messages\":[{\"ts\":\"1712345680.1\",\"text\":\"missed bot\"}],\"nextCursor\":\"catchup\",\"hasMore\":true}", s->state.generation, 0);
    apply(s, TAG_SNAPSHOT_THREAD, "slack_thread", SLACK_EMPTY_PAGE, s->state.generation, 0);
    live(s, "message", LIVE_MESSAGE); live(s, "message.changed", LIVE_EDIT);
    request_pane(s); s->feed = slack_feed_new(g_store.client, s->workspace, s->stream_generation);
    SlackFeed *feed = s->feed; unsigned pending_reads = s->snapshot_pending;
    uint64_t generation = s->state.generation, stream_generation = s->stream_generation;
    const int tags[] = {TAG_CONVERSATIONS, TAG_DETAIL, TAG_SNAPSHOT_CHANNEL, TAG_SNAPSHOT_THREAD};
    const char *operations[] = {"slack_conversations", "slack_conversation", "slack_history", "slack_thread"};
    for (size_t i = 0; i < sizeof tags / sizeof *tags; i++) {
        Request *req = pending(s, tags[i], operations[i], NULL); req->ok = false;
        api_error_set(&req->error, API_HTTP, 429, "Rate limited", 60); complete(req);
        CHECK(s->reconciling); CHECK(s->feed == feed); CHECK_INT(s->snapshot_pending, pending_reads);
        CHECK_INT(s->state.generation, generation); CHECK_INT(s->stream_generation, stream_generation);
        CHECK_INT(s->stream_failures, 0); CHECK_INT(s->reconnect_at, 0);
        CHECK_STR(s->snapshot_cursor, "page3"); CHECK_INT(json_count(s->snapshot_conversations), 2);
        CHECK_STR(s->snapshot_channel.cursor, "catchup"); CHECK_STR(s->snapshot_thread.cursor, "next");
        CHECK_INT(s->snapshot.message_count, 1); CHECK_INT(s->buffered.count, 2);
        CHECK(s->cooldown > GetTickCount64()); CHECK(strstr(s->error, "60 seconds") != NULL);
        CHECK(strstr(s->error, "Sync resumes automatically") != NULL);
        CHECK(strstr(s->error, "retry the read or action yourself") == NULL);
        // The timer keeps accepting events but must not issue any read before Retry-After.
        slack_inbox_pump(s);
        for (size_t j = 0; j < sizeof tags / sizeof *tags; j++) CHECK(s->requests[tags[j]] == NULL);
        if (!s->reconciling) { teardown(s); return; }
    }
    apply(s, TAG_PEOPLE, "slack_people", NULL, generation, 429);
    CHECK(strstr(s->error, "retry the read or action yourself") != NULL);
    s->cooldown = 0; slack_inbox_pump(s);
    for (size_t i = 0; i < sizeof tags / sizeof *tags; i++) CHECK(s->requests[tags[i]] != NULL);
    const Request *list = s->requests[TAG_CONVERSATIONS], *channel = s->requests[TAG_SNAPSHOT_CHANNEL], *thread = s->requests[TAG_SNAPSHOT_THREAD];
    CHECK_STR(json_str(json_get(list ? list->args : NULL, "cursor")), "page3");
    CHECK_STR(json_str(json_get(channel ? channel->args : NULL, "cursor")), "catchup");
    CHECK_STR(json_str(json_get(thread ? thread->args : NULL, "cursor")), "next");
    CHECK_STR(json_str(json_get(thread ? thread->args : NULL, "ts")), "1712345678.000001");
    deliver_inbox_requests(4);
    CHECK(!s->reconciling); CHECK_INT(s->snapshot_pending, 0); CHECK(s->feed == feed);
    CHECK_INT(json_count(s->conversations), 6);
    CHECK_STR(json_str(json_get(json_at(s->conversations, 0), "id")), "FIRST1");
    CHECK_STR(json_str(json_get(json_at(s->conversations, 1), "id")), "FIRST2");
    CHECK(find_message(s, "1712345680.1") != NULL);
    const SlackMessage *edited = find_message(s, "1712345678.000003"); CHECK(edited != NULL);
    CHECK_STR(json_str(json_get(edited ? edited->raw : NULL, "text")), "edited own");
    CHECK_INT(s->buffered.count, 0); CHECK(s->history_loaded); CHECK(s->detail != NULL);
    teardown(s);
}
static void ready_preserves_pending_receipts(void) {
    setup(); SlackScreen *s = (SlackScreen *)slack_screen_new(); load_navigation(s);
    for (int receipt_first = 0; receipt_first < 2; receipt_first++) {
        SlackDraft *d = slack_draft(&s->state, s->workspace, s->channel, NULL);
        set_string(&d->text, "human reply"); CHECK(slack_draft_begin(d));
        Request *send = pending(s, TAG_SEND, "slack_send", SLACK_RECEIPT);
        Request *read = pending(s, TAG_READ, "slack_read", "{\"ok\":true}");
        Request *people = pending(s, TAG_PEOPLE, "slack_people", SLACK_PEOPLE);
        Request *dm = pending(s, TAG_DM, "slack_open_dm", "{}");
        Request *history = pending(s, TAG_HISTORY, "slack_history", SLACK_HISTORY);
        live(s, "ready", LIVE_READY);
        CHECK(!send->cancelled); CHECK(!read->cancelled); CHECK(!people->cancelled); CHECK(!dm->cancelled);
        CHECK_INT(send->arg, s->state.generation); CHECK(d->sending); CHECK(!d->uncertain);
        CHECK(history->cancelled); CHECK(!s->requests[TAG_HISTORY]);
        if (receipt_first) complete(send);
        snapshots(s, SLACK_HISTORY);
        if (!receipt_first) complete(send);
        CHECK(!d->sending); CHECK(!d->uncertain); CHECK_STR(d->text, "");
        CHECK(find_message(s, "1712345680.000001") != NULL);
        complete(read); complete(people);
        CHECK_STR(slack_read(&s->state, s->workspace, s->channel)->marked, "1712345678.000001");
        request_cancel(&s->requests[TAG_DM]); complete(dm); complete(history);
    }
    teardown(s);
}
static void ordinary_loads_survive_stream_end(void) {
    setup(); SlackScreen *s = (SlackScreen *)slack_screen_new();
    apply(s, TAG_WORKSPACES, "slack_workspaces", SLACK_WORKSPACES, s->state.generation, 0); choose(s, ACT_WORKSPACE, 0);
    ApiError error; api_error_init(&error);
    const int statuses[] = {409, 503, 0};
    for (size_t i = 0; i < sizeof statuses / sizeof *statuses; i++) {
        Request *list = pending(s, TAG_CONVERSATIONS, "slack_conversations", SLACK_CONVERSATIONS);
        api_error_set(&error, statuses[i] ? API_HTTP : API_NETWORK, statuses[i], "Disconnected", -1);
        slack_inbox_stream_end(s, s->stream_generation, &error);
        CHECK(!list->cancelled); CHECK(s->requests[TAG_CONVERSATIONS] == list); complete(list);
        CHECK_INT(json_count(s->conversations), 4); choose(s, ACT_CHANNEL, 0);
        Request *detail = pending(s, TAG_DETAIL, "slack_conversation", "{\"conversation\":{\"id\":\"C1\"}}");
        Request *history = pending(s, TAG_HISTORY, "slack_history", SLACK_HISTORY);
        slack_inbox_stream_end(s, s->stream_generation, &error);
        CHECK(!detail->cancelled); CHECK(!history->cancelled); complete(detail); complete(history);
        CHECK(s->detail != NULL); CHECK(s->history_loaded); CHECK_INT(s->state.message_count, 2);
    }
    // Snapshot-owned reads are discarded when their stream ends.
    live(s, "ready", LIVE_READY);
    Request *snapshot = pending(s, TAG_CONVERSATIONS, "slack_conversations", SLACK_CONVERSATIONS);
    Request *detail = pending(s, TAG_DETAIL, "slack_conversation", "{\"conversation\":{\"id\":\"C1\"}}");
    slack_inbox_stream_end(s, s->stream_generation, &error);
    CHECK(snapshot->cancelled); CHECK(detail->cancelled); CHECK(!s->reconciling);
    complete(snapshot); complete(detail); api_error_clear(&error); teardown(s);
}
static void workspace_reload_backoff(void) {
    setup(); SlackScreen *s = (SlackScreen *)slack_screen_new(); load_navigation(s);
    live(s, "workspace.changed", "{\"workspaceId\":1727000000002}"); CHECK(s->workspace_reload);
    const int statuses[] = {503, 0, 502, 429};
    for (size_t i = 0; i < sizeof statuses / sizeof *statuses; i++) {
        uint64_t start = GetTickCount64();
        apply(s, TAG_WORKSPACES, "slack_workspaces", "{}", s->state.generation, statuses[i]);
        CHECK(s->workspace_reload); CHECK_INT(s->stream_failures, i + 1);
        CHECK(s->reconnect_at >= start + slack_reconnect_delay((unsigned)i + 1, statuses[i] == 429 ? 60 : -1));
        CHECK(s->requests[TAG_WORKSPACES] == NULL);
    }
    Request *network = pending(s, TAG_WORKSPACES, "slack_workspaces", NULL); network->ok = false;
    api_error_set(&network->error, API_NETWORK, 0, "Connection refused", -1);
    uint64_t start = GetTickCount64(); complete(network);
    CHECK(s->workspace_reload); CHECK_INT(s->stream_failures, 5); CHECK(s->reconnect_at >= start + 16000);
    s->cooldown = 0; apply(s, TAG_WORKSPACES, "slack_workspaces", SLACK_WORKSPACES, s->state.generation, 0);
    CHECK(!s->workspace_reload); CHECK_INT(s->stream_failures, 0); CHECK_INT(s->reconnect_at, 0);
    teardown(s);
}
static void broadcast_opens_parent_thread(void) {
    setup(); SlackScreen *s = (SlackScreen *)slack_screen_new(); load_navigation(s);
    apply(s, TAG_HISTORY, "slack_history", "{\"messages\":[{\"ts\":\"1712345679.000001\",\"thread_ts\":\"1712345678.000001\",\"subtype\":\"thread_broadcast\",\"text\":\"broadcast\"}],\"nextCursor\":\"\",\"hasMore\":false}", s->state.generation, 0);
    CHECK(slack_message_in_thread(&s->state.messages[0], s->workspace, s->channel, NULL));
    choose(s, ACT_THREAD, 0); CHECK_STR(s->thread, "1712345678.000001");
    apply(s, TAG_HISTORY, "slack_thread", SLACK_THREAD, s->state.generation, 0);
    CHECK_INT(s->state.message_count, 2);
    SlackDraft *d = slack_draft(&s->state, s->workspace, s->channel, s->thread); CHECK_STR(d->thread, "1712345678.000001");
    set_string(&d->text, "human reply"); CHECK(slack_draft_begin(d));
    apply(s, TAG_SEND, "slack_send", SLACK_RECEIPT, s->state.generation, 0);
    const SlackMessage *sent = find_message(s, "1712345680.000001"); CHECK(sent != NULL);
    CHECK_STR(json_str(json_get(sent ? sent->raw : NULL, "thread_ts")), "1712345678.000001");
    teardown(s);
}
static void ready_snapshot_ordering(void) {
    setup(); SlackScreen *s = (SlackScreen *)slack_screen_new(); load_navigation(s);
    apply(s, TAG_HISTORY, "slack_history", SLACK_HISTORY, s->state.generation, 0);
    uint64_t before = s->state.generation;
    live(s, "ready", LIVE_READY); CHECK(s->reconciling); CHECK(s->state.generation > before);
    live(s, "message", LIVE_MESSAGE); live(s, "message", LIVE_MESSAGE); live(s, "message.changed", LIVE_EDIT);
    live(s, "message.deleted", "{\"workspaceId\":1727000000002,\"event\":{\"channel\":\"C1\",\"deleted_ts\":\"1712345678.000002\"}}");
    live(s, "message.changed", "{\"workspaceId\":1727000000002,\"event\":{\"channel\":\"C1\",\"subtype\":\"message_replied\",\"message\":{\"ts\":\"1712345678.000001\",\"reply_count\":9}}}");
    live(s, "conversation.read", "{\"workspaceId\":1727000000002,\"channel\":\"C1\",\"ts\":\"1712345678.000003\"}");
    CHECK_INT(s->state.message_count, 2); // Old UI remains until all authoritative snapshots complete.
    apply(s, TAG_SNAPSHOT_CHANNEL, "slack_history", SLACK_HISTORY, s->state.generation, 0);
    apply(s, TAG_CONVERSATIONS, "slack_conversations", SLACK_CONVERSATIONS, s->state.generation, 0); CHECK(s->reconciling);
    apply(s, TAG_DETAIL, "slack_conversation", "{\"conversation\":{\"id\":\"C1\",\"last_read\":\"1712345678.000001\"}}", s->state.generation, 0);
    CHECK(!s->reconciling); CHECK_INT(s->state.message_count, 2); CHECK(!find_message(s, "1712345678.000002"));
    const SlackMessage *own = find_message(s, "1712345678.000003"); CHECK(own != NULL);
    CHECK_STR(json_str(json_get(own ? own->raw : NULL, "text")), "edited own");
    CHECK_INT(json_int_or(json_get(find_message(s, "1712345678.000001")->raw, "reply_count"), 0), 9);
    CHECK_STR(slack_read(&s->state, s->workspace, s->channel)->marked, "1712345678.000003");
    // Another ready is always a new reconciliation, even without disconnect or a durable cursor.
    uint64_t old = s->state.generation; live(s, "ready", LIVE_READY); CHECK(s->reconciling);
    apply(s, TAG_SNAPSHOT_CHANNEL, "slack_history", "{\"messages\":[{\"ts\":\"9999.1\"}],\"nextCursor\":\"\",\"hasMore\":false}", old, 0);
    CHECK_INT(s->snapshot.message_count, 0);
    snapshots(s, SLACK_HISTORY); CHECK(!s->reconciling); CHECK(!find_message(s, "9999.1")); teardown(s);
}
static void disconnect_catchup_thread(void) {
    setup(); SlackScreen *s = (SlackScreen *)slack_screen_new(); load_navigation(s);
    apply(s, TAG_HISTORY, "slack_history", SLACK_HISTORY, s->state.generation, 0); choose(s, ACT_THREAD, 0);
    ApiError error; api_error_init(&error); api_error_set(&error, API_NETWORK, 0, "lost connection", -1);
    slack_inbox_stream_end(s, s->stream_generation, &error); CHECK(s->reconnect_at > GetTickCount64()); CHECK_INT(s->stream_failures, 1);
    live(s, "ready", LIVE_READY); CHECK(s->snapshot_pending & (1u << TAG_SNAPSHOT_THREAD)); CHECK_INT(s->stream_failures, 1);
    // Messages accumulated during a long disconnect span multiple pages back to the loaded oldest message.
    apply(s, TAG_SNAPSHOT_CHANNEL, "slack_history", "{\"messages\":[{\"ts\":\"1712345680.1\",\"text\":\"missed bot\",\"bot_id\":\"B1\"}],\"nextCursor\":\"catchup\",\"hasMore\":true}", s->state.generation, 0);
    CHECK(s->snapshot_pending & (1u << TAG_SNAPSHOT_CHANNEL)); CHECK_STR(s->snapshot_channel.cursor, "catchup");
    apply(s, TAG_SNAPSHOT_THREAD, "slack_thread", SLACK_EMPTY_PAGE, s->state.generation, 0); CHECK(s->reconciling);
    apply(s, TAG_SNAPSHOT_THREAD, "slack_thread", SLACK_THREAD, s->state.generation, 0);
    snapshots(s, SLACK_HISTORY); CHECK(!s->reconciling); CHECK_INT(s->stream_failures, 0);
    CHECK(find_message(s, "1712345680.1") != NULL); CHECK_STR(s->thread, "1712345678.000001"); CHECK_INT(s->state.message_count, 4);
    api_error_clear(&error); teardown(s);
}
static void stream_overflow_and_mailbox(void) {
    setup(); SlackScreen *s = (SlackScreen *)slack_screen_new(); load_navigation(s);
    live(s, "ready", LIVE_READY);
    uint64_t old = s->stream_generation, old_state = s->state.generation;
    for (size_t i = 0; i <= SLACK_EVENTS_MAX; i++) live(s, "message", LIVE_MESSAGE);
    CHECK(!s->reconciling); CHECK(s->stream_generation > old); CHECK(s->reconnect_at > GetTickCount64()); CHECK_INT(s->buffered.count, 0);
    // A fresh connection reloads instead of trusting the incomplete snapshots/buffer.
    live(s, "ready", LIVE_READY); CHECK(s->state.generation > old_state); snapshots(s, SLACK_HISTORY); CHECK(!s->reconciling);
    SlackFeed *feed = slack_feed_new(g_store.client, s->workspace, s->stream_generation); s->feed = feed; s->shown = true;
    slack_feed_emit(feed, "ready", LIVE_READY, strlen(LIVE_READY)); slack_feed_emit(feed, "message", LIVE_MESSAGE, strlen(LIVE_MESSAGE));
    slack_inbox_pump(s); CHECK(s->reconciling); CHECK_INT(s->buffered.count, 1);
    for (size_t i = 0; i <= SLACK_EVENTS_MAX; i++) slack_feed_emit(feed, "message", LIVE_MESSAGE, strlen(LIVE_MESSAGE));
    slack_inbox_pump(s); CHECK(!s->feed); CHECK(!s->reconciling); CHECK(s->stream_failures > 0);
    // Byte overflow has the same terminal signal, even for one huge event.
    feed = slack_feed_new(g_store.client, s->workspace, 77);
    slack_feed_emit(feed, "message", "", SLACK_EVENTS_BYTES);
    SlackEvents q = {0}; ApiError error; api_error_init(&error); uint64_t generation;
    CHECK(!slack_feed_take(feed, &q, &error, &generation)); CHECK(q.overflow); CHECK_INT(generation, 77);
    slack_events_clear(&q); slack_feed_end(feed, &error); CHECK(slack_feed_take(feed, &q, &error, &generation));
    slack_events_clear(&q); slack_feed_stop(&feed); CHECK(!feed); api_error_clear(&error); teardown(s);
}
static void stream_setup_backoff_and_stale(void) {
    setup(); SlackScreen *s = (SlackScreen *)slack_screen_new(); load_navigation(s);
    apply(s, TAG_HISTORY, "slack_history", SLACK_HISTORY, s->state.generation, 0);
    SlackDraft *d = slack_draft(&s->state, s->workspace, s->channel, NULL); free(d->text); d->text = xstrdup("manual reply");
    ApiError error; api_error_init(&error); api_error_set(&error, API_HTTP, 409, "Set signing secret", -1);
    slack_inbox_stream_end(s, s->stream_generation, &error); CHECK(s->stream_disabled); CHECK_STR(s->workspace, "1727000000002");
    CHECK_INT(s->state.message_count, 2); CHECK_STR(d->text, "manual reply"); CHECK(strstr(s->live_status, "Event Subscriptions") != NULL);
    CHECK(slack_draft_begin(d)); apply(s, TAG_SEND, "slack_send", SLACK_RECEIPT, s->state.generation, 0); CHECK_STR(d->text, "");
    uint64_t old = s->stream_generation; choose(s, ACT_REFRESH, 0); CHECK(s->stream_generation > old); CHECK(s->stream_disabled);
    slack_inbox_event(s, old, "message", LIVE_MESSAGE, strlen(LIVE_MESSAGE)); CHECK_INT(s->state.message_count, 0);
    api_error_set(&error, API_HTTP, 429, "rate limited", 120);
    uint64_t start = GetTickCount64(); slack_inbox_stream_end(s, s->stream_generation, &error); CHECK(s->reconnect_at >= start + 120000);
    CHECK_INT(s->stream_failures, 1); slack_inbox_stream_end(s, s->stream_generation, &error); CHECK_INT(s->stream_failures, 2);
    api_error_set(&error, API_HTTP, 503, "server down", -1); slack_inbox_stream_end(s, s->stream_generation, &error); CHECK_INT(s->stream_failures, 3);
    CHECK(s->reconnect_at >= GetTickCount64() + 3990);
    api_error_clear(&error); teardown(s);
}
static void workspace_rotation_removal_revocation(void) {
    setup(); SlackScreen *s = (SlackScreen *)slack_screen_new(); load_navigation(s);
    live(s, "ready", LIVE_READY); uint64_t old = s->stream_generation, request_generation = s->state.generation;
    live(s, "workspace.changed", "{\"workspaceId\":1727000000002}"); CHECK(s->workspace_reload); CHECK(!s->reconciling);
    CHECK_INT(s->state.message_count, 0); CHECK_INT(s->state.draft_count, 0); CHECK_INT(json_count(s->people), 0);
    CHECK_STR(s->workspace, "1727000000002"); CHECK(s->stream_generation > old);
    slack_inbox_event(s, old, "ready", LIVE_READY, strlen(LIVE_READY)); CHECK(!s->reconciling);
    apply(s, TAG_SNAPSHOT_CHANNEL, "slack_history", SLACK_HISTORY, request_generation, 0); CHECK_INT(s->state.message_count, 0);
    apply(s, TAG_WORKSPACES, "slack_workspaces", SLACK_WORKSPACES, s->state.generation, 0); CHECK(!s->workspace_reload);
    live(s, "ready", LIVE_READY); snapshots(s, SLACK_HISTORY); // Channel cleared by rotation; directory-only reconciliation.
    live(s, "workspace.removed", "{\"workspaceId\":1727000000002}"); CHECK(!s->workspace); CHECK_INT(json_count(s->conversations), 0);
    load_navigation(s); live(s, "ready", "{\"workspaceId\":1727000000002,\"userId\":\"U2\",\"refresh\":true}"); CHECK(s->workspace_reload);
    CHECK_INT(s->state.message_count, 0); apply(s, TAG_WORKSPACES, "slack_workspaces", "{\"workspaces\":[]}", s->state.generation, 0); CHECK(!s->workspace);
    load_navigation(s); ApiError error; api_error_init(&error); api_error_set(&error, API_HTTP, 403, "Admin access revoked", -1);
    slack_inbox_stream_end(s, s->stream_generation, &error); CHECK(!s->workspace); CHECK_INT(s->state.read_count, 0); CHECK(!s->feed);
    load_navigation(s); slack_inbox_settings_changed(); CHECK(!s->workspace); CHECK(!s->feed); CHECK_INT(json_count(s->workspaces), 0);
    load_navigation(s); s->feed = slack_feed_new(g_store.client, s->workspace, s->stream_generation);
    old = s->stream_generation; s->base.vt->visible(&s->base, false); CHECK(!s->feed); CHECK(s->stream_generation > old);
    slack_inbox_event(s, old, "message", LIVE_MESSAGE, strlen(LIVE_MESSAGE)); CHECK_INT(s->state.message_count, 0);
    api_error_clear(&error); teardown(s);
}
static void own_receipts_after_events(void) {
    setup(); SlackScreen *s = (SlackScreen *)slack_screen_new(); load_navigation(s);
    const char *receipt = "{\"channel\":\"C1\",\"ts\":\"1712345678.000003\",\"message\":{\"text\":\"human reply\"}}";
    SlackDraft *d = slack_draft(&s->state, s->workspace, s->channel, NULL); free(d->text); d->text = xstrdup("human reply"); CHECK(slack_draft_begin(d));
    live(s, "message", LIVE_MESSAGE); live(s, "message.changed", LIVE_EDIT);
    apply(s, TAG_SEND, "slack_send", receipt, s->state.generation, 0); CHECK_INT(s->state.message_count, 1);
    CHECK_STR(json_str(json_get(s->state.messages[0].raw, "text")), "edited own"); CHECK_STR(d->text, "");
    free(d->text); d->text = xstrdup("human reply"); CHECK(slack_draft_begin(d)); live(s, "message.deleted", LIVE_DELETE);
    apply(s, TAG_SEND, "slack_send", receipt, s->state.generation, 0); CHECK_INT(s->state.message_count, 0); CHECK_STR(d->text, "");
    teardown(s);
}
static void own_receipt_preserves_snapshot_edit(void) {
    setup(); SlackScreen *s = (SlackScreen *)slack_screen_new(); load_navigation(s);
    SlackDraft *d = slack_draft(&s->state, s->workspace, s->channel, NULL);
    set_string(&d->text, "human reply"); CHECK(slack_draft_begin(d));
    Request *send = pending(s, TAG_SEND, "slack_send", "{\"channel\":\"C1\",\"ts\":\"1712345678.000003\",\"message\":{\"text\":\"human reply\"}}");
    live(s, "ready", LIVE_READY);
    // The edit predates ready, so only the history snapshot carries it; no edit event is buffered.
    apply(s, TAG_SNAPSHOT_CHANNEL, "slack_history", "{\"messages\":[{\"ts\":\"1712345678.000003\",\"text\":\"edited own\",\"edited\":{\"ts\":\"1712345679.000001\"}}],\"nextCursor\":\"\",\"hasMore\":false}", s->state.generation, 0);
    CHECK(s->reconciling); CHECK_INT(s->snapshot.message_count, 1); CHECK_INT(s->buffered.count, 0);
    complete(send);
    CHECK_STR(d->text, ""); CHECK(!d->sending); CHECK(!d->uncertain);
    CHECK_INT(s->snapshot.message_count, 1);
    CHECK_STR(json_str(json_get(s->snapshot.messages[0].raw, "text")), "edited own");
    apply(s, TAG_CONVERSATIONS, "slack_conversations", SLACK_CONVERSATIONS, s->state.generation, 0);
    apply(s, TAG_DETAIL, "slack_conversation", "{\"conversation\":{\"id\":\"C1\"}}", s->state.generation, 0);
    CHECK(!s->reconciling); CHECK_INT(s->state.message_count, 1);
    const SlackMessage *own = find_message(s, "1712345678.000003"); CHECK(own != NULL);
    CHECK_STR(json_str(json_get(own ? own->raw : NULL, "text")), "edited own");
    CHECK_STR(json_str(json_get(json_get(own ? own->raw : NULL, "edited"), "ts")), "1712345679.000001");
    teardown(s);
}
static void snapshot_read_sync_and_private_access(void) {
    setup(); SlackScreen *s = (SlackScreen *)slack_screen_new(); load_navigation(s);
    live(s, "conversation.read", "{\"workspaceId\":1727000000002,\"channel\":\"C1\",\"ts\":\"1712345680.1\"}");
    apply(s, TAG_DETAIL, "slack_conversation", "{\"conversation\":{\"id\":\"C1\",\"last_read\":\"1712345678.000001\"}}", s->state.generation, 0);
    SlackRead *r = slack_read(&s->state, s->workspace, s->channel); slack_read_viewed(r, "1712345678.000002", 0); CHECK(slack_read_due(r, 2000) == NULL);
    choose(s, ACT_THREAD, 0); // No messages yet, so this action has no target.
    slack_draft(&s->state, s->workspace, "G1", NULL);
    live(s, "ready", LIVE_READY); apply(s, TAG_CONVERSATIONS, "slack_conversations", "{\"conversations\":[{\"id\":\"C1\"}],\"nextCursor\":\"\"}", s->state.generation, 0);
    apply(s, TAG_DETAIL, "slack_conversation", "{\"conversation\":{\"id\":\"C1\",\"last_read\":\"1712345678.000001\"}}", s->state.generation, 0);
    apply(s, TAG_SNAPSHOT_CHANNEL, "slack_history", SLACK_HISTORY, s->state.generation, 0);
    CHECK(!s->reconciling); CHECK_INT(s->state.draft_count, 0); CHECK_STR(slack_read(&s->state, s->workspace, s->channel)->marked, "1712345680.1");
    // A refused snapshot is credential rotation, not the unsigned-events 409 fallback.
    live(s, "ready", LIVE_READY); apply(s, TAG_SNAPSHOT_CHANNEL, "slack_history", NULL, s->state.generation, 409);
    CHECK(!s->workspace); CHECK(!s->stream_disabled); CHECK_INT(s->state.message_count, 0); teardown(s);
}

static void recovery_revalidates_after_modal_events(void) {
    setup(); SlackScreen *s = (SlackScreen *)slack_screen_new(); load_navigation(s);
    Json *destination = json_object(); json_set_str(destination, "id", s->workspace); json_set_str(destination, "channel", s->channel);
    SlackDraft *d = slack_draft(&s->state, s->workspace, s->channel, NULL); d->uncertain = true;
    uint64_t generation = s->state.generation;
    slack_inbox_recover_confirmed(s, destination, generation); CHECK(!d->uncertain);
    d->uncertain = true;
    live(s, "workspace.removed", "{\"workspaceId\":1727000000002}");
    slack_inbox_recover_confirmed(s, destination, generation); CHECK_INT(s->state.draft_count, 0);
    load_navigation(s); d = slack_draft(&s->state, s->workspace, s->channel, NULL); d->uncertain = true;
    generation = s->state.generation; live(s, "ready", LIVE_READY);
    slack_inbox_recover_confirmed(s, destination, generation); CHECK(d->uncertain);
    json_set_str(destination, "channel", "G1"); slack_inbox_recover_confirmed(s, destination, s->state.generation); CHECK(d->uncertain);
    json_free(destination); teardown(s);
}
static void initial_empty_snapshot_and_signout(void) {
    setup(); SlackScreen *s = (SlackScreen *)slack_screen_new(); load_navigation(s);
    live(s, "ready", LIVE_READY); CHECK(s->snapshot_edge == NULL);
    apply(s, TAG_SNAPSHOT_CHANNEL, "slack_history", SLACK_EMPTY_PAGE, s->state.generation, 0);
    CHECK(s->snapshot_pending & (1u << TAG_SNAPSHOT_CHANNEL)); CHECK_STR(s->snapshot_channel.cursor, "next");
    snapshots(s, SLACK_HISTORY); CHECK(!s->reconciling); CHECK_INT(s->state.message_count, 2);
    s->shown = true; s->feed = slack_feed_new(g_store.client, s->workspace, s->stream_generation);
    uint64_t old_stream = s->stream_generation, old_request = s->state.generation;
    ApiClient *client = g_store.client; g_store.client = NULL; g_store.has_device = false;
    slack_inbox_store_changed(); CHECK(!s->feed); CHECK(!s->workspace); CHECK_INT(s->state.message_count, 0);
    slack_inbox_event(s, old_stream, "message", LIVE_MESSAGE, strlen(LIVE_MESSAGE));
    apply(s, TAG_HISTORY, "slack_history", SLACK_HISTORY, old_request, 0); CHECK_INT(s->state.message_count, 0);
    api_client_release(client); teardown(s);
}
static void one_owner_even_during_backoff(void) {
    setup(); SlackScreen *first = (SlackScreen *)slack_screen_new(); load_navigation(first);
    first->feed = slack_feed_new(g_store.client, first->workspace, first->stream_generation);
    ApiError error; api_error_init(&error); api_error_set(&error, API_NETWORK, 0, "disconnected", -1);
    slack_inbox_stream_end(first, first->stream_generation, &error); CHECK(first->reconnect_at > GetTickCount64());
    uint64_t generation = first->state.generation, stream_generation = first->stream_generation;
    SlackDraft *d = slack_draft(&first->state, first->workspace, first->channel, NULL);
    set_string(&d->text, "human reply"); CHECK(slack_draft_begin(d));
    Request *send = pending(first, TAG_SEND, "slack_send", SLACK_RECEIPT);
    Request *history = pending(first, TAG_HISTORY, "slack_history", SLACK_HISTORY);
    Request *detail = pending(first, TAG_DETAIL, "slack_conversation", "{\"conversation\":{\"id\":\"C1\"}}");
    SlackScreen *second = (SlackScreen *)slack_screen_new(); load_navigation(second);
    CHECK_INT(first->state.generation, generation); CHECK(first->stream_generation > stream_generation);
    CHECK(!send->cancelled); CHECK(!history->cancelled); CHECK(!detail->cancelled); CHECK(d->sending); CHECK(!d->uncertain);
    complete(history); complete(detail); complete(send);
    CHECK(first->history_loaded); CHECK(first->detail != NULL); CHECK_STR(d->text, ""); CHECK(!d->uncertain);
    CHECK(find_message(first, "1712345680.000001") != NULL);
    CHECK(!first->feed); CHECK(strstr(first->live_status, "active Slack inbox") != NULL);
    second->feed = slack_feed_new(g_store.client, second->workspace, second->stream_generation);
    slack_inbox_focus(first); CHECK(!second->feed); CHECK(strstr(second->live_status, "active Slack inbox") != NULL);
    slack_inbox_event(first, stream_generation, "message", LIVE_MESSAGE, strlen(LIVE_MESSAGE)); CHECK_INT(first->state.message_count, 3);
    CHECK(find_message(first, "1712345678.000003") == NULL);
    api_error_clear(&error); second->base.vt->destroy(&second->base); teardown(first);
}
static void ownership_loss_during_snapshot(void) {
    for (int thread = 0; thread < 2; thread++) {
        setup(); SlackScreen *first = (SlackScreen *)slack_screen_new(); load_navigation(first);
        if (thread) set_string(&first->thread, "1712345678.000001");
        SlackScreen *second = (SlackScreen *)slack_screen_new(); load_navigation(second); slack_inbox_focus(first);
        request_pane(first);
        live(first, "ready", LIVE_READY); CHECK(first->reconciling);
        uint64_t generation = first->state.generation, stream_generation = first->stream_generation;
        Request *snapshot = first->requests[TAG_CONVERSATIONS]; CHECK(snapshot != NULL);
        SlackDraft *d = slack_draft(&first->state, first->workspace, first->channel, first->thread);
        set_string(&d->text, "human reply"); CHECK(slack_draft_begin(d));
        Request *send = pending(first, TAG_SEND, "slack_send", SLACK_RECEIPT);
        if (thread) json_set_str(send->args, "threadTs", first->thread);
        slack_inbox_focus(second);
        CHECK(!first->reconciling); if (snapshot) CHECK(snapshot->cancelled); CHECK(!first->feed);
        CHECK_INT(first->state.generation, generation); CHECK(!send->cancelled); CHECK(d->sending); CHECK(!d->uncertain);
        CHECK(first->requests[TAG_CONVERSATIONS] != NULL); CHECK(first->requests[TAG_CONVERSATIONS] != snapshot);
        CHECK(first->requests[TAG_DETAIL] != NULL); CHECK(first->requests[TAG_HISTORY] != NULL);
        if (first->requests[TAG_HISTORY]) CHECK_STR(first->requests[TAG_HISTORY]->operation, thread ? "slack_thread" : "slack_history");
        CHECK(strstr(first->live_status, "active Slack inbox") != NULL);
        slack_inbox_event(first, stream_generation, "message", LIVE_MESSAGE, strlen(LIVE_MESSAGE));
        CHECK_INT(first->state.message_count, 0);
        deliver_inbox_requests(thread ? 7 : 6);
        CHECK_INT(json_count(first->conversations), 4); CHECK(first->detail != NULL); CHECK(first->history_loaded);
        CHECK_INT(first->state.message_count, 2); complete(send); CHECK_STR(d->text, ""); CHECK(!d->uncertain);
        first->base.pane = NULL;
        second->base.vt->destroy(&second->base); teardown(first);
    }
}
static void ownership_loss_during_workspace_reload(void) {
    setup(); SlackScreen *first = (SlackScreen *)slack_screen_new(); load_navigation(first);
    live(first, "workspace.changed", "{\"workspaceId\":1727000000002}"); CHECK(first->workspace_reload);
    SlackScreen *second = (SlackScreen *)slack_screen_new(); load_navigation(second);
    second->feed = slack_feed_new(g_store.client, second->workspace, second->stream_generation);
    SlackFeed *feed = second->feed; uint64_t generation = second->stream_generation;
    request_pane(first);
    apply(first, TAG_WORKSPACES, "slack_workspaces", SLACK_WORKSPACES, first->state.generation, 0);
    CHECK(!first->workspace_reload); CHECK(!first->feed);
    CHECK(strstr(first->live_status, "active Slack inbox") != NULL);
    CHECK(first->requests[TAG_CONVERSATIONS] != NULL); CHECK(first->requests[TAG_PEOPLE] != NULL);
    CHECK(second->feed == feed); CHECK_INT(second->stream_generation, generation);
    deliver_inbox_requests(2); CHECK_INT(json_count(first->conversations), 4); CHECK(json_count(first->people) > 0);
    first->base.pane = NULL;
    second->base.vt->destroy(&second->base); teardown(first);
}
static void reconcile_preserves_composer_editing(void) {
    for (int thread = 0; thread < 2; thread++) {
        setup(); SlackScreen *s = (SlackScreen *)slack_screen_new(); load_navigation(s);
        if (thread) set_string(&s->thread, "1712345678.000001");
        s->composer = CreateWindowExW(0, L"EDIT", L"typed draft", WS_CHILD | ES_MULTILINE,
            0, 0, 100, 100, inbox_hwnd, NULL, GetModuleHandleW(NULL), NULL);
        CHECK(s->composer != NULL);
        SendMessageW(s->composer, EM_SETSEL, 11, 11);
        SendMessageW(s->composer, EM_REPLACESEL, TRUE, (LPARAM)L"xyz");
        s->base.pane = inbox_pane;
        s->base.vt->command(&s->base, ID_COMPOSER, EN_CHANGE, s->composer);
        s->base.pane = NULL;
        SendMessageW(s->composer, EM_SETSEL, 1, 4);
        CHECK(SendMessageW(s->composer, EM_CANUNDO, 0, 0));
        EnableWindow(s->composer, FALSE);
        live(s, "ready", LIVE_READY); snapshots(s, SLACK_HISTORY);
        if (thread) apply(s, TAG_SNAPSHOT_THREAD, "slack_thread", SLACK_THREAD, s->state.generation, 0);
        CHECK(!s->reconciling); CHECK(IsWindowEnabled(s->composer));
        DWORD start = 0, end = 0;
        SendMessageW(s->composer, EM_GETSEL, (WPARAM)&start, (LPARAM)&end);
        CHECK_INT(start, 1); CHECK_INT(end, 4);
        CHECK_STR(slack_draft(&s->state, s->workspace, s->channel, s->thread)->text, "typed draftxyz");
        CHECK(SendMessageW(s->composer, EM_CANUNDO, 0, 0));
        CHECK(SendMessageW(s->composer, EM_UNDO, 0, 0));
        wchar_t text[32]; GetWindowTextW(s->composer, text, 32);
        CHECK(wcscmp(text, L"typed draft") == 0);
        // Revoked destinations must still clear and disable their composer.
        live(s, "ready", LIVE_READY);
        apply(s, TAG_CONVERSATIONS, "slack_conversations", "{\"conversations\":[],\"nextCursor\":\"\"}", s->state.generation, 0);
        apply(s, TAG_DETAIL, "slack_conversation", "{\"conversation\":{\"id\":\"C1\"}}", s->state.generation, 0);
        apply(s, TAG_SNAPSHOT_CHANNEL, "slack_history", SLACK_HISTORY, s->state.generation, 0);
        if (thread) apply(s, TAG_SNAPSHOT_THREAD, "slack_thread", SLACK_THREAD, s->state.generation, 0);
        CHECK(!s->channel); CHECK_INT(s->state.draft_count, 0);
        CHECK_INT(GetWindowTextLengthW(s->composer), 0); CHECK(!IsWindowEnabled(s->composer));
        teardown(s);
    }
}
static void interrupted_workspace_reload_rearms_timer(void) {
    for (int hidden = 0; hidden < 2; hidden++) {
        setup(); SlackScreen *first = (SlackScreen *)slack_screen_new(); load_navigation(first);
        SlackScreen *second = (SlackScreen *)slack_screen_new(); load_navigation(second); slack_inbox_focus(first);
        request_pane(first);
        live(first, "workspace.changed", "{\"workspaceId\":1727000000002}"); CHECK(first->requests[TAG_WORKSPACES] != NULL);
        if (hidden) first->base.vt->visible(&first->base, false);
        else {
            request_cancel(&first->requests[TAG_WORKSPACES]);
            slack_inbox_focus(first); slack_inbox_focus(second);
        }
        CHECK(first->workspace_reload); CHECK(!first->requests[TAG_WORKSPACES]);
        CHECK(!KillTimer(pane_hwnd(inbox_pane), TIMER_LIVE));
        deliver_inbox_requests(1);
        apply(first, TAG_WORKSPACES, "slack_workspaces", NULL, first->state.generation, 503);
        if (hidden) first->base.vt->visible(&first->base, true);
        else first->base.vt->refresh(&first->base);
        CHECK(KillTimer(pane_hwnd(inbox_pane), TIMER_LIVE));
        first->base.vt->timer(&first->base, TIMER_LIVE); CHECK(!first->requests[TAG_WORKSPACES]);
        deliver_inbox_requests(2);
        first->reconnect_at = 0; first->cooldown = GetTickCount64() + 60000;
        first->base.vt->timer(&first->base, TIMER_LIVE); CHECK(!first->requests[TAG_WORKSPACES]);
        first->cooldown = 0;
        first->base.vt->timer(&first->base, TIMER_LIVE); CHECK(first->requests[TAG_WORKSPACES] != NULL);
        // Another active inbox prevents a real event reader from starting in this fixture.
        slack_inbox_focus(second);
        deliver_inbox_requests(1); CHECK(!first->workspace_reload);
        deliver_inbox_requests(2); CHECK_INT(json_count(first->conversations), 4);
        CHECK(strstr(first->live_status, "active Slack inbox") != NULL);
        first->base.pane = NULL;
        second->base.vt->destroy(&second->base); teardown(first);
    }
}
static void shown_during_cooldown_rearms_timer(void) {
    for (int other_owner = 0; other_owner < 2; other_owner++) {
        setup(); SlackScreen *s = (SlackScreen *)slack_screen_new(); load_navigation(s); request_pane(s);
        s->feed = slack_feed_new(g_store.client, s->workspace, s->stream_generation);
        SetTimer(pane_hwnd(inbox_pane), TIMER_LIVE, 100, NULL);
        apply(s, TAG_HISTORY, "slack_history", NULL, s->state.generation, 429);
        uint64_t cooldown = s->cooldown; CHECK(cooldown > GetTickCount64());
        s->base.vt->visible(&s->base, false); CHECK(!s->feed);
        CHECK(!KillTimer(pane_hwnd(inbox_pane), TIMER_LIVE));
        SlackScreen *second = NULL;
        if (other_owner) {
            second = (SlackScreen *)slack_screen_new(); load_navigation(second);
            second->feed = slack_feed_new(g_store.client, second->workspace, second->stream_generation);
        }
        s->base.vt->visible(&s->base, true);
        CHECK_INT(s->cooldown, cooldown); CHECK(!s->feed);
        if (other_owner) {
            CHECK(!KillTimer(pane_hwnd(inbox_pane), TIMER_LIVE)); CHECK(second->feed != NULL);
            CHECK(strstr(s->live_status, "active Slack inbox") != NULL);
        } else {
            CHECK(KillTimer(pane_hwnd(inbox_pane), TIMER_LIVE));
            s->base.vt->timer(&s->base, TIMER_LIVE);
            CHECK(!s->feed); CHECK(KillTimer(pane_hwnd(inbox_pane), TIMER_LIVE));
        }
        for (int tag = 0; tag < TAG_COUNT; tag++) CHECK(!s->requests[tag]);
        s->base.pane = NULL;
        if (second) second->base.vt->destroy(&second->base);
        teardown(s);
    }
}
static void removed_conversation_rejects_pending_receipt(void) {
    setup(); SlackScreen *s = (SlackScreen *)slack_screen_new(); load_navigation(s); live(s, "ready", LIVE_READY);
    SlackDraft *d = slack_draft(&s->state, s->workspace, s->channel, NULL); set_string(&d->text, "human reply"); CHECK(slack_draft_begin(d));
    Request *req = xcalloc(1, sizeof *req); api_error_init(&req->error);
    req->owner = s; req->done = slack_inbox_done; req->operation = xstrdup("slack_send");
    req->client = api_client_retain(g_store.client); req->slot = &s->requests[TAG_SEND]; s->requests[TAG_SEND] = req;
    uint64_t generation = s->state.generation;
    apply(s, TAG_CONVERSATIONS, "slack_conversations", "{\"conversations\":[{\"id\":\"G1\"}],\"nextCursor\":\"\"}", generation, 0);
    apply(s, TAG_DETAIL, "slack_conversation", "{\"conversation\":{\"id\":\"C1\"}}", generation, 0);
    apply(s, TAG_SNAPSHOT_CHANNEL, "slack_history", SLACK_HISTORY, generation, 0);
    CHECK(!s->channel); CHECK(!s->reconciling); CHECK(req->cancelled); CHECK(s->requests[TAG_SEND] == NULL);
    CHECK_INT(s->state.message_count, 0); CHECK_INT(s->state.draft_count, 0);
    apply(s, TAG_SEND, "slack_send", SLACK_RECEIPT, generation, 0); CHECK_INT(s->state.message_count, 0); CHECK_INT(s->state.draft_count, 0);
    teardown(s); store_handle_message(WM_APP_REQUEST_DONE, 0, (LPARAM)req);
}
static void stale_rendered_thread_selection(void) {
    setup(); SlackScreen *s = (SlackScreen *)slack_screen_new(); load_navigation(s);
    apply(s, TAG_HISTORY, "slack_history", SLACK_HISTORY, s->state.generation, 0);
    if (!theme.canvas) theme_init();
    Doc doc; doc_init(&doc); doc_begin(&doc, NULL, 600); s->base.vt->layout(&s->base, &doc); doc_end(&doc);
    live(s, "message.deleted", "{\"workspaceId\":1727000000002,\"event\":{\"channel\":\"C1\",\"deleted_ts\":\"1712345678.000001\"}}");
    // The displayed first thread button belongs to the deleted parent, not the new state.messages[0].
    choose(s, ACT_THREAD, 0); CHECK(s->thread == NULL); CHECK_INT(s->state.message_count, 1);
    doc_begin(&doc, NULL, 600); s->base.vt->layout(&s->base, &doc); doc_end(&doc);
    choose(s, ACT_THREAD, 0); CHECK_STR(s->thread, "1712345678.000002");
    doc_free(&doc); teardown(s);
}
void app_slack_tests(void) {
    test_run("Slack Find revalidates access destination generation and screen lifetime", find_revalidates_screen);
    test_run("Slack concurrent rate limits retain the longest deadline and remaining wait", cooldown_keeps_longest_wait);
    test_run("Slack ready preserves pending send receipts and unrelated loads", ready_preserves_pending_receipts);
    test_run("Slack stream failures preserve ordinary list detail and history loads", ordinary_loads_survive_stream_end);
    test_run("Slack workspace reload failures and invalid responses use retry backoff", workspace_reload_backoff);
    test_run("Slack broadcast thread navigation and reply use the parent timestamp", broadcast_opens_parent_thread);
    test_run("Slack stale rendered indices cannot select another thread after live deletion", stale_rendered_thread_selection);
    test_run("Slack removed conversation snapshot invalidates pending receipt and private state", removed_conversation_rejects_pending_receipt);
    test_run("Slack active stream ownership preserves prior inbox receipts and loads during backoff", one_owner_even_during_backoff);
    // A shared message-only pane fixture supports real request slots without visible UI. Create it outside
    // per-test leak checkpoints because pane_create also initializes the process-lifetime pane registry.
    inbox_hwnd = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, GetModuleHandleW(NULL), NULL);
    CHECK(inbox_hwnd != NULL); inbox_pane = pane_create(inbox_hwnd, false);
    test_run("Slack snapshot 429 preserves pages and events then resumes saved cursors after cooldown", snapshot_rate_limit_resumes_progress);
    test_run("Slack ownership loss during channel or thread sync resumes ordinary loads and preserves receipts", ownership_loss_during_snapshot);
    test_run("Slack ownership loss during workspace reload restores list and active inbox status", ownership_loss_during_workspace_reload);
    test_run("Slack completed channel and thread reconciliation preserves composer selection and undo", reconcile_preserves_composer_editing);
    test_run("Slack hidden or de-owned workspace reload resumes its timer and respects retry deadlines", interrupted_workspace_reload_rearms_timer);
    test_run("Slack showing an inbox during cooldown restores its retry timer without stealing ownership", shown_during_cooldown_rearms_timer);
    pane_destroy(inbox_pane); DestroyWindow(inbox_hwnd); inbox_pane = NULL; inbox_hwnd = NULL;
    test_run("Slack initial empty snapshot follows cursor and signout rejects old completions", initial_empty_snapshot_and_signout);
    test_run("Slack modal send recovery rejects removed or reconciled destinations", recovery_revalidates_after_modal_events);
    test_run("Slack every ready snapshots ordered events edits deletes parent and cross-client reads", ready_snapshot_ordering);
    test_run("Slack reconnect catchup pages and open thread snapshots", disconnect_catchup_thread);
    test_run("Slack transport and reconciliation overflow restart without silent drops", stream_overflow_and_mailbox);
    test_run("Slack stream 409 JSON fallback Retry-After backoff and stale generations", stream_setup_backoff_and_stale);
    test_run("Slack stream rotation removal account change settings and cancellation", workspace_rotation_removal_revocation);
    test_run("Slack delayed own receipts cannot overwrite live edits or resurrect deletes", own_receipts_after_events);
    test_run("Slack delayed own receipt preserves an edit already in the reconciliation snapshot", own_receipt_preserves_snapshot_edit);
    test_run("Slack snapshot read synchronization private revocation and JSON 409", snapshot_read_sync_and_private_access);
    test_run("Slack workspace labels use the provider team field", workspace_team_label);
    test_run("Slack recovery revalidates cleared relocated and replacement drafts", recovery_revalidates_draft);
    test_run("Slack viewport debounce ignores hidden background and unfetched messages", viewed_messages_only);
    test_run("Slack screen thread sends hidden cancellation and late completion", cancellation_and_thread_send);
    test_run("Slack admin and catalog gates preserve old servers", permissions);
    test_run("Slack screen destinations directory Open DM drafts and stale results", navigation_and_stale);
    test_run("Slack screen confirmed ambiguous changed sends and private access clear", sends_and_access_clear);
    test_run("Slack screen read errors cooldown and removed workspaces", errors_and_read_marks);
}
