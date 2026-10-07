// Exercise actual inbox completions and navigation without network threads or a native window.
#include "slack_inbox.h"
#include "slack_fixtures.h"
#include "str.h"
#include "suites.h"
#include "test.h"
#include <stdlib.h>
#include <string.h>

static Route routes[] = {
    { "GET", "/slack/workspaces", "admin" },
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

void app_slack_tests(void) {
    test_run("Slack viewport debounce ignores hidden background and unfetched messages", viewed_messages_only);
    test_run("Slack screen thread sends hidden cancellation and late completion", cancellation_and_thread_send);
    test_run("Slack admin and catalog gates preserve old servers", permissions);
    test_run("Slack screen destinations directory Open DM drafts and stale results", navigation_and_stale);
    test_run("Slack screen confirmed ambiguous changed sends and private access clear", sends_and_access_clear);
    test_run("Slack screen read errors cooldown and removed workspaces", errors_and_read_marks);
}
