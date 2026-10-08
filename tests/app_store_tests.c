// store.c without the registry, Credential Manager or network: the poll backoff, what a token may do as g_store holds it,
// and the requests refused before they leave, answered through this thread's own message queue.
#include "api.h"
#include "json.h"
#include "models.h"
#include "store.h"
#include "str.h"
#include "suites.h"
#include "test.h"
#include "fixtures/october-2026/catalog.h"
#include <stdlib.h>
#include <string.h>

static const char *TOKEN = "brm_aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";

static Route ROUTES[] = {
    { "GET", "/sessions", "read" },
    { "POST", "/sessions/{id}/messages", "manage" },
    { "POST", "/uploads", "manage" },
    { "POST", "/transcribe", "manage" },
    { "GET", "/usage/all", "admin" },
};

/// g_store as a paired device with this permission and these routes would have it; the strings are borrowed.
static void store_fake(const char *permission, Route *routes, size_t count) {
    memset(&g_store, 0, sizeof g_store);
    g_store.has_device = true;
    g_store.device.id = "d1"; g_store.device.label = "Test"; g_store.device.permission = (char *)permission;
    g_store.routes = routes; g_store.route_count = count;
    g_store.transcribes = -1;
}
static void store_reset(void) { memset(&g_store, 0, sizeof g_store); }

// Catalog permissions must gate the actual store calls, including older deployments.
static void test_pinned_catalog_gates_existing_calls(void) {
    Str fixture; str_init(&fixture);
    for (size_t i = 0; i < sizeof OCTOBER_CATALOG / sizeof *OCTOBER_CATALOG; i++) str_appendz(&fixture, OCTOBER_CATALOG[i]);
    Json *j = json_parsez(fixture.data); str_free(&fixture);
    Route *routes = NULL; size_t n = 0; CHECK(routes_parse(j, &routes, &n)); json_free(j);
    const struct { const char *call; int least; } calls[] = {
        { "pulls", 0 }, { "sessions", 0 }, { "session", 0 }, { "runtimes", 0 },
        { "repo_tree", 0 }, { "repo_file", 0 }, { "pull_description", 0 }, { "findings", 0 },
        { "start_session", 1 }, { "message", 1 }, { "cancel", 1 }, { "compact", 1 }, { "usage_all", 2 },
    };
    const char *permissions[] = { "read", "manage", "admin" };
    for (size_t i = 0; i < sizeof calls / sizeof *calls; i++) {
        const ApiRoute *operation = api_route(calls[i].call); CHECK(operation != NULL);
        for (int p = 0; p < 3; p++) {
            store_fake(permissions[p], routes, n);
            CHECK(store_supports(calls[i].call) == (p >= calls[i].least));
            store_fake(permissions[p], NULL, 0); CHECK(!store_supports(calls[i].call));
            if (!operation) continue;
            Route *without = xmalloc(n * sizeof *without); size_t count = 0;
            for (size_t k = 0; k < n; k++) {
                // Match a concrete call through the same route matcher, independent of placeholder names.
                if (!str_eq(routes[k].method, operation->method) || !routes_allow(&routes[k], 1, operation->method, operation->path, "admin"))
                    without[count++] = routes[k];
            }
            store_fake(permissions[p], without, count); CHECK(!store_supports(calls[i].call)); free(without);
        }
    }
    store_reset(); routes_free(routes, n);
}

// MARK: - Poll backoff

static void test_poll_delay_is_the_base_without_failures(void) {
    CHECK_INT(poll_delay_ms(5000, 0, -1), 5000);
    CHECK_INT(poll_delay_ms(45000, 0, -1), 45000);
    CHECK_INT(poll_delay_ms(5000, -2, -1), 5000);
    // Retry-After only counts after a failure.
    CHECK_INT(poll_delay_ms(5000, 0, 90), 5000);
}

static void test_poll_delay_doubles_per_failure(void) {
    CHECK_INT(poll_delay_ms(5000, 1, -1), 2000);
    CHECK_INT(poll_delay_ms(5000, 2, -1), 4000);
    CHECK_INT(poll_delay_ms(5000, 3, -1), 8000);
    CHECK_INT(poll_delay_ms(5000, 5, -1), 32000);
    // Independent of the base: a slow poll retries a failure sooner than its own period.
    CHECK_INT(poll_delay_ms(45000, 1, -1), 2000);
}

static void test_poll_delay_is_capped_at_a_minute(void) {
    CHECK_INT(poll_delay_ms(5000, 6, -1), 60000);
    CHECK_INT(poll_delay_ms(5000, 7, -1), 60000);
    CHECK_INT(poll_delay_ms(5000, 1000, -1), 60000);
}

