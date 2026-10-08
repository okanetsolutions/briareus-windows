// The core tests: networking rules against /api/v1, transcript, board and cache behaviour.
#include "api.h"
#include "board.h"
#include "cache.h"
#include "diff.h"
#include "json.h"
#include "markdown.h"
#include "models.h"
#include "str.h"
#include "suites.h"
#include "test.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

static const char *TOKEN = "brm_aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";

// MARK: - Stub transport

typedef struct {
    int status; const char *content_type; const char *retry_after; const char *body;
    bool fail; const char *fail_message;
    int calls;
    char *last_method, *last_url, *last_body, *last_content_type, *last_authorization, *last_accept;
    size_t last_body_len;
} Stub;

static void stub_reset(Stub *s) {
    free(s->last_method); free(s->last_url); free(s->last_body); free(s->last_content_type); free(s->last_authorization); free(s->last_accept);
    memset(s, 0, sizeof *s);
}
static bool stub_transport(void *ctx, const char *method, const char *url, const char *const *headers, const void *body, size_t body_len,
                           int timeout_ms, int *status, char **content_type, char **retry_after, char **response, size_t *response_len,
                           char **error_message) {
    (void)timeout_ms;
    Stub *s = ctx;
    s->calls++;
    free(s->last_method); s->last_method = xstrdup(method);
    free(s->last_url); s->last_url = xstrdup(url);
    free(s->last_body); s->last_body = body_len ? xstrndup(body, body_len) : NULL; s->last_body_len = body_len;
    free(s->last_content_type); s->last_content_type = NULL;
    free(s->last_authorization); s->last_authorization = NULL;
    free(s->last_accept); s->last_accept = NULL;
    for (size_t i = 0; headers && headers[i] && headers[i + 1]; i += 2) {
        if (str_eq(headers[i], "Content-Type")) s->last_content_type = xstrdup(headers[i + 1]);
        if (str_eq(headers[i], "Authorization")) s->last_authorization = xstrdup(headers[i + 1]);
        if (str_eq(headers[i], "Accept")) s->last_accept = xstrdup(headers[i + 1]);
        CHECK(!str_eq(headers[i], "Cookie") && !str_eq(headers[i], "Origin"));
    }
    if (s->fail) { *error_message = xstrdup(s->fail_message ? s->fail_message : "timed out"); return false; }
    *status = s->status;
    *content_type = xstrdup(s->content_type);
    *retry_after = xstrdup(s->retry_after);
    *response = xstrdup(s->body ? s->body : ""); *response_len = strlen(*response);
    return true;
}
static ApiClient *client(Stub *stub) {
    ServerAddress a; CHECK(server_address_parse("https://example.com", &a));
    ApiError e; api_error_init(&e);
    ApiClient *c = api_client_new(&a, TOKEN, &e);
    server_address_free(&a);
    CHECK(c != NULL);
    api_client_set_transport(c, stub_transport, stub);
    return c;
}

// MARK: - Networking