static void test_a_longer_retry_after_wins(void) {
    CHECK_INT(poll_delay_ms(5000, 1, 90), 90000);
    CHECK_INT(poll_delay_ms(5000, 1, 2.5), 2500);
    CHECK_INT(poll_delay_ms(5000, 10, 120), 120000);
    // A shorter one leaves the backoff alone.
    CHECK_INT(poll_delay_ms(5000, 3, 1), 8000);
    CHECK_INT(poll_delay_ms(5000, 10, 30), 60000);
    CHECK_INT(poll_delay_ms(5000, 2, 0), 4000);
}

// MARK: - What the token may do

static void test_nothing_is_allowed_before_pairing(void) {
    store_reset();
    CHECK(!store_connected());
    CHECK(!store_can_manage());
    CHECK(!store_can_transcribe());
    CHECK(!store_supports("sessions"));
    CHECK(!store_supports("message"));
    CHECK(!store_supports_attachments());
}

static void test_a_client_means_connected(void) {
    store_reset();
    ServerAddress address;
    CHECK(server_address_parse("https://briareus.example.test", &address));
    ApiError e; api_error_init(&e);
    ApiClient *client = api_client_new(&address, TOKEN, &e);
    CHECK(client != NULL);
    g_store.client = client;
    CHECK(store_connected());
    // A connection without a device record allows nothing yet.
    CHECK(!store_supports("sessions"));
    api_client_release(client);
    server_address_free(&address);
    api_error_clear(&e);
    store_reset();
}

static void test_permission_ranks_decide_writing(void) {
    store_fake("read", ROUTES, 5);
    CHECK(!store_can_manage());
    CHECK(!store_can_transcribe());
    store_fake("manage", ROUTES, 5);
    CHECK(store_can_manage());
    CHECK(store_can_transcribe());
    store_fake("admin", ROUTES, 5);
    CHECK(store_can_manage());
    store_fake("owner", ROUTES, 5);
    CHECK(!store_can_manage());
    store_fake(NULL, ROUTES, 5);
    CHECK(!store_can_manage());
    g_store.has_device = false; g_store.device.permission = "admin";
    CHECK(!store_can_manage());
    store_reset();
}

static void test_supports_needs_the_route_and_its_access(void) {
    store_fake("read", ROUTES, 5);
    CHECK(store_supports("sessions"));
    CHECK(!store_supports("message"));
    CHECK(!store_supports("usage_all"));
    store_fake("manage", ROUTES, 5);
    CHECK(store_supports("sessions"));
    CHECK(store_supports("message"));
    CHECK(!store_supports("usage_all"));
    store_fake("admin", ROUTES, 5);
    CHECK(store_supports("usage_all"));
    CHECK(!store_supports("no_such_call"));
    CHECK(!store_supports(NULL));
    store_reset();
}

static void test_the_preview_token_needs_a_manage_token_on_a_server_that_has_it(void) {
    Route routes[] = { { "GET", "/preview/access", "manage" } };
    store_fake("read", routes, 1);
    CHECK(!store_supports("preview_access"));
    store_fake("manage", routes, 1);
    CHECK(store_supports("preview_access"));
    store_fake("admin", routes, 1);
    CHECK(store_supports("preview_access"));
    // An older server without the route: the Run tab opens the browser without a token.
    store_fake("admin", ROUTES, 5);
    CHECK(!store_supports("preview_access"));
    store_reset();
}

static void test_a_route_the_server_lacks_is_not_supported(void) {
    store_fake("admin", ROUTES, 1);
    CHECK(store_supports("sessions"));
    CHECK(!store_supports("message"));
    store_fake("admin", NULL, 0);
    CHECK(!store_supports("sessions"));
    store_reset();
}

static void test_attachments_need_uploads_messages_and_writing(void) {
    store_fake("manage", ROUTES, 5);
    CHECK(store_supports_attachments());
    store_fake("read", ROUTES, 5);
    CHECK(!store_supports_attachments());
    // Without the uploads route (the third), or without messages.
    Route no_uploads[] = { ROUTES[0], ROUTES[1], ROUTES[3] };
    store_fake("admin", no_uploads, 3);
    CHECK(!store_supports_attachments());
    Route no_messages[] = { ROUTES[0], ROUTES[2] };
    store_fake("admin", no_messages, 2);
    CHECK(!store_supports_attachments());
    store_reset();
}
static void test_a_first_prompt_takes_files_where_sessions_start(void) {
    // Starting a session is POST /sessions, which ROUTES lacks.
    store_fake("manage", ROUTES, 5);
    CHECK(!store_supports_attachments_on("start_session"));
    Route starts[] = { ROUTES[0], { "POST", "/sessions", "manage" }, ROUTES[2] };
    store_fake("manage", starts, 3);
    CHECK(store_supports_attachments_on("start_session"));
    CHECK(!store_supports_attachments());
    store_fake("read", starts, 3);
    CHECK(!store_supports_attachments_on("start_session"));
    store_reset();
}