static void test_normalizes_origin_and_api_path(void) {
    ServerAddress a;
    CHECK(server_address_parse(" https://EXAMPLE.com:443/api/v1/ ", &a));
    CHECK_STR(a.origin, "https://example.com");
    CHECK_STR(a.base_url, "https://example.com/api/v1/");
    server_address_free(&a);
    CHECK(server_address_parse("https://example.com/api/v1", &a));
    CHECK_STR(a.base_url, "https://example.com/api/v1/");
    server_address_free(&a);
    CHECK(server_address_parse("https://example.com:8443", &a));
    CHECK_STR(a.origin, "https://example.com:8443");
    server_address_free(&a);
}
static void test_rejects_unsafe_or_ambiguous_addresses(void) {
    const char *bad[] = { "http://example.com", "https://user:pass@example.com", "https://example.com?token=x", "https://example.com/#x",
                          "https://example.com/api/dev", "https://example.com/api/mobile/v1", "file:///secret", "example.com", "https://", "https://example.com:0", "https://example.com:99999" };
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) { ServerAddress a; bool ok = server_address_parse(bad[i], &a); if (ok) { printf("  accepted %s\n", bad[i]); server_address_free(&a); } CHECK(!ok); }
}
static void test_rejects_invalid_tokens(void) {
    char *long_token = xstrfmt("%sa", TOKEN);
    char *newline = xstrfmt("%s\n", TOKEN);
    const char *bad[] = { "", "Bearer brm_bad", "brm_short", long_token, newline };
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) CHECK(!api_token_valid(bad[i]));
    CHECK(api_token_valid(TOKEN));
    ServerAddress a; server_address_parse("https://example.com", &a);
    ApiError e; api_error_init(&e);
    CHECK(api_client_new(&a, "brm_short", &e) == NULL); CHECK(e.kind == API_INVALID_TOKEN);
    api_error_clear(&e); server_address_free(&a);
    free(long_token); free(newline);
}
static void test_calls_take_their_route_and_arguments_from_the_table(void) {
    Stub stub = { 0 }; stub.status = 200; stub.content_type = "application/json";
    stub.body = "{\"session\":{\"id\":\"abc\",\"status\":\"running\",\"extra\":42},\"events\":[]}";
    ApiClient *c = client(&stub);
    ApiError e; api_error_init(&e);
    char *bearer = xstrfmt("Bearer %s", TOKEN);
    // A read: the path parameter comes out of the arguments and the rest go in the query, with no body.
    Json *args = json_object(); json_set_str(args, "sessionId", "abc"); json_set_num(args, "since", 7);
    Json *result = api_call(c, "session", args, 0, &e);
    CHECK(result != NULL);
    CHECK_STR(stub.last_url, "https://example.com/api/v1/sessions/abc?since=7"); CHECK_STR(stub.last_method, "GET");
    CHECK(stub.last_body == NULL); CHECK(stub.last_content_type == NULL);
    CHECK_STR(stub.last_authorization, bearer); CHECK_STR(stub.last_accept, "application/json");
    CHECK_STR(json_str(json_get(json_get(result, "session"), "status")), "running"); CHECK(json_count(json_get(result, "events")) == 0);
    json_free(result); json_free(args);
    // A write: the path parameter is taken out of the JSON body, which carries the rest.
    args = json_object(); json_set_str(args, "sessionId", "abc"); json_set_str(args, "text", "hi"); Json *ids = json_array(); json_array_push(ids, json_string("f1")); json_object_set(args, "attachments", ids);
    result = api_call(c, "message", args, 0, &e); CHECK(result != NULL); json_free(result); json_free(args);
    CHECK_STR(stub.last_url, "https://example.com/api/v1/sessions/abc/messages"); CHECK_STR(stub.last_method, "POST"); CHECK_STR(stub.last_content_type, "application/json");
    Json *sent = json_parsez(stub.last_body);
    CHECK_STR(json_str(json_get(sent, "text")), "hi"); CHECK(json_is_null(json_get(sent, "sessionId"))); CHECK(json_count(json_get(sent, "attachments")) == 1);
    json_free(sent);
    // Two path parameters, and a DELETE.
    args = json_object(); json_set_str(args, "sessionId", "abc"); json_set_num(args, "index", 2);
    result = api_call(c, "drop_message", args, 0, &e); CHECK(result != NULL); json_free(result); json_free(args);
    CHECK_STR(stub.last_url, "https://example.com/api/v1/sessions/abc/queue/2"); CHECK_STR(stub.last_method, "DELETE"); CHECK(stub.last_body == NULL);
    // A pull request read pins its revision in the query; the project goes there too, encoded.
    args = json_object(); json_set_str(args, "repo", "o/r"); json_set_num(args, "pr", 12); json_set_num(args, "page", 2); json_set_str(args, "headSha", "h1"); json_set_str(args, "baseSha", "b1");
    result = api_call(c, "pull_files", args, 0, &e); CHECK(result != NULL); json_free(result); json_free(args);
    CHECK_STR(stub.last_url, "https://example.com/api/v1/pulls/12/files?repo=o%2Fr&page=2&headSha=h1&baseSha=b1");
    args = json_object(); json_set_str(args, "repo", "o/r"); json_set_num(args, "pr", 12); json_set_num(args, "page", 2);
    result = api_call(c, "pull_review_comments", args, 0, &e); CHECK(result != NULL); json_free(result); json_free(args);
    CHECK_STR(stub.last_url, "https://example.com/api/v1/pulls/12/review-comments?repo=o%2Fr&page=2"); CHECK_STR(stub.last_method, "GET");
    // Run takes its number from the errand's `prNumber`; the project stays in the body.
    args = json_object(); json_set_str(args, "repo", "o/r"); json_set_num(args, "prNumber", 12);
    result = api_call(c, "serve_pull", args, 0, &e); CHECK(result != NULL); json_free(result); json_free(args);
    CHECK_STR(stub.last_url, "https://example.com/api/v1/pulls/12/serve"); sent = json_parsez(stub.last_body);
    CHECK_STR(json_str(json_get(sent, "repo")), "o/r"); CHECK(json_is_null(json_get(sent, "prNumber"))); json_free(sent);
    // Code review is a session start with the `review` flag set here.
    args = json_object(); json_set_str(args, "repo", "o/r"); json_set_num(args, "prNumber", 12); json_set_str(args, "branch", "docs");
    result = api_call(c, "review", args, 0, &e); CHECK(result != NULL); json_free(result); json_free(args);
    CHECK_STR(stub.last_url, "https://example.com/api/v1/sessions"); sent = json_parsez(stub.last_body);
    CHECK(json_bool_is(json_get(sent, "review"), true)); CHECK_STR(json_str(json_get(sent, "branch")), "docs"); CHECK(json_num_or(json_get(sent, "prNumber"), 0) == 12); json_free(sent);
    // An errand names itself; a call without arguments still sends a JSON object.
    args = json_object(); json_set_str(args, "repo", "o/r"); json_set_str(args, "action", "solve-conflicts"); json_set_num(args, "prNumber", 12);
    result = api_call(c, "action", args, 0, &e); CHECK(result != NULL); json_free(result); json_free(args);
    CHECK_STR(stub.last_url, "https://example.com/api/v1/actions"); sent = json_parsez(stub.last_body); CHECK_STR(json_str(json_get(sent, "action")), "solve-conflicts"); json_free(sent);
    result = api_call(c, "projects", NULL, 0, &e); CHECK(result != NULL); json_free(result);
    CHECK_STR(stub.last_url, "https://example.com/api/v1/projects"); CHECK(stub.last_body == NULL);
    args = json_object(); json_set_str(args, "sessionId", "abc");
    result = api_call(c, "cancel", args, 0, &e); CHECK(result != NULL); json_free(result); json_free(args);
    CHECK_STR(stub.last_url, "https://example.com/api/v1/sessions/abc/cancel"); CHECK_STR(stub.last_body, "{}");
    // A path argument is encoded, so it cannot reach another route.
    args = json_object(); json_set_str(args, "sessionId", "../token");
    result = api_call(c, "session", args, 0, &e); CHECK(result != NULL); json_free(result); json_free(args);
    CHECK_STR(stub.last_url, "https://example.com/api/v1/sessions/..%2Ftoken");
    // A project's settings: its id goes in the path, the rest of the row in the body; a new one has no id.
    args = json_object(); json_set_num(args, "id", 7); json_set_str(args, "label", "Heedly"); json_object_set(args, "workerBudgetUsd", json_null());
    result = api_call(c, "update_project", args, 0, &e); CHECK(result != NULL); json_free(result); json_free(args);
    CHECK_STR(stub.last_url, "https://example.com/api/v1/settings/projects/7"); CHECK_STR(stub.last_method, "PUT"); sent = json_parsez(stub.last_body);
    CHECK(json_is_null(json_get(sent, "id"))); CHECK_STR(json_str(json_get(sent, "label")), "Heedly"); CHECK(json_count(sent) == 2); json_free(sent);
    args = json_object(); json_set_str(args, "repo", "o/new");
    result = api_call(c, "create_project", args, 0, &e); CHECK(result != NULL); json_free(result); json_free(args);
    CHECK_STR(stub.last_url, "https://example.com/api/v1/settings/projects"); CHECK_STR(stub.last_method, "POST");
    args = json_object(); json_set_num(args, "id", 7);
    result = api_call(c, "delete_project", args, 0, &e); CHECK(result != NULL); json_free(result); json_free(args);
    CHECK_STR(stub.last_url, "https://example.com/api/v1/settings/projects/7"); CHECK_STR(stub.last_method, "DELETE"); CHECK(stub.last_body == NULL);
    args = json_object();
    { Json *order = json_array(); json_array_push(order, json_number(2)); json_array_push(order, json_number(1)); json_object_set(args, "ids", order); }
    result = api_call(c, "order_projects", args, 0, &e); CHECK(result != NULL); json_free(result); json_free(args);
    CHECK_STR(stub.last_url, "https://example.com/api/v1/settings/projects/order"); CHECK_STR(stub.last_method, "PUT"); CHECK_STR(stub.last_body, "{\"ids\":[2,1]}");
    // A provider's status reads fresh through the query; finishing a login sends the code in the body, the id in the path.
    args = json_object(); json_set_num(args, "id", 3); json_set_num(args, "fresh", 1);
    result = api_call(c, "provider_status", args, 0, &e); CHECK(result != NULL); json_free(result); json_free(args);
    CHECK_STR(stub.last_url, "https://example.com/api/v1/settings/providers/3/status?fresh=1"); CHECK_STR(stub.last_method, "GET");
    args = json_object(); json_set_num(args, "id", 3); json_set_str(args, "code", "abc#def");
    result = api_call(c, "provider_login_finish", args, 0, &e); CHECK(result != NULL); json_free(result); json_free(args);
    CHECK_STR(stub.last_url, "https://example.com/api/v1/settings/providers/3/login/finish"); CHECK_STR(stub.last_method, "POST"); CHECK_STR(stub.last_body, "{\"code\":\"abc#def\"}");
    // A database server: the same shape; its probe sends the form's values, the saved row's id among them, in the body.
    args = json_object(); json_set_num(args, "id", 3); json_set_str(args, "host", "db1"); json_set_num(args, "port", 3306);
    result = api_call(c, "update_db_server", args, 0, &e); CHECK(result != NULL); json_free(result); json_free(args);
    CHECK_STR(stub.last_url, "https://example.com/api/v1/settings/db-servers/3"); CHECK_STR(stub.last_method, "PUT"); CHECK_STR(stub.last_body, "{\"host\":\"db1\",\"port\":3306}");
    args = json_object(); json_set_num(args, "id", 3); json_set_str(args, "host", "db1");
    result = api_call(c, "test_db_server", args, 0, &e); CHECK(result != NULL); json_free(result); json_free(args);
    CHECK_STR(stub.last_url, "https://example.com/api/v1/settings/db-servers/test"); CHECK_STR(stub.last_method, "POST"); CHECK_STR(stub.last_body, "{\"id\":3,\"host\":\"db1\"}");
    args = json_object(); json_set_num(args, "id", 3);
    result = api_call(c, "delete_db_server", args, 0, &e); CHECK(result != NULL); json_free(result); json_free(args);
    CHECK_STR(stub.last_url, "https://example.com/api/v1/settings/db-servers/3"); CHECK_STR(stub.last_method, "DELETE");
    // An SSH server, the same way.
    args = json_object(); json_set_num(args, "id", 1727000000000.0); json_set_str(args, "host", "web.example.com"); json_set_num(args, "port", 2222);
    result = api_call(c, "update_ssh_server", args, 0, &e); CHECK(result != NULL); json_free(result); json_free(args);
    CHECK_STR(stub.last_url, "https://example.com/api/v1/settings/ssh/servers/1727000000000"); CHECK_STR(stub.last_method, "PUT"); sent = json_parsez(stub.last_body);
    CHECK(json_is_null(json_get(sent, "id"))); CHECK(json_int_or(json_get(sent, "port"), 0) == 2222); json_free(sent);
    args = json_object(); json_set_str(args, "host", "db.example.com");
    result = api_call(c, "create_ssh_server", args, 0, &e); CHECK(result != NULL); json_free(result); json_free(args);
    CHECK_STR(stub.last_url, "https://example.com/api/v1/settings/ssh/servers"); CHECK_STR(stub.last_method, "POST");
    args = json_object(); json_set_num(args, "id", 3);
    result = api_call(c, "delete_ssh_server", args, 0, &e); CHECK(result != NULL); json_free(result); json_free(args);
    CHECK_STR(stub.last_url, "https://example.com/api/v1/settings/ssh/servers/3"); CHECK_STR(stub.last_method, "DELETE");
    // A filter picked more than once repeats its parameter.
    args = json_object(); json_set_str(args, "period", "all");
    { Json *picks = json_array(); json_array_push(picks, json_string("p:1")); json_array_push(picks, json_string("r:o/r")); json_object_set(args, "project", picks); }
    result = api_call(c, "usage_all", args, 0, &e); CHECK(result != NULL); json_free(result); json_free(args);
    CHECK_STR(stub.last_url, "https://example.com/api/v1/usage/all?period=all&project=p%3A1&project=r%3Ao%2Fr");
    // The session list has no project parameter: the project is kept back and the answer cut down to it.
    stub.body = "{\"sessions\":[{\"id\":\"a\",\"status\":\"idle\",\"repo\":\"o/r\"},{\"id\":\"b\",\"status\":\"idle\",\"repo\":\"o/other\"}]}";
    args = json_object(); json_set_str(args, "repo", "o/r");
    result = api_call(c, "sessions", args, 0, &e); CHECK(result != NULL);
    CHECK_STR(stub.last_url, "https://example.com/api/v1/sessions");
    CHECK(json_count(json_get(result, "sessions")) == 1); CHECK_STR(json_str(json_get(json_at(json_get(result, "sessions"), 0), "id")), "a");
    json_free(result); json_free(args);
    result = api_call(c, "sessions", NULL, 0, &e); CHECK(result != NULL); CHECK(json_count(json_get(result, "sessions")) == 2); json_free(result);
    // What the table does not know, or a path argument that is missing, never leaves the client.
    int calls = stub.calls;
    CHECK(api_call(c, "../projects", NULL, 0, &e) == NULL); CHECK(e.kind == API_HTTP && e.status == 400);
    args = json_object(); json_set_str(args, "text", "hi");
    CHECK(api_call(c, "message", args, 0, &e) == NULL); CHECK(e.kind == API_HTTP && e.status == 400); CHECK_STR(e.message, "Missing argument: sessionId");
    json_free(args);
    args = json_object(); json_set_str(args, "sessionId", "");
    CHECK(api_call(c, "session", args, 0, &e) == NULL); CHECK(e.kind == API_HTTP && e.status == 400);
    json_free(args);
    CHECK(stub.calls == calls);
    // Every call the board and the screens make is in the table.
    const char *names[] = { "projects", "sessions", "session", "runtimes", "branches", "actions", "action", "pulls", "pull", "pull_description", "pull_files", "pull_comments",
                            "pull_reviews", "pull_review_comments", "findings",
                            "finding_decision", "merge_pull", "serve_pull", "start_session", "review", "message", "rename", "delete", "drop_message", "cancel", "close",
                            "reopen", "review_loop", "complete_findings", "save_findings", "reply_finding", "delete_finding", "upload", "transcribe",
                            "settings_projects", "create_project", "update_project", "delete_project", "order_projects",
                            "settings_providers", "create_provider", "update_provider", "delete_provider", "test_provider", "provider_status",
                            "provider_login", "provider_login_start", "provider_login_finish",
                            "settings_db_servers", "create_db_server", "update_db_server", "delete_db_server", "test_db_server",

                            "settings_ssh_servers", "create_ssh_server", "update_ssh_server", "delete_ssh_server", "ssh_db_credentials" };
    for (size_t i = 0; i < sizeof names / sizeof *names; i++) { if (!api_route(names[i])) printf("  no route for %s\n", names[i]); CHECK(api_route(names[i]) != NULL); }
    CHECK(api_route("operations") == NULL);
    free(bearer); api_error_clear(&e); api_client_release(c); stub_reset(&stub);
}
static void test_discovery_validates_version_and_milliseconds(void) {
    Stub stub = { 0 }; stub.status = 200; stub.content_type = "application/json; charset=utf-8";
    stub.body = "{\"version\":1,\"client\":{\"id\":\"d\",\"label\":\"Laptop\",\"repos\":[\"a/b\"],\"permission\":\"read\",\"expiresAt\":1000000}}";
    ApiClient *c = client(&stub);
    Discovery d; ApiError e; api_error_init(&e);
    CHECK(api_discovery(c, &d, &e));
    CHECK(!device_can_manage(&d.device)); CHECK(device_expiry(&d.device) == 1000); CHECK(d.transcribe == -1);
    CHECK_STR(stub.last_url, "https://example.com/api/v1/"); CHECK_STR(stub.last_method, "GET");
    discovery_free(&d);
    // An admin token writes too; one with a permission this app does not know does nothing.
    stub.body = "{\"version\":1,\"client\":{\"id\":\"d\",\"label\":\"Web\",\"repos\":[],\"permission\":\"admin\",\"expiresAt\":0}}";
    CHECK(api_discovery(c, &d, &e)); CHECK(device_can_manage(&d.device)); discovery_free(&d);
    CHECK(permission_rank("read") == 0 && permission_rank("manage") == 1 && permission_rank("admin") == 2 && permission_rank("owner") < 0);
    // The mobile API's shape is not this API's.
    stub.body = "{\"version\":1,\"device\":{\"id\":\"d\",\"label\":\"Phone\",\"repos\":[],\"permission\":\"manage\",\"expiresAt\":0}}";
    CHECK(!api_discovery(c, &d, &e)); CHECK(e.kind == API_NON_JSON);
    stub.body = "{\"version\":2,\"client\":{\"id\":\"d\",\"label\":\"Phone\",\"repos\":[],\"permission\":\"manage\",\"expiresAt\":0}}";
    CHECK(!api_discovery(c, &d, &e)); CHECK(e.kind == API_INCOMPATIBLE_VERSION);
    api_error_clear(&e); api_client_release(c); stub_reset(&stub);
}
static void test_redirect_and_html_are_not_accepted(void) {
    Stub stub = { 0 }; stub.content_type = "text/html"; stub.body = "<html>Login</html>";
    ApiClient *c = client(&stub);
    ApiError e; api_error_init(&e);
    stub.status = 302;
    CHECK(api_call(c, "projects", NULL, 0, &e) == NULL); CHECK(e.kind == API_REDIRECTED);
    stub.status = 200;
    CHECK(api_call(c, "projects", NULL, 0, &e) == NULL); CHECK(e.kind == API_NON_JSON);
    api_error_clear(&e); api_client_release(c); stub_reset(&stub);
}
static void test_errors_keep_status_and_retry_after_without_retrying_write(void) {
    Stub stub = { 0 }; stub.status = 429; stub.content_type = "application/json"; stub.retry_after = "90"; stub.body = "{\"error\":\"Wait\"}";
    ApiClient *c = client(&stub);
    ApiError e; api_error_init(&e);
    Json *args = json_object(); json_set_str(args, "sessionId", "abc"); json_set_str(args, "text", "hi");
    CHECK(api_call(c, "message", args, 0, &e) == NULL);
    CHECK(e.kind == API_HTTP && e.status == 429); CHECK_STR(e.message, "Wait"); CHECK(e.retry_after == 90);
    CHECK(stub.calls == 1);
    json_free(args);
    stub.status = 401; stub.retry_after = NULL; stub.body = "{\"error\":\"Expired\"}";
    CHECK(api_call(c, "projects", NULL, 0, &e) == NULL); CHECK(api_error_unauthorized(&e));
    char *text = api_error_description(&e); CHECK(strstr(text, "expired or was revoked") != NULL); free(text);
    api_error_clear(&e); api_client_release(c); stub_reset(&stub);
}
static void test_timeout_does_not_retry_write(void) {
    Stub stub = { 0 }; stub.fail = true; stub.fail_message = "The request timed out.";
    ApiClient *c = client(&stub);
    ApiError e; api_error_init(&e);
    CHECK(api_call(c, "start_session", NULL, 0, &e) == NULL); CHECK(e.kind == API_NETWORK); CHECK_STR(e.message, "The request timed out.");
    CHECK(stub.calls == 1);
    api_error_clear(&e); api_client_release(c); stub_reset(&stub);
}
static void test_voice_note_is_posted_as_recorded_and_answered_with_its_text(void) {
    Stub stub = { 0 }; stub.status = 200; stub.content_type = "application/json"; stub.body = "{\"text\":\"hola mundo\"}";
    ApiClient *c = client(&stub);
    ApiError e; api_error_init(&e);
    unsigned char audio[] = { 0, 1, 2, 255 };
    char *text = api_transcribe(c, audio, sizeof audio, "audio/mp4", &e);
    CHECK_STR(text, "hola mundo"); free(text);
    CHECK_STR(stub.last_url, "https://example.com/api/v1/transcribe");
    CHECK_STR(stub.last_method, "POST"); CHECK_STR(stub.last_content_type, "audio/mp4");
    CHECK(stub.last_body_len == 4 && memcmp(stub.last_body, audio, 4) == 0);
    stub.status = 502; stub.body = "{\"error\":\"OpenAI answered 400\"}";
    CHECK(api_transcribe(c, audio, 1, NULL, &e) == NULL);
    CHECK_STR(stub.last_content_type, "audio/mp4");
    CHECK(e.kind == API_HTTP && e.status == 502); CHECK_STR(e.message, "OpenAI answered 400");
    stub.status = 200; stub.body = "{\"ok\":true}";
    CHECK(api_transcribe(c, audio, 1, NULL, &e) == NULL); CHECK(e.kind == API_NON_JSON);
    api_error_clear(&e); api_client_release(c); stub_reset(&stub);
}
static void test_attachment_is_posted_raw_and_answered_with_its_id(void) {
    Stub stub = { 0 }; stub.status = 201; stub.content_type = "application/json"; stub.body = "{\"file\":{\"id\":\"1a2b3c4d\",\"name\":\"pasted.png\",\"size\":4}}";
    ApiClient *c = client(&stub);
    ApiError e; api_error_init(&e);
    unsigned char png[] = { 0x89, 'P', 'N', 'G' };
    char *id = api_upload(c, "pasted image #1.png", png, sizeof png, &e);
    CHECK_STR(id, "1a2b3c4d"); free(id);
    CHECK_STR(stub.last_url, "https://example.com/api/v1/uploads?name=pasted%20image%20%231.png");
    CHECK_STR(stub.last_method, "POST"); CHECK_STR(stub.last_content_type, "application/octet-stream");
    CHECK(stub.last_body_len == 4 && memcmp(stub.last_body, png, 4) == 0);
    // A file whose own type is JSON must not reach the server's JSON parser either.
    id = api_upload(c, "", "{}", 2, &e); CHECK_STR(id, "1a2b3c4d"); free(id);
    CHECK_STR(stub.last_content_type, "application/octet-stream"); CHECK(strstr(stub.last_url, "?name=file") != NULL);
    // Refusals keep the server's words; an answer without an id is not one.
    stub.status = 404; stub.body = "{\"error\":\"Not found\"}";
    CHECK(api_upload(c, "a.txt", "x", 1, &e) == NULL); CHECK(e.kind == API_HTTP && e.status == 404); CHECK_STR(e.message, "Not found");
    stub.status = 201; stub.body = "{\"ok\":true}";
    CHECK(api_upload(c, "a.txt", "x", 1, &e) == NULL); CHECK(e.kind == API_NON_JSON);
    // Oversized files never leave the client.
    int calls = stub.calls;
    void *big = xcalloc(1, API_UPLOAD_LIMIT + 1);
    CHECK(api_upload(c, "big.bin", big, API_UPLOAD_LIMIT + 1, &e) == NULL); CHECK(e.kind == API_HTTP && e.status == 413); CHECK(stub.calls == calls);
    free(big);
    api_error_clear(&e); api_client_release(c); stub_reset(&stub);
}
static void test_catalog_reads_routes_and_who_may_call_them_from_openapi(void) {
    Stub stub = { 0 }; stub.status = 200; stub.content_type = "application/json";
    stub.body = "{\"openapi\":\"3.1.0\",\"paths\":{"
        "\"/\":{\"get\":{\"operationId\":\"clientGet\",\"x-briareus-access\":\"read\"}},"
        "\"/projects\":{\"get\":{\"x-briareus-access\":\"read\",\"x-briareus-scope\":\"any\"}},"
        "\"/sessions\":{\"get\":{\"x-briareus-access\":\"read\"},\"post\":{\"x-briareus-access\":\"manage\"}},"
        "\"/sessions/{id}\":{\"get\":{\"x-briareus-access\":\"read\"},\"patch\":{\"x-briareus-access\":\"manage\"},\"delete\":{\"x-briareus-access\":\"manage\"}},"
        "\"/sessions/{id}/messages\":{\"post\":{\"x-briareus-access\":\"manage\"}},"
        "\"/pulls/{number}/files\":{\"get\":{\"x-briareus-access\":\"read\"}},"
        "\"/uploads\":{\"post\":{\"x-briareus-access\":\"manage\"}},"
        "\"/settings/devices\":{\"get\":{\"x-briareus-access\":\"admin\"},\"post\":{}}"
        "},\"components\":{}}";
    ApiClient *c = client(&stub);
    ApiError e; api_error_init(&e);
    Route *routes = NULL; size_t n = 0;
    CHECK(api_catalog(c, &routes, &n, &e)); CHECK(n == 12);
    CHECK_STR(stub.last_url, "https://example.com/api/v1/openapi.json"); CHECK_STR(stub.last_method, "GET");
    // The app's own paths name their parameters after its arguments; the server's are matched segment by segment.
    CHECK(routes_allow(routes, n, "GET", "sessions/{sessionId}", "read"));
    CHECK(routes_allow(routes, n, "GET", "pulls/{pr}/files", "read"));
    CHECK(!routes_allow(routes, n, "GET", "pulls/{pr}", "read"));
    CHECK(!routes_allow(routes, n, "POST", "sessions/{sessionId}/messages", "read"));
    CHECK(routes_allow(routes, n, "POST", "sessions/{sessionId}/messages", "manage"));
    CHECK(routes_allow(routes, n, "DELETE", "sessions/{sessionId}", "admin"));
    CHECK(!routes_allow(routes, n, "GET", "settings/devices", "manage")); CHECK(routes_allow(routes, n, "GET", "settings/devices", "admin"));
    // An operation that says nothing about access is the operator's; a permission this app does not know may do nothing.
    CHECK(!routes_allow(routes, n, "POST", "settings/devices", "manage")); CHECK(routes_allow(routes, n, "POST", "settings/devices", "admin"));
    CHECK(!routes_allow(routes, n, "GET", "projects", "owner"));
    CHECK(!routes_allow(routes, n, "POST", "sessions/{sessionId}/cancel", "admin"));
    CHECK(!routes_allow(routes, n, "GET", "sessions/{sessionId}/messages", "admin"));
    // The table's routes are checked against the catalog by method and path.
    const ApiRoute *message = api_route("message"), *upload = api_route("upload"), *cancel = api_route("cancel");
    CHECK(routes_allow(routes, n, message->method, message->path, "manage") && routes_allow(routes, n, upload->method, upload->path, "manage"));
    CHECK(!routes_allow(routes, n, cancel->method, cancel->path, "manage"));
    // A saved copy reads back the same.
    Json *saved = routes_json(routes, n);
    Route *again = NULL; size_t m = 0;
    CHECK(routes_parse(saved, &again, &m)); CHECK(m == n);
    CHECK(routes_allow(again, m, "POST", "sessions/{sessionId}/messages", "manage")); CHECK(!routes_allow(again, m, "POST", "sessions/{sessionId}/messages", "read"));
    Route *copy = routes_copy(again, m); CHECK(routes_allow(copy, m, "GET", "/", "read")); routes_free(copy, m);
    routes_free(again, m); json_free(saved); routes_free(routes, n);
    // Not an OpenAPI document.
    stub.body = "{\"operations\":[{\"name\":\"projects\",\"readOnly\":true}]}";
    CHECK(!api_catalog(c, &routes, &n, &e)); CHECK(e.kind == API_NON_JSON);
    api_error_clear(&e); api_client_release(c); stub_reset(&stub);
}
static void test_discovery_says_whether_the_server_transcribes(void) {
    const char *device = "{\"id\":\"d\",\"label\":\"Laptop\",\"repos\":[],\"permission\":\"manage\",\"expiresAt\":0}";
    char *a = xstrfmt("{\"version\":1,\"client\":%s}", device), *b = xstrfmt("{\"version\":1,\"client\":%s,\"transcribe\":true}", device), *s = xstrfmt("{\"device\":%s,\"routes\":[]}", device);
    Json *ja = json_parsez(a), *jb = json_parsez(b), *js = json_parsez(s);
    Discovery d;
    CHECK(discovery_parse(ja, &d)); CHECK(d.transcribe == -1); discovery_free(&d);
    CHECK(discovery_parse(jb, &d)); CHECK(d.transcribe == 1); discovery_free(&d);
    Connection saved; CHECK(connection_parse(js, &saved)); CHECK(saved.transcribe == -1); connection_free(&saved);
    // A connection saved by the mobile API's client is paired again rather than read.
    Json *old = json_parsez("{\"device\":{\"id\":\"d\",\"label\":\"Phone\",\"repos\":[],\"permission\":\"manage\",\"expiresAt\":0},\"operations\":[]}");
    CHECK(!connection_parse(old, &saved)); json_free(old);
    CHECK(discovery_voice_notes_off(1) == NULL);
    CHECK(strstr(discovery_voice_notes_off(0), "OPENAI_TRANSCRIBE_API_KEY") != NULL);
    CHECK(strstr(discovery_voice_notes_off(-1), "Update Briareus") != NULL);
    json_free(ja); json_free(jb); json_free(js); free(a); free(b); free(s);
}
static void test_oversized_write_never_leaves_client(void) {
    Stub stub = { 0 }; stub.status = 200; stub.content_type = "application/json"; stub.body = "{}";
    ApiClient *c = client(&stub);
    char *big = xmalloc(1048577); memset(big, 'x', 1048576); big[1048576] = 0;
    Json *args = json_object(); json_set_str(args, "sessionId", "abc"); json_set_str(args, "text", big);
    ApiError e; api_error_init(&e);
    CHECK(api_call(c, "message", args, 0, &e) == NULL); CHECK(e.kind == API_OVERSIZED_REQUEST); CHECK(stub.calls == 0);
    free(big); json_free(args); api_error_clear(&e); api_client_release(c); stub_reset(&stub);
}
static void test_retry_after_http_date(void) {
    CHECK(api_retry_after("Thu, 01 Jan 1970 00:02:00 GMT", 0) == 120);
    CHECK(api_retry_after("nonsense", 0) < 0); CHECK(api_retry_after("-10", 0) == 0); CHECK(api_retry_after(NULL, 0) < 0);
}

// MARK: - Models

static void test_transcript_deduplicates_sorts_and_advances_unknown_events(void) {
    Json *events = json_parsez("[{\"seq\":3,\"kind\":\"future_event\"},{\"seq\":1,\"kind\":\"user\",\"text\":\"Hi\"},{\"seq\":2,\"kind\":\"text\",\"text\":\"Hello\"},{\"seq\":2,\"kind\":\"text\",\"text\":\"Hello\"}]");
    Transcript t; transcript_init(&t);
    transcript_append(&t, events); transcript_append(&t, events);
    CHECK(t.count == 3 && t.events[0].seq == 1 && t.events[1].seq == 2 && t.events[2].seq == 3); CHECK(t.cursor == 3);
    int visible = 0; for (size_t i = 0; i < t.count; i++) visible += event_visible(&t.events[i]);
    CHECK(visible == 2);
    Json *empty = json_array(); transcript_append(&t, empty); CHECK(t.cursor == 3); json_free(empty);
    Transcript fresh; transcript_init(&fresh); CHECK(fresh.cursor == 0);
    transcript_free(&t); json_free(events);
}
static void test_session_reads_review_loop_and_held_triage(void) {
    Json *list = json_parsez("[{\"id\":\"a\",\"status\":\"idle\",\"reviewLoop\":{\"triage\":{\"round\":2,\"findings\":[{\"key\":\"k1\"}]}}},"
                             "{\"id\":\"b\",\"status\":\"idle\",\"reviewBranch\":\"feature\",\"reviewTriage\":{\"mine\":false,\"findings\":[{\"key\":\"k2\"}]},\"reviewLoop\":null},"
                             "{\"id\":\"c\",\"status\":\"closed\",\"local\":false}]");
    Session *s; size_t n; CHECK(sessions_parse(list, &s, &n)); CHECK(n == 3);
    CHECK(session_review_loop_on(&s[0]) && !session_review_loop_on(&s[1]) && !session_review_loop_on(&s[2]));
    CHECK(session_can_review_loop(&s[0]) && !session_can_review_loop(&s[1]) && !session_can_review_loop(&s[2]));
    CHECK_STR(json_str(json_get(json_at(json_get(session_held_triage(&s[0]), "findings"), 0), "key")), "k1");
    CHECK_STR(json_str(json_get(json_at(json_get(session_held_triage(&s[1]), "findings"), 0), "key")), "k2");
    CHECK(session_held_triage(&s[2]) == NULL);
    sessions_free(s, n); json_free(list);
}
static void test_findings_queue_lists_held_rounds_oldest_first_by_pull_request(void) {
    Json *list = json_parsez("[{\"id\":\"a\",\"repo\":\"o/r\",\"status\":\"idle\",\"reviewLoop\":{\"triage\":{\"round\":2,\"prNumber\":7,\"heldAt\":\"2026-09-29T10:00:00Z\",\"findings\":[{\"key\":\"k1\"}]}},\"prStatus\":{\"number\":7,\"url\":\"https://github.com/o/r/pull/7\"}},"
                             "{\"id\":\"b\",\"repo\":\"o/r\",\"status\":\"idle\",\"reviewTriage\":{\"standalone\":true,\"mine\":false,\"prNumber\":9,\"heldAt\":\"2026-09-29T09:00:00Z\",\"findings\":[]},\"prStatus\":{\"number\":12,\"url\":\"https://github.com/o/r/pull/12\"}},"
                             "{\"id\":\"c\",\"repo\":\"o/r\",\"status\":\"idle\",\"reviewTriage\":null,\"reviewLoop\":{\"on\":true,\"triage\":null}},"
                             "{\"id\":\"d\",\"repo\":\"o/r\",\"status\":\"idle\",\"reviewTriage\":{\"standalone\":true,\"mine\":true,\"prNumber\":7,\"findings\":[{\"key\":\"k3\"}]}}]");
    Session *s; size_t n; CHECK(sessions_parse(list, &s, &n)); CHECK(n == 4);
    // A review whose every finding was deleted still waits to be completed; the old helper leaves it out.
    CHECK(session_held_round(&s[1]) != NULL && session_held_triage(&s[1]) == NULL);
    CHECK(session_held_round(&s[2]) == NULL);
    size_t count; HeldRound *rounds = sessions_held_rounds(s, n, &count);
    CHECK(count == 3);
    CHECK(rounds[0].index == 3 && rounds[1].index == 1 && rounds[2].index == 0);   // no hold time sorts first, then the oldest
    CHECK(held_round_is_mine(rounds[2].held) && !held_round_is_mine(rounds[1].held) && held_round_is_mine(rounds[0].held));
    CHECK(held_round_pr_number(rounds[1].held) == 9);
    char *own = held_round_pr_url(&s[0], rounds[2].held), *moved = held_round_pr_url(&s[1], rounds[1].held);
    CHECK_STR(own, "https://github.com/o/r/pull/7"); CHECK_STR(moved, "https://github.com/o/r/pull/9");
    free(own); free(moved); free(rounds);
    bool danger;
    Json *fixing = json_parsez("{\"fixing\":true}"), *nothing = json_parsez("{\"dismissed\":false}"), *done = json_parsez("{\"completed\":true,\"prNumber\":9}");
    char *t1 = triage_outcome_text(fixing, &danger); CHECK_STR(t1, "Verdicts recorded; a fix session is running."); CHECK(!danger);
    char *t2 = triage_outcome_text(nothing, &danger); CHECK(str_has_prefix(t2, "Verdicts recorded, but no fix session started")); CHECK(danger);
    char *t3 = triage_outcome_text(done, &danger); CHECK_STR(t3, "Review completed; what it found stays on PR #9 for its author."); CHECK(!danger);
    free(t1); free(t2); free(t3); json_free(fixing); json_free(nothing); json_free(done);
    char *s0 = findings_subtitle(0, 0), *s1 = findings_subtitle(1, 1), *s2 = findings_subtitle(3, 2);
    CHECK_STR(s0, "nothing is waiting"); CHECK_STR(s1, "1 review waiting for a decision"); CHECK_STR(s2, "3 reviews on 2 pull requests waiting for a decision");
    free(s0); free(s1); free(s2);
    sessions_free(s, n); json_free(list);
}
static void test_setup_events_are_shown_and_status_hidden(void) {
    Json *events = json_parsez("[{\"seq\":1,\"kind\":\"setup\",\"text\":\"SQLSTATE[HY000] Connection refused\"},{\"seq\":2,\"kind\":\"status\",\"status\":\"failed\"},{\"seq\":3,\"kind\":\"text\",\"text\":\"Done\"}]");
    Transcript t; transcript_init(&t); transcript_append(&t, events);
    CHECK(event_visible(&t.events[0]) && !event_visible(&t.events[1]) && event_visible(&t.events[2]));
    transcript_free(&t); json_free(events);
}
static void test_optional_fields_and_unknown_statuses_do_not_break_decoding(void) {
    Json *j = json_parsez("{\"session\":{\"id\":\"x\",\"status\":\"future\",\"title\":null,\"model\":null,\"unknown\":true},\"events\":null}");
    Session s; CHECK(session_parse(json_get(j, "session"), &s));
    CHECK_STR(session_display_title(&s), "New conversation"); CHECK(!session_is_active(&s)); CHECK(json_is_null(json_get(j, "events")));
    session_free(&s); json_free(j);
}
static void test_session_finds_its_pull_request(void) {
    const char *cases[] = { "{\"id\":\"s\",\"status\":\"idle\"}", "{\"id\":\"s\",\"status\":\"idle\",\"prStatus\":null,\"startedOnPr\":null}",
                            "{\"id\":\"s\",\"status\":\"idle\",\"startedOnPr\":4}", "{\"id\":\"s\",\"status\":\"idle\",\"prStatus\":{\"number\":9,\"state\":\"open\"},\"startedOnPr\":4}" };
    int expected[] = { 0, 0, 4, 9 };
    for (int i = 0; i < 4; i++) { Json *j = json_parsez(cases[i]); Session s; session_parse(j, &s); CHECK(session_pull_number(&s) == expected[i]); session_free(&s); json_free(j); }
}
static void test_markdown_blocks(void) {
    size_t n; MdBlock *b = md_parse("## Plan\nFirst **line**\nsame paragraph\n\n- one\n  wrapped\n2. two\n\n```swift\nlet x = 1\n\n```\n> quoted\n---\ntail", &n);
    CHECK(n == 8);
    if (n == 8) {
        CHECK(b[0].kind == MD_HEADING && b[0].level == 2); CHECK_STR(b[0].text, "Plan");
        CHECK(b[1].kind == MD_PARAGRAPH); CHECK_STR(b[1].text, "First **line**\nsame paragraph");
        CHECK(b[2].kind == MD_BULLET && b[2].indent == 0); CHECK_STR(b[2].marker, "\xE2\x80\xA2"); CHECK_STR(b[2].text, "one\nwrapped");
        CHECK(b[3].kind == MD_BULLET); CHECK_STR(b[3].marker, "2."); CHECK_STR(b[3].text, "two");
        CHECK(b[4].kind == MD_CODE); CHECK_STR(b[4].language, "swift"); CHECK_STR(b[4].text, "let x = 1\n");
        CHECK(b[5].kind == MD_QUOTE); CHECK_STR(b[5].text, "quoted");
        CHECK(b[6].kind == MD_RULE);
        CHECK(b[7].kind == MD_PARAGRAPH); CHECK_STR(b[7].text, "tail");
    }
    md_free(b, n);
    b = md_parse("```\nunterminated", &n); CHECK(n == 1 && b[0].kind == MD_CODE && b[0].language == NULL); CHECK_STR(b[0].text, "unterminated"); md_free(b, n);
    b = md_parse("#hashtag", &n); CHECK(n == 1 && b[0].kind == MD_PARAGRAPH); CHECK_STR(b[0].text, "#hashtag"); md_free(b, n);
    size_t sn; MdSpan *spans = md_inline("Use `x_y` and **bold** with [a link](https://example.com/p) and snake_case_name", &sn);
    CHECK(sn == 7);
    if (sn == 7) {
        CHECK_STR(spans[0].text, "Use "); CHECK(spans[1].flags == SPAN_CODE); CHECK_STR(spans[1].text, "x_y");
        CHECK(spans[3].flags == SPAN_BOLD); CHECK_STR(spans[3].text, "bold");
        CHECK(spans[5].flags & SPAN_LINK); CHECK_STR(spans[5].url, "https://example.com/p");
    } else { for (size_t i = 0; i < sn; i++) printf("    span %u '%s'\n", spans[i].flags, spans[i].text); }
    md_spans_free(spans, sn);
    char *plain = md_plain("**a** _b_ `c`"); CHECK_STR(plain, "a b c"); free(plain);
    // Tables, task lists, strikethrough, images and entities.
    b = md_parse("| Name | Count |\n|:-----|------:|\n| a \\| b | 1 |\n| c |\n\n- [ ] todo\n- [x] done\n* [X] also", &n);
    CHECK(n == 4);
    if (n == 4) {
        CHECK(b[0].kind == MD_TABLE && b[0].rows == 3 && b[0].cols == 2); CHECK_STR(b[0].aligns, "lr");
        CHECK_STR(b[0].cells[0], "Name"); CHECK_STR(b[0].cells[2], "a | b"); CHECK_STR(b[0].cells[3], "1"); CHECK_STR(b[0].cells[4], "c"); CHECK_STR(b[0].cells[5], "");
        char *source = md_table_source(&b[0]);
        CHECK_STR(source, "| Name | Count |\n| --- | ---: |\n| a \\| b | 1 |\n| c |  |"); free(source);
        CHECK(b[1].kind == MD_BULLET && b[1].task == 1); CHECK_STR(b[1].text, "todo");
        CHECK(b[2].task == 2); CHECK_STR(b[2].text, "done"); CHECK(b[3].task == 2); CHECK_STR(b[3].text, "also");
    }
    md_free(b, n);
    b = md_parse("a | b\nnot a table", &n); CHECK(n == 1 && b[0].kind == MD_PARAGRAPH); md_free(b, n);
    spans = md_inline("~~gone~~ &amp; ![pic](https://x.example/p.png) 5 &lt; 6", &sn);
    CHECK(sn == 4);
    if (sn == 4) { CHECK(spans[0].flags == SPAN_STRIKE); CHECK_STR(spans[0].text, "gone"); CHECK_STR(spans[1].text, " & "); CHECK(spans[2].flags & SPAN_LINK); CHECK_STR(spans[2].text, "pic"); CHECK_STR(spans[2].url, "https://x.example/p.png"); CHECK_STR(spans[3].text, " 5 < 6"); }
    else { for (size_t i = 0; i < sn; i++) printf("    span %u '%s'\n", spans[i].flags, spans[i].text); }
    md_spans_free(spans, sn);
}
static void test_runtime_catalog_resolves_choices_per_model(void) {
    Json *j = json_parsez("{\"default\":{\"providerId\":2,\"model\":\"opus\",\"effort\":\"high\"},\"providers\":[{\"id\":1,\"label\":\"Codex\",\"available\":false,\"models\":[{\"id\":\"gpt\",\"label\":\"gpt\",\"efforts\":[\"low\"],\"defaultEffort\":\"low\"}],\"defaultModel\":\"gpt\"},{\"id\":2,\"label\":\"Claude\",\"available\":true,\"models\":[{\"id\":\"sonnet\",\"label\":\"Sonnet\",\"efforts\":[\"low\",\"medium\"],\"defaultEffort\":\"medium\"},{\"id\":\"opus\",\"label\":\"opus\",\"efforts\":[\"low\",\"high\"],\"defaultEffort\":\"high\"}],\"defaultModel\":\"opus\",\"future\":1}]}");
    RuntimeCatalog c; CHECK(runtime_catalog_parse(j, &c));
    CHECK(c.has_default && c.def.provider_id == 2); CHECK_STR(c.def.model, "opus"); CHECK_STR(c.def.effort, "high");
    RuntimeChoice ch;
    CHECK(runtime_catalog_choice(&c, 2, NULL, &ch)); CHECK_STR(ch.model, "opus"); CHECK_STR(ch.effort, "high"); runtime_choice_free(&ch);
    CHECK(runtime_catalog_choice(&c, 2, "sonnet", &ch)); CHECK_STR(ch.model, "sonnet"); CHECK_STR(ch.effort, "medium");
    size_t en; const char *const *efforts = runtime_catalog_efforts(&c, &ch, &en); CHECK(en == 2 && str_eq(efforts[0], "low") && str_eq(efforts[1], "medium"));
    runtime_choice_free(&ch);
    CHECK(runtime_catalog_choice(&c, 2, "gone", &ch)); CHECK_STR(ch.model, "opus"); runtime_choice_free(&ch);
    CHECK(!runtime_catalog_choice(&c, 9, NULL, &ch));
    CHECK(runtime_catalog_first_available(&c, &ch)); CHECK(ch.provider_id == 2); runtime_choice_free(&ch);
    char *label = runtime_catalog_label(&c, &c.def); CHECK_STR(label, "Claude \xC2\xB7 opus"); free(label);
    Json *args = runtime_choice_arguments(&c.def);
    CHECK(json_num_or(json_get(args, "provider"), 0) == 2); CHECK(json_is_null(json_get(args, "providerId"))); CHECK_STR(json_str(json_get(args, "model")), "opus"); CHECK_STR(json_str(json_get(args, "effort")), "high");
    json_free(args);
    RuntimeChoice bare = { 3, NULL, NULL }; args = runtime_choice_arguments(&bare); CHECK(json_count(args) == 1); json_free(args);
    Json *round = runtime_catalog_json(&c); RuntimeCatalog again; CHECK(runtime_catalog_parse(round, &again)); CHECK(again.provider_count == 2); runtime_catalog_free(&again); json_free(round);
    runtime_catalog_free(&c); json_free(j);
    j = json_parsez("{\"default\":null,\"providers\":[]}"); CHECK(runtime_catalog_parse(j, &c)); CHECK(!c.has_default); CHECK(!runtime_catalog_first_available(&c, &ch)); runtime_catalog_free(&c); json_free(j);
}
static void test_pull_file_pages_pin_later_pages_to_the_first_revision(void) {
    Json *first = json_parsez("{\"pr\":{\"number\":7,\"headSha\":\"h1\",\"baseSha\":\"b1\",\"body\":\"Why\"},\"files\":[{\"filename\":\"src/a.js\",\"previousFilename\":null,\"status\":\"modified\",\"additions\":3,\"deletions\":1,\"patch\":\"@@ -1 +1 @@\",\"url\":\"https://github.com/o/r/blob/x/src/a.js\"}],\"nextPage\":2,\"truncated\":false}");
    Json *second = json_parsez("{\"pr\":{\"number\":7,\"headSha\":\"h1\",\"baseSha\":\"b1\"},\"files\":[{\"filename\":\"src/a.js\"},{\"filename\":\"logo.png\",\"status\":\"added\",\"patch\":null}],\"nextPage\":null}");
    PullFileList list; pull_file_list_init(&list);
    Json *args = pull_file_list_arguments(&list, "o/r", 7);
    CHECK(json_count(args) == 2 && json_num_or(json_get(args, "pr"), 0) == 7); json_free(args);
    PullFilesPage p1; CHECK(pull_files_page_parse(first, &p1)); pull_file_list_append(&list, &p1); pull_files_page_free(&p1);
    args = pull_file_list_arguments(&list, "o/r", 7);
    CHECK(json_num_or(json_get(args, "page"), 0) == 2); CHECK_STR(json_str(json_get(args, "headSha")), "h1"); CHECK_STR(json_str(json_get(args, "baseSha")), "b1"); json_free(args);
    PullFilesPage p2; CHECK(pull_files_page_parse(second, &p2)); pull_file_list_append(&list, &p2); pull_files_page_free(&p2);
    CHECK(list.file_count == 2); CHECK_STR(list.files[0].filename, "src/a.js"); CHECK_STR(list.files[1].filename, "logo.png");
    CHECK_STR(json_str(json_get(list.pr, "body")), "Why");
    CHECK(pull_file_list_arguments(&list, "o/r", 7) == NULL); CHECK(!list.truncated);
    CHECK_STR(pull_file_name(&list.files[0]), "a.js"); char *dir = pull_file_directory(&list.files[0]); CHECK_STR(dir, "src"); free(dir);
    CHECK(list.files[1].patch == NULL); dir = pull_file_directory(&list.files[1]); CHECK_STR(dir, ""); free(dir);
    pull_file_list_free(&list); json_free(first); json_free(second);
}
static void test_diff_lines_are_numbered_from_hunk_headers(void) {
    size_t n; DiffLine *lines = diff_parse("@@ -10,3 +10,4 @@ func a()\n one\n-two\n+2\n+3\n\n\\ No newline at end of file\n@@ -40 +41 @@\n-x\r\n+y\n", &n);
    CHECK(n == 10);
    if (n == 10) {
        DiffKind kinds[] = { DIFF_HUNK, DIFF_CONTEXT, DIFF_REMOVED, DIFF_ADDED, DIFF_ADDED, DIFF_CONTEXT, DIFF_NOTE, DIFF_HUNK, DIFF_REMOVED, DIFF_ADDED };
        int olds[] = { 0, 10, 11, 0, 0, 12, 0, 0, 40, 0 }, news[] = { 0, 10, 0, 11, 12, 13, 0, 0, 0, 41 };
        for (size_t i = 0; i < 10; i++) { CHECK(lines[i].kind == kinds[i]); CHECK(lines[i].old_line == olds[i]); CHECK(lines[i].new_line == news[i]); }
        CHECK_STR(lines[2].text, "two"); CHECK_STR(lines[6].text, "No newline at end of file"); CHECK_STR(lines[8].text, "x");
    }
    diff_free(lines, n);
    lines = diff_parse("", &n); CHECK(n == 0); diff_free(lines, n);
}