// MARK: - Requests refused before they leave

typedef struct { int calls; int tag; intptr_t arg; bool ok; int status; char *text; char *unexpected; } Answer;

static void answer_done(void *owner, Request *req) {
    Answer *a = owner;
    a->calls++; a->tag = req->tag; a->arg = req->arg; a->ok = req->ok; a->status = req->error.status;
    free(a->text); a->text = request_error_text(req);
    free(a->unexpected); a->unexpected = request_error_or_unexpected(req);
}
static void answer_free(Answer *a) { free(a->text); free(a->unexpected); memset(a, 0, sizeof *a); }

/// Hands the refusals posted to this thread (g_store.hwnd is NULL) to the store, as the main window would. Returns how many.
static int deliver(void) {
    int n = 0;
    MSG msg;
    while (PeekMessageW(&msg, (HWND)-1, WM_APP_REQUEST_DONE, WM_APP_REQUEST_DONE, PM_REMOVE)) { store_handle_message(msg.message, msg.wParam, msg.lParam); n++; }
    return n;
}

static bool stub_transport(void *ctx, const char *method, const char *url, const char *const *headers, const void *body, size_t body_len,
                           int timeout_ms, int *status, char **content_type, char **retry_after, char **response, size_t *response_len,
                           char **error_message) {
    (*(int *)ctx)++;
    *error_message = xstrdup("the test has no network");
    return false;
}

static void test_a_call_without_a_connection_is_refused(void) {
    store_reset();
    Answer a; memset(&a, 0, sizeof a);
    Request *slot = NULL;
    Request *r = store_call("sessions", json_object(), 1000, &a, answer_done, 3, &slot);
    CHECK(slot == r);
    CHECK_INT(deliver(), 1);
    CHECK_INT(a.calls, 1);
    CHECK_INT(a.tag, 3);
    CHECK(!a.ok);
    CHECK_INT(a.status, 403);
    CHECK_STR(a.text, "Access denied: This token cannot perform that action.");
    CHECK_STR(a.unexpected, a.text);
    CHECK(slot == NULL);
    answer_free(&a);
}

static void test_a_call_the_token_cannot_make_never_reaches_the_network(void) {
    store_fake("read", ROUTES, 5);
    ServerAddress address;
    CHECK(server_address_parse("https://briareus.example.test", &address));
    ApiError e; api_error_init(&e);
    ApiClient *client = api_client_new(&address, TOKEN, &e);
    int sent = 0;
    api_client_set_transport(client, stub_transport, &sent);
    g_store.client = client;
    Answer a; memset(&a, 0, sizeof a);
    Json *args = json_object(); json_set_str(args, "sessionId", "s1"); json_set_str(args, "text", "hi");
    store_call("message", args, 1000, &a, answer_done, 1, NULL);
    store_call("no_such_call", NULL, 1000, &a, answer_done, 2, NULL);
    CHECK_INT(deliver(), 2);
    CHECK_INT(a.calls, 2);
    CHECK_INT(a.status, 403);
    CHECK_INT(sent, 0);
    answer_free(&a);
    api_client_release(client);
    server_address_free(&address);
    api_error_clear(&e);
    store_reset();
}

static void test_transcription_and_uploads_are_refused_with_their_own_words(void) {
    store_fake("read", ROUTES, 5);
    Answer a; memset(&a, 0, sizeof a);
    char audio[4] = { 1, 2, 3, 4 };
    store_transcribe(audio, sizeof audio, NULL, &a, answer_done, 1, NULL);
    CHECK_INT(deliver(), 1);
    CHECK_STR(a.text, "Access denied: This token cannot transcribe voice notes.");
    store_upload("notes.txt", xstrdup("hello"), 5, &a, answer_done, 2, NULL);
    CHECK_INT(deliver(), 1);
    CHECK_STR(a.text, "Access denied: This server does not take files with a message.");
    store_upload(NULL, NULL, 0, &a, answer_done, 3, NULL);
    CHECK_INT(deliver(), 1);
    CHECK_INT(a.calls, 3);
    CHECK_INT(a.tag, 3);
    answer_free(&a);
    store_reset();
}