// MARK: - Cache

static wchar_t *temp_directory(void) {
    wchar_t base[MAX_PATH]; GetTempPathW(MAX_PATH, base);
    wchar_t *dir = xmalloc(MAX_PATH * sizeof(wchar_t));
    swprintf(dir, MAX_PATH, L"%lsbriareus-test-%lu-%lu", base, (unsigned long)GetCurrentProcessId(), (unsigned long)GetTickCount());
    return dir;
}
static int count_files(const wchar_t *dir, bool *suspicious) {
    wchar_t pattern[MAX_PATH]; swprintf(pattern, MAX_PATH, L"%ls\\*", dir);
    WIN32_FIND_DATAW f; HANDLE h = FindFirstFileW(pattern, &f); int n = 0; *suspicious = false;
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (f.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        n++;
        if (f.cFileName[0] == L'.' || wcschr(f.cFileName, L'/') || wcschr(f.cFileName, L'\\')) *suspicious = true;
    } while (FindNextFileW(h, &f));
    FindClose(h);
    return n;
}
static void test_cache_restores_transcript_so_only_new_events_are_requested(void) {
    wchar_t *dir = temp_directory();
    DiskCache *cache = cache_new(dir);
    Json *saved = cache_lines(cache, "transcript:s/1"); CHECK(json_count(saved) == 0); json_free(saved);
    Json *first = json_parsez("[{\"seq\":1,\"kind\":\"user\",\"text\":\"Hi\\nthere\"},{\"seq\":2,\"kind\":\"tool\",\"name\":\"Bash\",\"summary\":\"ls\",\"options\":[{\"label\":\"a\"}]}]");
    Json *second = json_parsez("[{\"seq\":2,\"kind\":\"tool\"},{\"seq\":3,\"kind\":\"result\",\"costUsd\":0.5,\"isError\":false}]");
    CHECK(cache_append(cache, first, "transcript:s/1"));
    // A write cut short must cost only its own line.
    char *name = cache_file_name("transcript:s/1"); wchar_t *wname = utf8_to_wide(name);
    wchar_t path[MAX_PATH]; swprintf(path, MAX_PATH, L"%ls\\%ls", dir, wname);
    HANDLE h = CreateFileW(path, FILE_APPEND_DATA, 0, NULL, OPEN_EXISTING, 0, NULL); CHECK(h != INVALID_HANDLE_VALUE);
    DWORD w; WriteFile(h, "\n{\"seq\":9,\"kin", 14, &w, NULL); CloseHandle(h);
    free(name); free(wname);
    CHECK(cache_append(cache, second, "transcript:s/1"));
    Transcript t; transcript_init(&t);
    Json *lines = cache_lines(cache, "transcript:s/1"); transcript_append(&t, lines); json_free(lines);
    CHECK(t.count == 3 && t.events[0].seq == 1 && t.events[1].seq == 2 && t.events[2].seq == 3 && t.cursor == 3);
    if (t.count == 3) {
        CHECK_STR(t.events[0].text, "Hi\nthere"); CHECK_STR(event_detail(&t.events[1]), "ls"); CHECK(t.events[2].has_cost && t.events[2].cost_usd == 0.5);
        time_t when; CHECK(!event_time(&t.events[2], &when));
    }
    Json *timed = json_parsez("{\"seq\":4,\"kind\":\"text\",\"t\":\"2026-09-28T15:55:49.120Z\",\"text\":\"Hi\"}");
    Event e; CHECK(event_parse(timed, &e)); time_t when; CHECK(event_time(&e, &when) && when == 1790610949); event_free(&e); json_free(timed);
    CHECK(cache_replace(cache, second, "transcript:s/1"));
    lines = cache_lines(cache, "transcript:s/1"); CHECK(json_count(lines) == 2 && json_num_or(json_get(json_at(lines, 0), "seq"), 0) == 2); json_free(lines);
    cache_remove(cache, "transcript:s/1");
    lines = cache_lines(cache, "transcript:s/1"); CHECK(json_count(lines) == 0); json_free(lines);
    transcript_free(&t); json_free(first); json_free(second);
    cache_remove_all(cache); cache_free(cache); free(dir);
}
static void test_cache_keeps_values_per_key_inside_its_directory(void) {
    wchar_t *dir = temp_directory();
    DiskCache *cache = cache_new(dir);
    Json *sessions = json_parsez("[{\"id\":\"a\",\"status\":\"running\",\"title\":\"One\",\"reviewLoop\":{\"triage\":{\"findings\":[{\"key\":\"k\"}]}},\"prStatus\":{\"number\":4},\"local\":false,\"queued\":[{\"text\":\"next\"}]}]");
    CHECK(cache_store(cache, sessions, "sessions:o/r"));
    Json *projects = json_parsez("[{\"repo\":\"o/r\",\"label\":\"Repo\"}]");
    CHECK(cache_store(cache, projects, "../../projects"));
    bool suspicious; CHECK(count_files(dir, &suspicious) == 2); CHECK(!suspicious);
    Json *found = cache_value(cache, "sessions:o/r"); CHECK(found != NULL);
    Session *s; size_t n; CHECK(sessions_parse(found, &s, &n) && n == 1);
    CHECK_STR(json_str(json_get(s[0].raw, "title")), "One"); CHECK(session_review_loop_on(&s[0]) && session_can_review_loop(&s[0]));
    CHECK(session_pull_number(&s[0]) == 4); CHECK_STR(json_str(json_get(json_at(session_queued(&s[0]), 0), "text")), "next");
    CHECK(json_count(json_get(session_held_triage(&s[0]), "findings")) == 1);
    sessions_free(s, n); json_free(found);
    CHECK(cache_value(cache, "sessions:o/other") == NULL);
    // Unchanged content costs no write.
    CHECK(cache_store(cache, sessions, "sessions:o/r"));
    cache_prune(cache, 60, time(NULL));
    found = cache_value(cache, "../../projects"); CHECK(found != NULL); json_free(found);
    cache_prune(cache, 60, time(NULL) + 3600);
    CHECK(cache_value(cache, "../../projects") == NULL);
    CHECK(cache_store(cache, sessions, "sessions:o/r"));
    cache_remove_all(cache);
    CHECK(cache_value(cache, "sessions:o/r") == NULL);
    json_free(sessions); json_free(projects); cache_free(cache); free(dir);
}
static void test_saved_pull_files_are_kept_only_for_the_same_revision(void) {
    PullFileList list; pull_file_list_init(&list);
    Json *j = json_parsez("{\"pr\":{\"headSha\":\"h1\",\"baseSha\":\"b1\",\"body\":\"Old\"},\"files\":[{\"filename\":\"a\"},{\"filename\":\"b\"}],\"nextPage\":null}");
    PullFilesPage p; CHECK(pull_files_page_parse(j, &p)); pull_file_list_append(&list, &p); pull_files_page_free(&p); json_free(j);
    Json *round = pull_file_list_json(&list); PullFileList saved; CHECK(pull_file_list_parse(round, &saved)); json_free(round);
    CHECK(saved.file_count == 2 && saved.next_page == 0);
    j = json_parsez("{\"pr\":{\"headSha\":\"h1\",\"baseSha\":\"b1\",\"body\":\"New\"},\"files\":[{\"filename\":\"a\"}],\"nextPage\":2}");
    CHECK(pull_files_page_parse(j, &p)); CHECK(pull_file_list_confirm(&saved, &p)); pull_files_page_free(&p); json_free(j);
    CHECK_STR(json_str(json_get(saved.pr, "body")), "New"); CHECK(saved.file_count == 2 && saved.next_page == 0);
    j = json_parsez("{\"pr\":{\"headSha\":\"h2\",\"baseSha\":\"b1\"},\"files\":[],\"nextPage\":null}");
    CHECK(pull_files_page_parse(j, &p)); CHECK(!pull_file_list_confirm(&saved, &p)); pull_files_page_free(&p); json_free(j);
    j = json_parsez("{\"pr\":{\"headSha\":\"h1\",\"baseSha\":\"b2\"},\"files\":[],\"nextPage\":null}");
    CHECK(pull_files_page_parse(j, &p)); CHECK(!pull_file_list_confirm(&saved, &p)); pull_files_page_free(&p); json_free(j);
    PullFileList empty; pull_file_list_init(&empty);
    j = json_parsez("{\"pr\":{},\"files\":[],\"nextPage\":null}");
    CHECK(pull_files_page_parse(j, &p)); CHECK(!pull_file_list_confirm(&empty, &p)); pull_files_page_free(&p); json_free(j);
    pull_file_list_free(&empty); pull_file_list_free(&saved); pull_file_list_free(&list);
}