static void test_a_cancelled_request_is_never_answered(void) {
    store_reset();
    Answer a; memset(&a, 0, sizeof a);
    Request *slot = NULL;
    store_call("sessions", NULL, 1000, &a, answer_done, 1, &slot);
    request_cancel(&slot);
    CHECK(slot == NULL);
    request_cancel(&slot);
    request_cancel(NULL);
    CHECK_INT(deliver(), 1);
    CHECK_INT(a.calls, 0);
    answer_free(&a);
}

static void test_a_new_request_in_the_slot_replaces_the_old_one(void) {
    store_reset();
    Answer a; memset(&a, 0, sizeof a);
    Request *slot = NULL;
    Request *first = store_call("sessions", NULL, 1000, &a, answer_done, 1, &slot);
    Request *second = store_call("sessions", NULL, 1000, &a, answer_done, 2, &slot);
    CHECK(first != second);
    CHECK(slot == second);
    CHECK_INT(deliver(), 2);
    CHECK_INT(a.calls, 1);
    CHECK_INT(a.tag, 2);
    CHECK(slot == NULL);
    answer_free(&a);
}

static void test_request_error_helpers(void) {
    Request req; memset(&req, 0, sizeof req);
    api_error_init(&req.error);
    req.ok = true;
    CHECK_OWNED_STR(request_error_or_unexpected(&req), "The server returned an unexpected response.");
    CHECK_OWNED_STR(request_error_text(&req), "");
    req.ok = false;
    api_error_set(&req.error, API_HTTP, 429, NULL, 30);
    CHECK_OWNED_STR(request_error_or_unexpected(&req), "The server is rate limiting requests. Updates will resume after a delay.");
    char *slot = xstrdup("old");
    request_error_into(&slot, &req);
    CHECK_STR(slot, "The server is rate limiting requests. Updates will resume after a delay.");
    api_error_set(&req.error, API_NETWORK, 0, "Connection reset", -1);
    request_error_into(&slot, &req);
    CHECK_STR(slot, "Connection reset");
    free(slot);
    api_error_clear(&req.error);
}

static void test_only_a_refusal_leaves_the_outcome_known(void) {
    Request req; memset(&req, 0, sizeof req);
    api_error_init(&req.error);
    req.ok = true;
    CHECK(!request_outcome_unknown(&req));
    req.ok = false;
    api_error_set(&req.error, API_HTTP, 409, "Draining", -1);
    CHECK(!request_outcome_unknown(&req));
    api_error_set(&req.error, API_HTTP, 503, NULL, -1);
    CHECK(request_outcome_unknown(&req));
    api_error_set(&req.error, API_NETWORK, 0, "Connection reset", -1);
    CHECK(request_outcome_unknown(&req));
    api_error_clear(&req.error);
}

void app_store_tests(void) {
    test_run("pinned catalog gates actual calls and absent routes", test_pinned_catalog_gates_existing_calls);
    test_run("poll delay is the base without failures", test_poll_delay_is_the_base_without_failures);
    test_run("poll delay doubles per failure", test_poll_delay_doubles_per_failure);
    test_run("poll delay is capped at a minute", test_poll_delay_is_capped_at_a_minute);
    test_run("a longer retry-after wins", test_a_longer_retry_after_wins);
    test_run("nothing is allowed before pairing", test_nothing_is_allowed_before_pairing);
    test_run("a client means connected", test_a_client_means_connected);
    test_run("permission ranks decide writing", test_permission_ranks_decide_writing);
    test_run("supports needs the route and its access", test_supports_needs_the_route_and_its_access);
    test_run("the preview token needs a manage token on a server that has it", test_the_preview_token_needs_a_manage_token_on_a_server_that_has_it);
    test_run("a route the server lacks is not supported", test_a_route_the_server_lacks_is_not_supported);
    test_run("attachments need uploads, messages and writing", test_attachments_need_uploads_messages_and_writing);
    test_run("a first prompt takes files where sessions start", test_a_first_prompt_takes_files_where_sessions_start);
    test_run("a call without a connection is refused", test_a_call_without_a_connection_is_refused);
    test_run("a call the token cannot make never reaches the network", test_a_call_the_token_cannot_make_never_reaches_the_network);
    test_run("transcription and uploads are refused with their own words", test_transcription_and_uploads_are_refused_with_their_own_words);
    test_run("a cancelled request is never answered", test_a_cancelled_request_is_never_answered);
    test_run("a new request in the slot replaces the old one", test_a_new_request_in_the_slot_replaces_the_old_one);
    test_run("request error helpers", test_request_error_helpers);
    test_run("only a refusal leaves the outcome known", test_only_a_refusal_leaves_the_outcome_known);
}