// MARK: - Board

static Json *board(void) {
    return json_parsez(
        "{\"author\":\"TheBot\",\"pulls\":["
        "{\"number\":7,\"title\":\"Add invoices\",\"branch\":\"feat/invoices\",\"baseBranch\":\"main\",\"draft\":false,\"author\":\"thebot\",\"assignees\":[\"ana\"],"
        "\"reviewers\":[{\"user\":\"Ana\",\"state\":\"requested\"},{\"user\":\"luis\",\"state\":\"approved\"}],"
        "\"labels\":[{\"name\":\"Has-Conflicts\",\"color\":\"d93f0b\"},{\"name\":\"backend\",\"color\":\"zzz\"}],"
        "\"issues\":[{\"number\":3,\"title\":\"Invoices\",\"state\":\"closed\",\"stateReason\":\"not_planned\",\"repo\":\"acme/other\",\"labels\":[]}],"
        "\"mergeable\":\"unknown\",\"checks\":\"failure\",\"reviewDecision\":\"review_required\",\"recommended\":\"solve-conflicts\",\"updatedAt\":\"2026-09-28T15:55:49Z\"},"
        "{\"number\":8,\"title\":\"Fix login\",\"branch\":\"fix/login\",\"author\":\"ana\",\"labels\":[{\"name\":\"feedback-given\",\"color\":\"fbca04\"},{\"name\":\"backend\"}],"
        "\"reviewers\":[{\"user\":\"luis\",\"state\":\"commented\"}],\"mergeable\":\"conflicting\",\"checks\":\"pending\",\"updatedAt\":\"2026-09-28T15:55:49.120Z\"},"
        "{\"number\":9,\"title\":\"Docs\",\"branch\":\"docs\",\"author\":\"luis\",\"mergeable\":\"mergeable\"},"
        "{\"title\":\"No number\"}],"
        "\"issues\":["
        "{\"number\":20,\"title\":\"Child\",\"author\":\"ana\",\"labels\":[],\"parent\":{\"number\":21,\"title\":\"Epic\",\"repo\":\"o/r\"},\"pulls\":[{\"number\":8,\"title\":\"Fix login\",\"draft\":true,\"repo\":\"o/r\"}]},"
        "{\"number\":21,\"title\":\"Epic\",\"labels\":[{\"name\":\"backend\"}],\"subIssues\":{\"total\":3,\"completed\":1,\"open\":2}},"
        "{\"number\":22,\"title\":\"Elsewhere\",\"parent\":{\"number\":21,\"title\":\"Theirs\",\"repo\":\"acme/other\"}},"
        "{\"number\":23,\"title\":\"Loop a\",\"parent\":{\"number\":24,\"title\":\"Loop b\"}},"
        "{\"number\":24,\"title\":\"Loop b\",\"parent\":{\"number\":23,\"title\":\"Loop a\"}}]}");
}
static void test_board_rows_read_labels_conflicts_and_what_each_asks_for(void) {
    Json *b = board(); size_t n; PullSummary *pulls = pull_summaries_parse(json_get(b, "pulls"), &n);
    CHECK(n == 3 && pulls[0].number == 7 && pulls[1].number == 8 && pulls[2].number == 9);
    CHECK(pulls[0].label_count == 2); CHECK_STR(pulls[0].labels[0].name, "Has-Conflicts"); CHECK_STR(pulls[0].labels[1].name, "backend");
    int rgb[3]; CHECK(pull_label_rgb(&pulls[0].labels[0], rgb) && rgb[0] == 0xd9 && rgb[1] == 0x3f && rgb[2] == 0x0b);
    CHECK(!pull_label_rgb(&pulls[0].labels[1], rgb));
    CHECK(!pull_conflicting(&pulls[0]) && pull_has_conflicts(&pulls[0]) && pull_checks_failed(&pulls[0]));
    CHECK(pull_conflicting(&pulls[1]) && pull_awaits_feedback(&pulls[1]) && !pull_checks_failed(&pulls[1]));
    CHECK(!pull_has_conflicts(&pulls[2]) && pulls[2].checks == NULL && pulls[2].label_count == 0);
    CHECK_STR(pulls[0].recommended, "solve-conflicts");
    CHECK(pulls[0].reviewer_count == 2); CHECK_STR(pulls[0].reviewers[0].state, "requested"); CHECK_STR(pulls[0].reviewers[1].state, "approved");
    CHECK(pulls[0].has_updated && pulls[1].has_updated && !pulls[2].has_updated);
    CHECK(pulls[0].issue_count == 1 && board_link_not_planned(&pulls[0].issues[0]));
    char *ref = board_link_reference(&pulls[0].issues[0], "o/r"); CHECK_STR(ref, "acme/other#3"); free(ref);
    CHECK_STR(json_str(json_get(pulls[0].raw, "branch")), "feat/invoices");
    pull_summaries_free(pulls, n); json_free(b);
}
static char *options_text(const FilterOption *o, size_t n, bool value) {
    Str s; str_init(&s);
    for (size_t i = 0; i < n; i++) str_appendf(&s, "%s%s %d", i ? "," : "", value ? o[i].value : o[i].text, o[i].count);
    return str_detach(&s);
}
static char *passing(const BoardFilter *f, const PullSummary *pulls, const BoardRow *rows, size_t n) {
    Str s; str_init(&s);
    for (size_t i = 0; i < n; i++) if (board_filter_passes(f, &rows[i], -1)) str_appendf(&s, "%s%d", s.len ? "," : "", pulls[i].number);
    return str_detach(&s);
}
static void test_board_filter_counts_each_picker_against_the_others(void) {
    Json *b = board(); size_t n; PullSummary *pulls = pull_summaries_parse(json_get(b, "pulls"), &n);
    BoardRow rows[3]; for (size_t i = 0; i < n; i++) rows[i] = pull_board_row(&pulls[i]);
    BoardFilter f; board_filter_opening(&f, "TheBot", rows, n);
    CHECK_STR(f.author, "thebot");
    char *p = passing(&f, pulls, rows, n); CHECK_STR(p, "7"); free(p);
    size_t on; FilterOption *o = board_filter_options(&f, FILTER_AUTHOR, rows, n, &on);
    char *t = options_text(o, on, false); CHECK_STR(t, "ana 1,luis 1,thebot 1"); free(t); filter_options_free(o, on);
    o = board_filter_options(&f, FILTER_LABEL, rows, n, &on); t = options_text(o, on, true); CHECK_STR(t, "backend 1,has-conflicts 1"); free(t); filter_options_free(o, on);
    board_filter_set(&f, FILTER_AUTHOR, ""); board_filter_set(&f, FILTER_LABEL, "Backend");
    p = passing(&f, pulls, rows, n); CHECK_STR(p, "7,8"); free(p);
    o = board_filter_options(&f, FILTER_REVIEWER, rows, n, &on); t = options_text(o, on, false); CHECK_STR(t, "Ana 1,luis 2"); free(t); filter_options_free(o, on);
    board_filter_set(&f, FILTER_REVIEWER, "ANA");
    p = passing(&f, pulls, rows, n); CHECK_STR(p, "7"); free(p);
    // A pick the other filters have emptied still lists itself, so the board can be widened again.
    board_filter_set(&f, FILTER_AUTHOR, "luis");
    p = passing(&f, pulls, rows, n); CHECK_STR(p, ""); free(p);
    o = board_filter_options(&f, FILTER_REVIEWER, rows, n, &on); CHECK(on == 1); if (on == 1) { CHECK_STR(o[0].value, "ana"); CHECK_STR(o[0].text, "ana"); CHECK(o[0].count == 0); } filter_options_free(o, on);
    CHECK(board_filter_is_on(&f));
    board_filter_free(&f);
    board_filter_opening(&f, "ghost", rows, n); CHECK(!board_filter_is_on(&f)); board_filter_free(&f);
    board_filter_opening(&f, NULL, rows, n); CHECK(!board_filter_is_on(&f)); board_filter_free(&f);
    pull_summaries_free(pulls, n); json_free(b);
}
static char *ids(const BoardAction *a, size_t n) { Str s; str_init(&s); for (size_t i = 0; i < n; i++) str_appendf(&s, "%s%s", i ? "," : "", a[i].id); return str_detach(&s); }
static bool has_id(const BoardAction *a, size_t n, const char *id) { for (size_t i = 0; i < n; i++) if (str_eq(a[i].id, id)) return true; return false; }
static void test_board_offers_the_errands_a_pull_request_is_in_a_state_for(void) {
    Json *b = board(); size_t n; PullSummary *pulls = pull_summaries_parse(json_get(b, "pulls"), &n);
    size_t an; BoardAction *a = board_actions_offered(NULL, &pulls[0], 0, &an);
    char *s = ids(a, an); CHECK_STR(s, "run,review,solve-conflicts,fix-checks,custom-feedback,test-sheet,test-run,pr-body-summary,delete-self-comments"); free(s); board_actions_free(a, an);
    a = board_actions_offered(NULL, &pulls[1], 0, &an);
    s = ids(a, an); CHECK_STR(s, "run,review,solve-conflicts,implement-feedback,custom-feedback,test-sheet,test-run,pr-body-summary,delete-self-comments"); free(s); board_actions_free(a, an);
    a = board_actions_offered(NULL, &pulls[2], 0, &an);
    CHECK(!has_id(a, an, "solve-conflicts") && !has_id(a, an, "fix-checks") && !has_id(a, an, "implement-feedback")); board_actions_free(a, an);
    a = board_actions_offered(NULL, &pulls[2], 1, &an); CHECK(has_id(a, an, "fix-checks")); board_actions_free(a, an);
    a = board_actions_offered(NULL, NULL, 0, &an); CHECK(has_id(a, an, "solve-conflicts")); board_actions_free(a, an);
    Json *catalog = json_parsez("[{\"id\":\"custom-feedback\",\"label\":\"Give feedback\",\"hint\":\"h\",\"input\":{\"label\":\"Tell it\",\"placeholder\":\"e.g.\",\"required\":true}},"
                                "{\"id\":\"label-pull\",\"label\":\"Label it\",\"icon\":\"x\",\"hint\":\"Tag it\",\"input\":null},{\"label\":\"No id\"},"
                                "{\"id\":\"qa\",\"label\":\"QA\"},{\"id\":\"test-sheet\",\"label\":\"Test sheet\"},{\"id\":\"test-run\",\"label\":\"Run test sheet\"}]");
    a = board_actions_offered(catalog, &pulls[2], 0, &an);
    CHECK(an > 0); if (an) { CHECK_STR(a[an - 1].id, "label-pull"); char *op = board_action_operation(&a[an - 1]); CHECK_STR(op, "action"); free(op); }
    CHECK(!has_id(a, an, "qa") && has_id(a, an, "test-sheet") && has_id(a, an, "test-run"));
    // Run and Review are always there; an errand the server does not list is not offered.
    CHECK(has_id(a, an, "run") && has_id(a, an, "review") && has_id(a, an, "custom-feedback"));
    CHECK(!has_id(a, an, "pr-body-summary") && !has_id(a, an, "delete-self-comments"));
    const BoardAction *feedback = NULL; for (size_t i = 0; i < an; i++) if (str_eq(a[i].id, "custom-feedback")) feedback = &a[i];
    CHECK(feedback && feedback->has_input); if (feedback) CHECK_STR(feedback->input.label, "Tell it");
    if (feedback) {
        Json *args = board_action_arguments(feedback, "o/r", 9, "docs", " Use 404 \n");
        CHECK(json_count(args) == 4); CHECK_STR(json_str(json_get(args, "repo")), "o/r"); CHECK(json_num_or(json_get(args, "prNumber"), 0) == 9); CHECK_STR(json_str(json_get(args, "input")), "Use 404");
        CHECK_STR(json_str(json_get(args, "action")), "custom-feedback");
        json_free(args);
    }
    board_actions_free(a, an); json_free(catalog);
    size_t kn; const BoardAction *known = board_actions_known(&kn);
    const BoardAction *run = NULL, *solve = NULL, *review = NULL;
    for (size_t i = 0; i < kn; i++) { if (str_eq(known[i].id, "run")) run = &known[i]; if (str_eq(known[i].id, "solve-conflicts")) solve = &known[i]; if (str_eq(known[i].id, "review")) review = &known[i]; }
    Json *args;
    char *op = board_action_operation(run); CHECK_STR(op, "serve_pull"); free(op);
    op = board_action_operation(solve); CHECK_STR(op, "action"); free(op);
    op = board_action_operation(review); CHECK_STR(op, "review"); free(op);
    args = board_action_arguments(solve, "o/r", 9, "docs", NULL); CHECK(json_count(args) == 3); CHECK_STR(json_str(json_get(args, "action")), "solve-conflicts"); json_free(args);
    CHECK(board_action_timeout_ms(run) == 170000 && board_action_timeout_ms(review) == 0);
    args = board_action_arguments(review, "o/r", 9, "docs", "ignored");
    CHECK(json_count(args) == 3); CHECK_STR(json_str(json_get(args, "branch")), "docs"); CHECK(json_is_null(json_get(args, "action"))); json_free(args);
    args = board_action_arguments(run, "o/r", 9, "docs", NULL); CHECK(json_count(args) == 2); json_free(args);
    pull_summaries_free(pulls, n); json_free(b);
}
static void test_issues_nest_under_their_epic_and_word_their_own_session(void) {
    Json *b = board(); size_t n; IssueSummary *issues = issue_summaries_parse(json_get(b, "issues"), &n);
    CHECK(n == 5);
    size_t rn; IssueRow *rows = issues_nested(issues, n, "o/r", &rn);
    Str s; str_init(&s);
    for (size_t i = 0; i < rn; i++) str_appendf(&s, "%s%d:%d", i ? "," : "", issues[rows[i].index].number, rows[i].depth);
    // A child follows its epic; one whose epic is elsewhere, and a loop of parents, stay flat and are drawn once.
    CHECK_STR(s.data, "21:0,20:1,22:0,23:0,24:1"); str_free(&s); free(rows);
    CHECK(issue_is_epic(&issues[1]) && issues[1].sub_issues_done == 1 && !issue_is_epic(&issues[0]));
    CHECK(issues[0].pull_count == 1 && issues[0].pulls[0].number == 8 && issues[0].pulls[0].draft);
    rows = issues_nested(issues, 1, "o/r", &rn); CHECK(rn == 1 && rows[0].depth == 0); free(rows);
    char *prompt = issue_prompt(&issues[0], "o/r");
    CHECK(str_has_prefix(prompt, "Issue #20: Child\n")); CHECK(strstr(prompt, "gh issue view 20 --repo o/r --comments") != NULL);
    CHECK(strstr(prompt, "sub-issue of o/r#21 (Epic)") != NULL); CHECK(strstr(prompt, "`Closes #20`") != NULL); free(prompt);
    prompt = issue_prompt(&issues[2], "o/r"); CHECK(strstr(prompt, "sub-issue of acme/other#21") != NULL); free(prompt);
    prompt = issue_prompt(&issues[1], "o/r"); CHECK(strstr(prompt, "sub-issue of") == NULL); free(prompt);
    issue_summaries_free(issues, n); json_free(b);
}
static void test_merge_warnings_say_what_stands_in_the_way(void) {
    Json *yes = json_bool(true), *no = json_bool(false), *unknown = json_null();
    size_t n; char **w = merge_warnings(yes, "clean", &n); CHECK(n == 0); str_array_free(w, n);
    w = merge_warnings(no, "dirty", &n); CHECK(n == 1); str_array_free(w, n);
    w = merge_warnings(unknown, "unknown", &n); CHECK(n == 1 && strstr(w[0], "still checking")); str_array_free(w, n);
    w = merge_warnings(yes, "behind", &n); CHECK(n == 1 && strstr(w[0], "behind")); str_array_free(w, n);
    w = merge_warnings(yes, "blocked", &n); CHECK(n == 1 && strstr(w[0], "blocked")); str_array_free(w, n);
    json_free(yes); json_free(no); json_free(unknown);
}
static void test_review_status_and_stacks(void) {
    Json *reviews = json_parsez("[{\"state\":\"COMMENTED\"},{\"state\":\"approved\"}]");
    CHECK(review_status(NULL, reviews) == REVIEW_APPROVED);
    CHECK(review_status("changes_requested", reviews) == REVIEW_CHANGES_REQUESTED);
    CHECK(review_status("review_required", NULL) == REVIEW_REQUESTED);
    CHECK(review_status(NULL, NULL) == REVIEW_NONE);
    json_free(reviews);
    Json *stacks = json_parsez("{\"5\":[{\"number\":1,\"title\":\"Base\",\"depth\":1},{\"number\":2,\"title\":\"Top\",\"depth\":2,\"draft\":true}]}");
    Json *value = json_parsez("{\"id\":5,\"position\":2,\"total\":2}");
    StackPosition sp; CHECK(stack_position_parse(value, stacks, &sp));
    CHECK(sp.chain_count == 2 && sp.chain[1].draft);
    char *label = stack_position_label(&sp, 0); CHECK_STR(label, "2/2"); free(label);
    label = stack_position_label(&sp, 1); CHECK_STR(label, "1/2"); free(label);
    // The board's rows lend each item its branch and the bottom its base; the saved form keeps them, and the overview lists the top first.
    Json *rows = json_parsez("[{\"number\":1,\"branch\":\"feature/base\",\"baseBranch\":\"master\"},{\"number\":2,\"branch\":\"feature/top\",\"baseBranch\":\"feature/base\"}]");
    size_t rn; PullSummary *rs = pull_summaries_parse(rows, &rn); CHECK(rn == 2);
    stack_position_branches(&sp, rs, rn);
    CHECK_STR(sp.chain[0].branch, "feature/base"); CHECK_STR(sp.chain[1].branch, "feature/top"); CHECK_STR(sp.base, "master");
    size_t *order = stack_position_top_first(&sp); CHECK(order[0] == 1 && order[1] == 0); free(order);
    Json *saved = stack_position_json(&sp);
    StackPosition back; CHECK(stack_position_restore(saved, &back));
    CHECK(back.position == 2 && back.total == 2 && back.chain_count == 2 && back.chain[1].draft);
    CHECK_STR(back.base, "master"); CHECK_STR(back.chain[1].branch, "feature/top"); CHECK_STR(back.chain[0].title, "Base");
    stack_position_free(&back); json_free(saved);
    // A partial chain whose bottom is out of view has no known base.
    sp.partial = true; free(sp.base); sp.base = NULL; sp.chain[0].depth = 2; sp.chain[1].depth = 3;
    stack_position_branches(&sp, rs, rn); CHECK(sp.base == NULL);
    pull_summaries_free(rs, rn); json_free(rows);
    stack_position_free(&sp); json_free(stacks); json_free(value);
    CHECK(safe_web_url("https://github.com/o/r/pull/1") && !safe_web_url("http://github.com") && !safe_web_url("https://user@evil.example") && !safe_web_url("https://"));
}
static void test_json_round_trips(void) {
    Json *j = json_parsez("{\"b\":[1,2.5,\"x\\u00e9\\ud83d\\ude00\",null,true],\"a\":{\"z\":-0.001,\"y\":1e21}}");
    CHECK(j != NULL);
    char *s = json_serialize(j, true);
    CHECK_STR(s, "{\"a\":{\"y\":1e+21,\"z\":-0.001},\"b\":[1,2.5,\"x\xC3\xA9\xF0\x9F\x98\x80\",null,true]}");
    Json *again = json_parsez(s); CHECK(json_equal(j, again)); json_free(again); free(s); json_free(j);
    CHECK(json_parsez("{\"a\":}") == NULL); CHECK(json_parsez("[1,]") == NULL); CHECK(json_parsez("") == NULL); CHECK(json_parsez("nul") == NULL);
    CHECK(json_parsez("{} x") == NULL);
}

int main(void) {
    struct { const char *name; void (*fn)(void); } tests[] = {
        { "normalizes origin and API path", test_normalizes_origin_and_api_path },
        { "rejects unsafe or ambiguous addresses", test_rejects_unsafe_or_ambiguous_addresses },
        { "rejects invalid tokens", test_rejects_invalid_tokens },
        { "calls take their route and arguments from the table", test_calls_take_their_route_and_arguments_from_the_table },
        { "discovery validates version and milliseconds", test_discovery_validates_version_and_milliseconds },
        { "redirect and HTML are not accepted", test_redirect_and_html_are_not_accepted },
        { "errors keep status and Retry-After without retrying a write", test_errors_keep_status_and_retry_after_without_retrying_write },
        { "timeout does not retry a write", test_timeout_does_not_retry_write },
        { "voice note is posted as recorded and answered with its text", test_voice_note_is_posted_as_recorded_and_answered_with_its_text },
        { "attachment is posted raw and answered with its id", test_attachment_is_posted_raw_and_answered_with_its_id },
        { "catalog reads routes and who may call them from OpenAPI", test_catalog_reads_routes_and_who_may_call_them_from_openapi },
        { "discovery says whether the server transcribes", test_discovery_says_whether_the_server_transcribes },
        { "oversized write never leaves the client", test_oversized_write_never_leaves_client },
        { "Retry-After HTTP date", test_retry_after_http_date },
        { "transcript deduplicates, sorts and advances unknown events", test_transcript_deduplicates_sorts_and_advances_unknown_events },
        { "session reads review loop and held triage", test_session_reads_review_loop_and_held_triage },
        { "findings queue lists held rounds oldest first by pull request", test_findings_queue_lists_held_rounds_oldest_first_by_pull_request },
        { "setup events are shown, status events hidden", test_setup_events_are_shown_and_status_hidden },
        { "optional fields and unknown statuses do not break decoding", test_optional_fields_and_unknown_statuses_do_not_break_decoding },
        { "session finds its pull request", test_session_finds_its_pull_request },
        { "markdown blocks and inline spans", test_markdown_blocks },
        { "runtime catalog resolves choices per model", test_runtime_catalog_resolves_choices_per_model },
        { "pull file pages pin later pages to the first revision", test_pull_file_pages_pin_later_pages_to_the_first_revision },
        { "diff lines are numbered from hunk headers", test_diff_lines_are_numbered_from_hunk_headers },
        { "cache restores transcript so only new events are requested", test_cache_restores_transcript_so_only_new_events_are_requested },
        { "cache keeps values per key inside its directory", test_cache_keeps_values_per_key_inside_its_directory },
        { "saved pull files are kept only for the same revision", test_saved_pull_files_are_kept_only_for_the_same_revision },
        { "board rows read labels, conflicts and what each asks for", test_board_rows_read_labels_conflicts_and_what_each_asks_for },
        { "board filter counts each picker against the others", test_board_filter_counts_each_picker_against_the_others },
        { "board offers the errands a pull request is in a state for", test_board_offers_the_errands_a_pull_request_is_in_a_state_for },
        { "issues nest under their epic and word their own session", test_issues_nest_under_their_epic_and_word_their_own_session },
        { "merge warnings say what stands in the way", test_merge_warnings_say_what_stands_in_the_way },
        { "review status and stacks", test_review_status_and_stacks },
        { "JSON round trips", test_json_round_trips },
    };
    for (size_t i = 0; i < sizeof tests / sizeof *tests; i++) test_run(tests[i].name, tests[i].fn);
    str_tests();
    json_tests();
    markdown_tests();
    diff_tests();
    models_tests();
    board_tests();
    api_tests();
    contract_tests();
    cache_tests();
    vt_tests();
    sftp_tests();
    browser_tests();
    meet_tests();
    update_tests();
    repo_tests();
    mail_tests();
    repo_index_tests();
    return test_summary();
}
