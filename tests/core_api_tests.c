// The client API: addresses, tokens, errors, the route table and every call built from it, answers and Retry-After.
#include "api.h"
#include "json.h"
#include "models.h"
#include "str.h"
#include "suites.h"
#include "test.h"
#include "fixtures/october-2026/catalog.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TOKEN = "brm_aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
#define BASE "https://example.com/api/v1/"

// MARK: - Stub transport

typedef struct {
    int status; const char *content_type; const char *retry_after; const char *body;
    bool fail; const char *fail_message;
    int calls, last_timeout, last_header_count;
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
    Stub *s = ctx;
    s->calls++;
    s->last_timeout = timeout_ms;
    free(s->last_method); s->last_method = xstrdup(method);
    free(s->last_url); s->last_url = xstrdup(url);
    free(s->last_body); s->last_body = NULL; s->last_body_len = body_len;
    if (body_len) { s->last_body = xmalloc(body_len + 1); memcpy(s->last_body, body, body_len); s->last_body[body_len] = 0; }
    free(s->last_content_type); s->last_content_type = NULL;
    free(s->last_authorization); s->last_authorization = NULL;
    free(s->last_accept); s->last_accept = NULL;
    s->last_header_count = 0;
    for (size_t i = 0; headers && headers[i] && headers[i + 1]; i += 2) {
        s->last_header_count++;
        if (str_eq(headers[i], "Content-Type")) s->last_content_type = xstrdup(headers[i + 1]);
        if (str_eq(headers[i], "Authorization")) s->last_authorization = xstrdup(headers[i + 1]);
        if (str_eq(headers[i], "Accept")) s->last_accept = xstrdup(headers[i + 1]);
    }
    *status = 0; *content_type = NULL; *retry_after = NULL; *response = NULL; *response_len = 0; *error_message = NULL;
    if (s->fail) { *error_message = xstrdup(s->fail_message); return false; }
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
    CHECK(c != NULL); CHECK(e.kind == API_OK);
    api_client_set_transport(c, stub_transport, stub);
    return c;
}
static void stub_json(Stub *s, int status, const char *body) { s->status = status; s->content_type = "application/json"; s->body = body; s->fail = false; s->retry_after = NULL; }

// MARK: - Addresses

static void check_address(const char *input, const char *base, const char *origin, const char *host, int port) {
    ServerAddress a;
    bool ok = server_address_parse(input, &a);
    if (!ok) printf("  refused %s\n", input);
    CHECK(ok);
    if (!ok) return;
    CHECK_STR(a.base_url, base); CHECK_STR(a.origin, origin); CHECK_STR(a.host, host); CHECK_INT(a.port, port);
    server_address_free(&a);
    CHECK(a.base_url == NULL && a.origin == NULL && a.host == NULL && a.port == 0);
}
static void test_server_address_accepts_https_origins_and_the_api_base(void) {
    check_address("https://example.com", BASE, "https://example.com", "example.com", 0);
    check_address("https://example.com/", BASE, "https://example.com", "example.com", 0);
    check_address("https://example.com/api/v1/", BASE, "https://example.com", "example.com", 0);
    check_address("HTTPS://Example.COM", BASE, "https://example.com", "example.com", 0);
    check_address("HtTpS://dev.OkaNet-Solutions.com/api/v1", "https://dev.okanet-solutions.com/api/v1/", "https://dev.okanet-solutions.com", "dev.okanet-solutions.com", 0);
    check_address("\t https://example.com \r\n", BASE, "https://example.com", "example.com", 0);
    check_address("https://localhost:3000", "https://localhost:3000/api/v1/", "https://localhost:3000", "localhost", 3000);
    check_address("https://127.0.0.1:8443/", "https://127.0.0.1:8443/api/v1/", "https://127.0.0.1:8443", "127.0.0.1", 8443);
    check_address("https://my_host.internal", "https://my_host.internal/api/v1/", "https://my_host.internal", "my_host.internal", 0);
    check_address("https://example.com:1", "https://example.com:1/api/v1/", "https://example.com:1", "example.com", 1);
    check_address("https://example.com:65535", "https://example.com:65535/api/v1/", "https://example.com:65535", "example.com", 65535);
    check_address("https://example.com:443/", BASE, "https://example.com", "example.com", 0);
    check_address("https://example.com:80", "https://example.com:80/api/v1/", "https://example.com:80", "example.com", 80);
    check_address("https://[::1]", "https://[::1]/api/v1/", "https://[::1]", "[::1]", 0);
    check_address("https://[::1]:8443/api/v1", "https://[::1]:8443/api/v1/", "https://[::1]:8443", "[::1]", 8443);
    check_address("https://[FE80::1]:443", "https://[fe80::1]/api/v1/", "https://[fe80::1]", "[fe80::1]", 0);
}
static void test_server_address_refuses_everything_else(void) {
    const char *bad[] = {
        "", "   ", "https:/", "https:", "https//example.com", "ftp://example.com", "wss://example.com", "http://example.com", "HTTP://example.com",
        "https://", "https:///api/v1", "https://:8443", "https://example.com:", "https://example.com:abc", "https://example.com:-1", "https://example.com:65536",
        "https://example.com:0", "https://example.com:80:90", "https://example.com:8443x", "https://user@example.com", "https://:secret@example.com",
        "https://example.com/?token=x", "https://example.com?x", "https://example.com#frag", "https://example.com/api/v1?x=1", "https://example.com/api/v1#x",
        "https://example.com//", "https://example.com/api", "https://example.com/api/v1/sessions", "https://example.com/api/v1//", "https://example.com/api/v2",
        "https://example.com/api/mobile/v1", "https://example.com/api/mobile/v1/", "https://example.com/index.html", "https://exa mple.com", "https://example.com\\api",
        "https://ex\xC3\xA4mple.com", "https://[::1", "https://[::1]x", "https://[::1]:", "https://[::1]:99999", "https://[::1]:0", " http://example.com ",
    };
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
        ServerAddress a;
        bool ok = server_address_parse(bad[i], &a);
        if (ok) { printf("  accepted %s\n", bad[i]); server_address_free(&a); }
        CHECK(!ok);
        CHECK(a.base_url == NULL && a.origin == NULL && a.host == NULL);
    }
    ServerAddress a;
    CHECK(!server_address_parse(NULL, &a));
}
static void test_server_address_copy_is_independent(void) {
    ServerAddress a; CHECK(server_address_parse("https://Example.com:8443/api/v1", &a));
    ServerAddress b; server_address_copy(&b, &a);
    CHECK(b.base_url != a.base_url && b.origin != a.origin && b.host != a.host);
    server_address_free(&a);
    CHECK_STR(b.base_url, "https://example.com:8443/api/v1/"); CHECK_STR(b.origin, "https://example.com:8443"); CHECK_STR(b.host, "example.com"); CHECK_INT(b.port, 8443);
    server_address_free(&b);
    server_address_free(NULL);
}

// MARK: - Tokens

static void test_token_shape(void) {
    CHECK(api_token_valid(TOKEN));
    CHECK(api_token_valid("brm_ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopq"));
    CHECK(api_token_valid("brm_0123456789-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-"));
    char dashes[48] = "brm_", unders[48] = "brm_";
    memset(dashes + 4, '-', 43); dashes[47] = 0; memset(unders + 4, '_', 43); unders[47] = 0;
    CHECK(api_token_valid(dashes)); CHECK(api_token_valid(unders));
    char *body43 = xstrfmt("brm_%s", "a1b2c3d4e5f6g7h8i9j0k1l2m3n4o5p6q7r8s9t0u1v");
    CHECK_INT(strlen(body43), 47); CHECK(api_token_valid(body43));
    const char *bad[] = { NULL, "", "brm_", "brm_a", "Bearer brm_aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa" };
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) CHECK(!api_token_valid(bad[i]));
    // One character off a valid token, at the same length.
    char t[48];
    const char *prefixes[] = { "BRM_", "brn_", "brm-", "Brm_", " brm" };
    for (size_t i = 0; i < sizeof prefixes / sizeof *prefixes; i++) { strcpy(t, TOKEN); memcpy(t, prefixes[i], 4); CHECK_INT(strlen(t), 47); CHECK(!api_token_valid(t)); }
    const char outsiders[] = ".+/= \t\n:%~\x7F\xC3";
    for (size_t i = 0; i < sizeof outsiders - 1; i++) {
        strcpy(t, TOKEN); t[46] = outsiders[i];
        if (api_token_valid(t)) printf("  accepted byte 0x%02X at the end\n", (unsigned char)outsiders[i]);
        CHECK(!api_token_valid(t));
        strcpy(t, TOKEN); t[20] = outsiders[i]; CHECK(!api_token_valid(t));
    }
    // One too few or too many.
    strcpy(t, TOKEN); t[46] = 0; CHECK(!api_token_valid(t));
    char *longer = xstrfmt("%s-", TOKEN); CHECK(!api_token_valid(longer)); free(longer);
    free(body43);
}
static void test_client_refuses_a_bad_token_without_an_error_to_fill(void) {
    ServerAddress a; CHECK(server_address_parse("https://example.com", &a));
    CHECK(api_client_new(&a, NULL, NULL) == NULL);
    ApiError e; api_error_init(&e);
    CHECK(api_client_new(&a, "brm_", &e) == NULL); CHECK(e.kind == API_INVALID_TOKEN); CHECK(e.message == NULL); CHECK(e.retry_after < 0);
    api_error_clear(&e); server_address_free(&a);
}
static void test_client_keeps_its_own_copy_of_the_address_and_counts_references(void) {
    ServerAddress a; CHECK(server_address_parse("https://Example.com:8443", &a));
    ApiError e; api_error_init(&e);
    ApiClient *c = api_client_new(&a, TOKEN, &e);
    CHECK(c != NULL);
    const ServerAddress *own = api_client_address(c);
    CHECK(own != &a && own->base_url != a.base_url);
    server_address_free(&a);
    CHECK_STR(own->base_url, "https://example.com:8443/api/v1/"); CHECK_INT(own->port, 8443);
    CHECK(api_client_retain(c) == c);
    CHECK(api_client_retain(NULL) == NULL);
    api_client_release(c);
    // Still alive after one of two releases.
    Stub stub = { 0 }; stub_json(&stub, 200, "{}");
    api_client_set_transport(c, stub_transport, &stub);
    Json *r = api_call(c, "projects", NULL, 0, &e); CHECK(r != NULL); json_free(r);
    CHECK_STR(stub.last_url, "https://example.com:8443/api/v1/projects");
    api_client_release(c);
    api_client_release(NULL);
    api_error_clear(&e); stub_reset(&stub);
}

// MARK: - Errors

static void test_error_set_clear_and_copy(void) {
    ApiError e; api_error_init(&e);
    CHECK(e.kind == API_OK && e.status == 0 && e.message == NULL && e.retry_after == -1);
    api_error_set(&e, API_HTTP, 503, "Down", 12.5);
    CHECK(e.kind == API_HTTP && e.status == 503 && e.retry_after == 12.5); CHECK_STR(e.message, "Down");
    ApiError copy; api_error_init(&copy);
    api_error_copy(&copy, &e);
    CHECK(copy.message != e.message); CHECK_STR(copy.message, "Down"); CHECK(copy.kind == API_HTTP && copy.status == 503 && copy.retry_after == 12.5);
    // Setting again replaces rather than leaks; a NULL message stays NULL (#46).
    api_error_set(&e, API_NETWORK, 0, NULL, -1);
    CHECK(e.kind == API_NETWORK && e.message == NULL && e.status == 0);
    api_error_clear(&e);
    CHECK(e.kind == API_OK && e.message == NULL && e.retry_after == -1);
    CHECK_STR(copy.message, "Down");
    api_error_clear(&copy);
    api_error_clear(NULL);
    api_error_set(NULL, API_HTTP, 400, "ignored", -1);
}
static char *describe(ApiErrorKind kind, int status, const char *message) {
    ApiError e; api_error_init(&e);
    api_error_set(&e, kind, status, message, -1);
    char *text = api_error_description(&e);
    api_error_clear(&e);
    return text;
}
static void test_error_descriptions_for_every_kind(void) {
    CHECK_OWNED_STR(describe(API_OK, 0, NULL), "");
    CHECK_OWNED_STR(describe(API_INVALID_ADDRESS, 0, NULL), "Enter an HTTPS server address, optionally ending in /api/v1, without credentials or query parameters.");
    CHECK_OWNED_STR(describe(API_INVALID_TOKEN, 0, NULL), "Paste the complete token from Settings \xE2\x86\x92 Devices and clients.");
    CHECK_OWNED_STR(describe(API_REDIRECTED, 302, NULL), "The server redirected this request. Check the Cloudflare Access exception for /api/v1 and /api/v1/*.");
    CHECK_OWNED_STR(describe(API_NON_JSON, 200, NULL), "The server returned an unexpected response. Check that the client API is deployed and reachable through Cloudflare Access.");
    CHECK_OWNED_STR(describe(API_INCOMPATIBLE_VERSION, 0, NULL), "This server uses an unsupported client API version.");
    CHECK_OWNED_STR(describe(API_OVERSIZED_REQUEST, 0, NULL), "This message exceeds the server\xE2\x80\x99s 1 MiB request limit. Shorten it before sending.");
    CHECK_OWNED_STR(describe(API_NETWORK, 0, "The request timed out."), "The request timed out.");
    CHECK_OWNED_STR(describe(API_NETWORK, 0, NULL), "The server could not be reached.");
    CHECK_OWNED_STR(describe(API_CANCELLED, 0, NULL), "Cancelled.");
    // The server's words are not shown for an expired token, and the old mobile API wording is gone (#26).
    CHECK_OWNED_STR(describe(API_HTTP, 401, "Token revoked"), "This token has expired or was revoked. Reconnect with a new token.");
    CHECK_OWNED_STR(describe(API_HTTP, 403, "Admin only"), "Access denied: Admin only");
    CHECK_OWNED_STR(describe(API_HTTP, 403, NULL), "Access denied: ");
    CHECK_OWNED_STR(describe(API_HTTP, 429, "Slow down"), "The server is rate limiting requests. Updates will resume after a delay.");
    CHECK_OWNED_STR(describe(API_HTTP, 404, "Session not found"), "Session not found (HTTP 404)");
    CHECK_OWNED_STR(describe(API_HTTP, 500, NULL), "Request failed (HTTP 500)");
    CHECK_OWNED_STR(describe((ApiErrorKind)99, 0, NULL), "Unknown error");
    for (int k = API_OK; k <= API_CANCELLED; k++) {
        char *text = describe((ApiErrorKind)k, 0, NULL);
        CHECK(strstr(text, "mobile") == NULL);
        free(text);
    }
}
static void test_unauthorized_is_only_a_401_and_refusals_are_only_4xx(void) {
    ApiError e; api_error_init(&e);
    CHECK(!api_error_unauthorized(NULL)); CHECK(!api_error_is_refusal(NULL));
    CHECK(!api_error_unauthorized(&e)); CHECK(!api_error_is_refusal(&e));
    struct { ApiErrorKind kind; int status; bool unauthorized, refusal; } cases[] = {
        { API_HTTP, 401, true, true }, { API_HTTP, 403, false, true }, { API_HTTP, 400, false, true }, { API_HTTP, 404, false, true },
        { API_HTTP, 409, false, true }, { API_HTTP, 413, false, true }, { API_HTTP, 422, false, true }, { API_HTTP, 429, false, true },
        { API_HTTP, 499, false, true }, { API_HTTP, 500, false, false }, { API_HTTP, 502, false, false }, { API_HTTP, 503, false, false },
        { API_HTTP, 399, false, false }, { API_HTTP, 100, false, false },
        { API_NETWORK, 401, false, false }, { API_NETWORK, 0, false, false }, { API_REDIRECTED, 302, false, false }, { API_REDIRECTED, 401, false, false },
        { API_NON_JSON, 200, false, false }, { API_CANCELLED, 0, false, false }, { API_OVERSIZED_REQUEST, 413, false, false },
        { API_INVALID_TOKEN, 401, false, false },
    };
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        api_error_set(&e, cases[i].kind, cases[i].status, NULL, -1);
        CHECK(api_error_unauthorized(&e) == cases[i].unauthorized);
        CHECK(api_error_is_refusal(&e) == cases[i].refusal);
    }
    api_error_clear(&e);
}

// MARK: - Route table

typedef struct { const char *name, *method, *path, *set, *filter, *list; } Expected;
static const Expected ROUTE_TABLE[] = {
    { "projects", "GET", "projects" }, { "branches", "GET", "branches" }, { "runtimes", "GET", "runtimes" }, { "usage", "GET", "usage" },
    { "usage_all", "GET", "usage/all" }, { "actions", "GET", "actions" }, { "action", "POST", "actions" },
    { "pulls", "GET", "pulls" }, { "pull", "GET", "pulls/{pr}" }, { "pull_description", "GET", "pulls/{pr}/description" },
    { "pull_files", "GET", "pulls/{pr}/files" }, { "pull_comments", "GET", "pulls/{pr}/comments" }, { "pull_reviews", "GET", "pulls/{pr}/reviews" },
    { "pull_review_comments", "GET", "pulls/{pr}/review-comments" }, { "findings", "GET", "pulls/{pr}/findings" },
    { "finding_decision", "POST", "pulls/{pr}/findings/decision" }, { "merge_pull", "POST", "pulls/{pr}/merge" },
    { "update_pull", "PATCH", "pulls/{pr}" }, { "update_pull_branch", "POST", "pulls/{pr}/update-branch" },
    { "serve_pull", "POST", "pulls/{prNumber}/serve" }, { "serve_branch", "POST", "branches/serve" },
    { "issue", "GET", "issues/{issue}" }, { "issue_timeline", "GET", "issues/{issue}/timeline" }, { "close_issue", "POST", "issues/{issue}/close" },
    { "update_issue", "PATCH", "issues/{issue}" },
    { "project_board", "GET", "project-board" }, { "project_board_move", "POST", "project-board/move" },
    { "repo_tree", "GET", "repo/tree" }, { "repo_file", "GET", "repo/file" }, { "commit", "GET", "commits/{sha}" },
    { "sessions", "GET", "sessions", NULL, "repo", "sessions" }, { "start_session", "POST", "sessions" },
    { "review", "POST", "sessions", "review" }, { "qa", "POST", "sessions", "qa" },
    { "session", "GET", "sessions/{sessionId}" }, { "rename", "PATCH", "sessions/{sessionId}" }, { "delete", "DELETE", "sessions/{sessionId}" },
    { "message", "POST", "sessions/{sessionId}/messages" }, { "drop_message", "DELETE", "sessions/{sessionId}/queue/{index}" },
    { "cancel", "POST", "sessions/{sessionId}/cancel" }, { "close", "POST", "sessions/{sessionId}/close" }, { "reopen", "POST", "sessions/{sessionId}/reopen" },
    { "serve", "POST", "sessions/{sessionId}/serve" }, { "compact", "POST", "sessions/{sessionId}/compact" }, { "clear", "POST", "sessions/{sessionId}/clear" },
    { "review_loop", "POST", "sessions/{sessionId}/review-loop" }, { "qa_loop", "POST", "sessions/{sessionId}/qa-loop" },
    { "link_pr", "POST", "sessions/{sessionId}/link-pr" }, { "complete_findings", "POST", "sessions/{sessionId}/findings/triage" },
    { "save_findings", "POST", "sessions/{sessionId}/findings/save" }, { "reply_finding", "POST", "sessions/{sessionId}/findings/reply" },
    { "delete_finding", "POST", "sessions/{sessionId}/findings/delete" },
    { "browser", "GET", "sessions/{sessionId}/browser" }, { "browser_on", "POST", "sessions/{sessionId}/browser" },
    { "browser_off", "DELETE", "sessions/{sessionId}/browser" }, { "browser_input", "POST", "sessions/{sessionId}/browser/input" },
    { "browser_stream", "GET", "sessions/{sessionId}/browser/stream" },
    { "upload", "POST", "uploads" }, { "transcribe", "POST", "transcribe" },
    { "settings_projects", "GET", "settings/projects" }, { "create_project", "POST", "settings/projects" },
    { "update_project", "PUT", "settings/projects/{id}" }, { "delete_project", "DELETE", "settings/projects/{id}" },
    { "order_projects", "PUT", "settings/projects/order" },
    { "settings_providers", "GET", "settings/providers" }, { "create_provider", "POST", "settings/providers" },
    { "update_provider", "PUT", "settings/providers/{id}" }, { "delete_provider", "DELETE", "settings/providers/{id}" },
    { "test_provider", "POST", "settings/providers/test" }, { "provider_status", "GET", "settings/providers/{id}/status" },
    { "provider_login", "POST", "settings/providers/{id}/login" }, { "provider_login_start", "POST", "settings/providers/{id}/login/start" },
    { "provider_login_finish", "POST", "settings/providers/{id}/login/finish" },
    { "settings_db_servers", "GET", "settings/db-servers" }, { "create_db_server", "POST", "settings/db-servers" },
    { "update_db_server", "PUT", "settings/db-servers/{id}" }, { "delete_db_server", "DELETE", "settings/db-servers/{id}" },
    { "test_db_server", "POST", "settings/db-servers/test" },
    { "settings_ssh_servers", "GET", "settings/ssh/servers" }, { "create_ssh_server", "POST", "settings/ssh/servers" },
    { "update_ssh_server", "PUT", "settings/ssh/servers/{id}" }, { "delete_ssh_server", "DELETE", "settings/ssh/servers/{id}" },
    { "ssh_db_credentials", "GET", "settings/ssh/servers/{id}/db-credentials" },
    { "settings_forge_accounts", "GET", "settings/forge/accounts" }, { "create_forge_account", "POST", "settings/forge/accounts" },
    { "update_forge_account", "PUT", "settings/forge/accounts/{id}" }, { "delete_forge_account", "DELETE", "settings/forge/accounts/{id}" },
    { "mail_messages", "GET", "mail/messages" },
    { "mail_message", "GET", "mail/accounts/{account}/messages/{id}" },
    { "delete_mail_message", "DELETE", "mail/accounts/{account}/messages/{id}" },
    { "settings_mail_accounts", "GET", "settings/mail/accounts" },
    { "connect_mail_account", "POST", "settings/mail/accounts/connect" },
    { "finish_mail_account", "POST", "settings/mail/accounts/connect/finish" },
    { "update_mail_account", "PUT", "settings/mail/accounts/{id}" },
    { "delete_mail_account", "DELETE", "settings/mail/accounts/{id}" },
    { "sync_mail_account", "POST", "settings/mail/accounts/{id}/sync" },
    { "settings_mcp_servers", "GET", "settings/mcp/servers" },
    { "create_mcp_server", "POST", "settings/mcp/servers" },
    { "update_mcp_server", "PUT", "settings/mcp/servers/{id}" },
    { "delete_mcp_server", "DELETE", "settings/mcp/servers/{id}" },
    { "connect_mcp_server", "POST", "settings/mcp/servers/{id}/connect" },
    { "finish_mcp_sign_in", "POST", "settings/mcp/servers/{id}/finish-sign-in" },

};
#define ROUTE_COUNT (sizeof ROUTE_TABLE / sizeof *ROUTE_TABLE)

static void test_every_route_has_its_method_path_and_flags(void) {
    for (size_t i = 0; i < ROUTE_COUNT; i++) {
        const Expected *x = &ROUTE_TABLE[i];
        const ApiRoute *r = api_route(x->name);
        if (!r) { printf("  no route for %s\n", x->name); CHECK(r != NULL); continue; }
        CHECK_STR(r->name, x->name); CHECK_STR(r->method, x->method); CHECK_STR(r->path, x->path);
        CHECK_STR(r->set, x->set); CHECK_STR(r->filter, x->filter); CHECK_STR(r->list, x->list);
        // Paths are relative to the base, and only the five verbs the server routes are used.
        CHECK(r->path[0] != '/' && !strchr(r->path, '?'));
        CHECK(str_eq(r->method, "GET") || str_eq(r->method, "POST") || str_eq(r->method, "PUT") || str_eq(r->method, "PATCH") || str_eq(r->method, "DELETE"));
        for (size_t j = 0; j < i; j++) CHECK(!str_eq(ROUTE_TABLE[j].name, x->name));
    }
}
static void test_route_lookup_is_exact(void) {
    const char *missing[] = { NULL, "", "Projects", "projects ", " projects", "project", "operations", "operations/projects", "revoke", "token",
                              "api_revoke", "devices", "settings_devices", "../projects", "sessions/abc", "SESSION" };
    for (size_t i = 0; i < sizeof missing / sizeof *missing; i++) CHECK(api_route(missing[i]) == NULL);
}

static void fill_path(Str *url, const char *path, const char *encoded) {
    for (const char *p = path; *p;) {
        if (*p != '{') { str_appendc(url, *p++); continue; }
        str_appendz(url, encoded);
        p = strchr(p, '}') + 1;
    }
}
static void test_every_route_builds_its_request(void) {
    Stub stub = { 0 }; stub_json(&stub, 200, "{\"ok\":true}");
    ApiClient *c = client(&stub);
    ApiError e; api_error_init(&e);
    char *bearer = xstrfmt("Bearer %s", TOKEN);
    for (size_t i = 0; i < ROUTE_COUNT; i++) {
        const Expected *x = &ROUTE_TABLE[i];
        Json *args = json_object();
        int params = 0;
        for (const char *p = strchr(x->path, '{'); p; p = strchr(p + 1, '{')) {
            char *name = xstrndup(p + 1, (size_t)(strchr(p, '}') - p - 1));
            json_set_str(args, name, "a b/\xC3\xA9?#");
            free(name); params++;
        }
        json_set_str(args, "extra", "v&1");
        int calls = stub.calls;
        Json *result = api_call(c, x->name, args, 0, &e);
        if (!result) printf("  %s failed\n", x->name);
        CHECK(result != NULL); CHECK_INT(stub.calls, calls + 1);
        json_free(result);
        Str url; str_init(&url); str_appendz(&url, BASE);
        fill_path(&url, x->path, "a%20b%2F%C3%A9%3F%23");
        bool reads = str_eq(x->method, "GET") || str_eq(x->method, "DELETE");
        if (reads) str_appendz(&url, "?extra=v%261");
        CHECK_STR(stub.last_method, x->method);
        CHECK_STR(stub.last_url, url.data);
        CHECK_STR(stub.last_authorization, bearer); CHECK_STR(stub.last_accept, "application/json");
        CHECK_INT(stub.last_timeout, 30000);
        if (reads) {
            CHECK(stub.last_body == NULL && stub.last_body_len == 0); CHECK(stub.last_content_type == NULL); CHECK_INT(stub.last_header_count, 2);
        } else {
            CHECK_STR(stub.last_content_type, "application/json"); CHECK_INT(stub.last_header_count, 3);
            Json *sent = json_parsez(stub.last_body);
            CHECK(json_is_object(sent));
            CHECK_STR(json_str(json_get(sent, "extra")), "v&1");
            // Path arguments are not repeated in the body.
            CHECK_INT(json_count(sent), 1 + (x->set ? 1 : 0));
            if (x->set) CHECK(json_bool_is(json_get(sent, x->set), true));
            json_free(sent);
        }
        str_free(&url); json_free(args);
        (void)params;
    }
    free(bearer); api_error_clear(&e); api_client_release(c); stub_reset(&stub);
}

// MARK: - Arguments

static Json *call(ApiClient *c, const char *name, const char *arguments, ApiError *e) {
    Json *args = arguments ? json_parsez(arguments) : NULL;
    Json *result = api_call(c, name, args, 0, e);
    json_free(args);
    return result;
}
static void check_url(ApiClient *c, Stub *stub, const char *name, const char *arguments, const char *url) {
    ApiError e; api_error_init(&e);
    Json *r = call(c, name, arguments, &e);
    if (!r) printf("  %s %s failed\n", name, arguments ? arguments : "");
    CHECK(r != NULL); json_free(r);
    CHECK_STR(stub->last_url, url);
    api_error_clear(&e);
}
static void test_path_arguments_are_encoded_and_numbers_written_whole(void) {
    Stub stub = { 0 }; stub_json(&stub, 200, "{}");
    ApiClient *c = client(&stub);
    check_url(c, &stub, "session", "{\"sessionId\":\"a b\"}", BASE "sessions/a%20b");
    check_url(c, &stub, "session", "{\"sessionId\":\"../../settings\"}", BASE "sessions/..%2F..%2Fsettings");
    check_url(c, &stub, "session", "{\"sessionId\":\"a\\\\b\"}", BASE "sessions/a%5Cb");
    check_url(c, &stub, "session", "{\"sessionId\":\"\xE6\x97\xA5\xE6\x9C\xAC\"}", BASE "sessions/%E6%97%A5%E6%9C%AC");
    check_url(c, &stub, "session", "{\"sessionId\":\"%2F\"}", BASE "sessions/%252F");
    check_url(c, &stub, "session", "{\"sessionId\":\"a+b&c=d;e\"}", BASE "sessions/a%2Bb%26c%3Dd%3Be");
    check_url(c, &stub, "session", "{\"sessionId\":\"Az09-_.~\"}", BASE "sessions/Az09-_.~");
    check_url(c, &stub, "session", "{\"sessionId\":\".\"}", BASE "sessions/.");
    check_url(c, &stub, "pull", "{\"pr\":12}", BASE "pulls/12");
    check_url(c, &stub, "pull", "{\"pr\":12.0}", BASE "pulls/12");
    check_url(c, &stub, "pull", "{\"pr\":-3}", BASE "pulls/-3");
    check_url(c, &stub, "pull", "{\"pr\":0}", BASE "pulls/0");
    check_url(c, &stub, "pull", "{\"pr\":\"12\"}", BASE "pulls/12");
    check_url(c, &stub, "drop_message", "{\"sessionId\":\"s\",\"index\":0}", BASE "sessions/s/queue/0");
    // A millisecond id is a double; it must not turn into an exponent (#39).
    check_url(c, &stub, "delete_ssh_server", "{\"id\":1727000000000}", BASE "settings/ssh/servers/1727000000000");
    check_url(c, &stub, "ssh_db_credentials", "{\"id\":1727000000000}", BASE "settings/ssh/servers/1727000000000/db-credentials");
    check_url(c, &stub, "update_forge_account", "{\"id\":1727000000001}", BASE "settings/forge/accounts/1727000000001");
    check_url(c, &stub, "update_provider", "{\"id\":999999999999999}", BASE "settings/providers/999999999999999");
    api_client_release(c); stub_reset(&stub);
}
static void test_a_missing_or_unusable_path_argument_is_refused_before_the_network(void) {
    Stub stub = { 0 }; stub_json(&stub, 200, "{}");
    ApiClient *c = client(&stub);
    ApiError e; api_error_init(&e);
    const char *bad[] = { NULL, "{}", "[]", "\"abc\"", "{\"sessionId\":\"\"}", "{\"sessionId\":null}", "{\"sessionId\":true}", "{\"sessionId\":1.5}",
                          "{\"sessionId\":[\"a\"]}", "{\"sessionId\":{\"id\":\"a\"}}", "{\"sessionId\":1e15}", "{\"SessionId\":\"a\"}", "{\"session_id\":\"a\"}" };
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
        api_error_init(&e);
        CHECK(call(c, "message", bad[i], &e) == NULL);
        CHECK(e.kind == API_HTTP && e.status == 400); CHECK_STR(e.message, "Missing argument: sessionId");
        CHECK(api_error_is_refusal(&e));
        api_error_clear(&e);
    }
    // The first missing one is named, even when another is present.
    CHECK(call(c, "drop_message", "{\"index\":1}", &e) == NULL); CHECK_STR(e.message, "Missing argument: sessionId");
    CHECK(call(c, "drop_message", "{\"sessionId\":\"s\"}", &e) == NULL); CHECK_STR(e.message, "Missing argument: index");
    CHECK(call(c, "serve_pull", "{\"pr\":12}", &e) == NULL); CHECK_STR(e.message, "Missing argument: prNumber");
    CHECK(call(c, "update_project", "{\"projectId\":7}", &e) == NULL); CHECK_STR(e.message, "Missing argument: id");
    const char *unknown[] = { NULL, "", "operations", "revoke", "Projects", "sessions/abc", "../projects" };
    for (size_t i = 0; i < sizeof unknown / sizeof *unknown; i++) {
        CHECK(call(c, unknown[i], "{}", &e) == NULL);
        CHECK(e.kind == API_HTTP && e.status == 400); CHECK_STR(e.message, "Unknown call");
    }
    CHECK_INT(stub.calls, 0);
    api_error_clear(&e); api_client_release(c); stub_reset(&stub);
}
static void test_query_arguments_of_reads(void) {
    Stub stub = { 0 }; stub_json(&stub, 200, "{}");
    ApiClient *c = client(&stub);
    check_url(c, &stub, "projects", NULL, BASE "projects");
    check_url(c, &stub, "projects", "{}", BASE "projects");
    check_url(c, &stub, "projects", "[1,2]", BASE "projects");
    check_url(c, &stub, "pulls", "{\"repo\":\"o/r\",\"state\":\"open\"}", BASE "pulls?repo=o%2Fr&state=open");
    check_url(c, &stub, "pulls", "{\"q\":\"a b&c=d?e#f%\"}", BASE "pulls?q=a%20b%26c%3Dd%3Fe%23f%25");
    check_url(c, &stub, "pulls", "{\"q\":\"\xC3\xB1\"}", BASE "pulls?q=%C3%B1");
    check_url(c, &stub, "pulls", "{\"a key\":\"v\"}", BASE "pulls?a%20key=v");
    check_url(c, &stub, "pulls", "{\"empty\":\"\"}", BASE "pulls?empty=");
    check_url(c, &stub, "pulls", "{\"on\":true,\"off\":false}", BASE "pulls?on=1&off=0");
    check_url(c, &stub, "pulls", "{\"page\":2,\"neg\":-1,\"half\":0.5,\"big\":1e15}", BASE "pulls?page=2&neg=-1&half=0.5&big=1000000000000000");
    // What has no place in a query is left out.
    check_url(c, &stub, "pulls", "{\"gone\":null,\"obj\":{\"a\":1},\"page\":3}", BASE "pulls?page=3");
    check_url(c, &stub, "pulls", "{\"gone\":null}", BASE "pulls");
    // An array repeats its parameter, skipping what has no place (#35).
    check_url(c, &stub, "usage_all", "{\"project\":[\"a\",\"b\"]}", BASE "usage/all?project=a&project=b");
    check_url(c, &stub, "usage_all", "{\"project\":[\"a\",null,{},7,true]}", BASE "usage/all?project=a&project=7&project=1");
    check_url(c, &stub, "usage_all", "{\"project\":[],\"period\":\"7d\"}", BASE "usage/all?period=7d");
    check_url(c, &stub, "usage_all", "{\"period\":\"30d\",\"project\":[\"p:1\"],\"model\":[\"m/1\",\"m 2\"]}", BASE "usage/all?period=30d&project=p%3A1&model=m%2F1&model=m%202");
    // A DELETE carries its extra arguments in the query too, and never a body (#30).
    check_url(c, &stub, "delete", "{\"sessionId\":\"s-70b\"}", BASE "sessions/s-70b");
    CHECK_STR(stub.last_method, "DELETE"); CHECK(stub.last_body == NULL); CHECK(stub.last_content_type == NULL);
    check_url(c, &stub, "delete", "{\"sessionId\":\"s-70b\",\"force\":true}", BASE "sessions/s-70b?force=1");
    check_url(c, &stub, "drop_message", "{\"sessionId\":\"s\",\"index\":2,\"id\":\"q\"}", BASE "sessions/s/queue/2?id=q");
    api_client_release(c); stub_reset(&stub);
}
static void test_json_bodies_of_writes(void) {
    Stub stub = { 0 }; stub_json(&stub, 200, "{}");
    ApiClient *c = client(&stub);
    ApiError e; api_error_init(&e);
    Json *r;
    // No arguments, or arguments that are not an object, send an empty object.
    r = call(c, "start_session", NULL, &e); json_free(r); CHECK_STR(stub.last_body, "{}");
    r = call(c, "start_session", "[1,2]", &e); json_free(r); CHECK_STR(stub.last_body, "{}");
    r = call(c, "start_session", "\"x\"", &e); json_free(r); CHECK_STR(stub.last_body, "{}");
    // Values keep their JSON types in a body, nulls and nested values included.
    r = call(c, "create_project", "{\"repo\":\"o/r\",\"n\":1.5,\"on\":false,\"none\":null,\"list\":[1,\"a\"],\"obj\":{\"k\":\"v\"}}", &e); json_free(r);
    CHECK_STR(stub.last_url, BASE "settings/projects"); CHECK_STR(stub.last_method, "POST");
    CHECK_STR(stub.last_body, "{\"repo\":\"o/r\",\"n\":1.5,\"on\":false,\"none\":null,\"list\":[1,\"a\"],\"obj\":{\"k\":\"v\"}}");
    // The path argument is taken out; everything else stays.
    r = call(c, "rename", "{\"sessionId\":\"abc\",\"autoCompact\":true}", &e); json_free(r);
    CHECK_STR(stub.last_url, BASE "sessions/abc"); CHECK_STR(stub.last_method, "PATCH"); CHECK_STR(stub.last_body, "{\"autoCompact\":true}");
    // An edit sends its lists whole, an empty one included, since each replaces what GitHub has.
    r = call(c, "update_pull", "{\"pr\":7,\"repo\":\"o/r\",\"labels\":[\"bug\",\"good first issue\"],\"assignees\":[]}", &e); json_free(r);
    CHECK_STR(stub.last_url, BASE "pulls/7"); CHECK_STR(stub.last_method, "PATCH");
    CHECK_STR(stub.last_body, "{\"repo\":\"o/r\",\"labels\":[\"bug\",\"good first issue\"],\"assignees\":[]}");
    r = call(c, "update_issue", "{\"issue\":9,\"repo\":\"o/r\",\"title\":\"T\",\"body\":\"\"}", &e); json_free(r);
    CHECK_STR(stub.last_url, BASE "issues/9"); CHECK_STR(stub.last_method, "PATCH"); CHECK_STR(stub.last_body, "{\"repo\":\"o/r\",\"title\":\"T\",\"body\":\"\"}");
    r = call(c, "update_pull_branch", "{\"pr\":7,\"repo\":\"o/r\",\"headSha\":\"abc\",\"baseRef\":\"main\"}", &e); json_free(r);
    CHECK_STR(stub.last_url, BASE "pulls/7/update-branch"); CHECK_STR(stub.last_method, "POST");
    CHECK_STR(stub.last_body, "{\"repo\":\"o/r\",\"headSha\":\"abc\",\"baseRef\":\"main\"}");
    r = call(c, "compact", "{\"sessionId\":\"abc\"}", &e); json_free(r);
    CHECK_STR(stub.last_url, BASE "sessions/abc/compact"); CHECK_STR(stub.last_body, "{}");
    r = call(c, "clear", "{\"sessionId\":\"abc\"}", &e); json_free(r);
    CHECK_STR(stub.last_url, BASE "sessions/abc/clear"); CHECK_STR(stub.last_method, "POST");
    // A start names its provider as `provider` and goes in the body; nothing goes in the query of a POST (#26).
    r = call(c, "start_session", "{\"repo\":\"o/r\",\"provider\":2,\"prompt\":\"a&b\"}", &e); json_free(r);
    CHECK_STR(stub.last_url, BASE "sessions"); CHECK_STR(stub.last_body, "{\"repo\":\"o/r\",\"provider\":2,\"prompt\":\"a&b\"}");
    // A path argument in the body of a write with the same name is taken once only.
    r = call(c, "serve_pull", "{\"prNumber\":12,\"repo\":\"o/r\"}", &e); json_free(r);
    CHECK_STR(stub.last_url, BASE "pulls/12/serve"); CHECK_STR(stub.last_body, "{\"repo\":\"o/r\"}");
    r = call(c, "message", "{\"sessionId\":\"abc\",\"text\":\"Hi\",\"attachments\":[\"1a2b\"]}", &e); json_free(r);
    CHECK_STR(stub.last_body, "{\"text\":\"Hi\",\"attachments\":[\"1a2b\"]}");
    r = call(c, "finding_decision", "{\"pr\":4,\"key\":\"k\",\"decision\":\"accept\"}", &e); json_free(r);
    CHECK_STR(stub.last_url, BASE "pulls/4/findings/decision"); CHECK_STR(stub.last_body, "{\"key\":\"k\",\"decision\":\"accept\"}");
    // Unicode text goes through as it is.
    r = call(c, "message", "{\"sessionId\":\"abc\",\"text\":\"\xC2\xA1Hola \xF0\x9F\x91\x8B\"}", &e); json_free(r);
    Json *sent = json_parsez(stub.last_body); CHECK_STR(json_str(json_get(sent, "text")), "\xC2\xA1Hola \xF0\x9F\x91\x8B"); json_free(sent);
    api_error_clear(&e); api_client_release(c); stub_reset(&stub);
}
static void test_a_run_profile_switch_names_the_session_in_the_path(void) {
    Stub stub = { 0 }; stub_json(&stub, 200, "{\"url\":\"https://8123.preview.example.com\",\"profile\":\"tenant-a\"}");
    ApiClient *c = client(&stub);
    ApiError e; api_error_init(&e);
    // The Run tab's profile dropdown: the session goes in the path, the profile in the body.
    Json *r = call(c, "serve", "{\"sessionId\":\"run-1\",\"profile\":\"tenant-a\"}", &e);
    CHECK_STR(stub.last_url, BASE "sessions/run-1/serve"); CHECK_STR(stub.last_method, "POST"); CHECK_STR(stub.last_body, "{\"profile\":\"tenant-a\"}");
    CHECK_STR(json_str(json_get(r, "profile")), "tenant-a"); json_free(r);
    // Without a profile the server serves the one it served last.
    r = call(c, "serve", "{\"sessionId\":\"run-1\"}", &e); json_free(r);
    CHECK_STR(stub.last_url, BASE "sessions/run-1/serve"); CHECK_STR(stub.last_body, "{}");
    api_error_clear(&e); api_client_release(c); stub_reset(&stub);
}
static void test_the_set_flag_is_always_sent_true(void) {
    Stub stub = { 0 }; stub_json(&stub, 200, "{}");
    ApiClient *c = client(&stub);
    ApiError e; api_error_init(&e);
    Json *r = call(c, "review", "{\"repo\":\"o/r\",\"pr\":4}", &e); json_free(r);
    CHECK_STR(stub.last_url, BASE "sessions"); CHECK_STR(stub.last_body, "{\"repo\":\"o/r\",\"pr\":4,\"review\":true}");
    r = call(c, "review", "{\"review\":false,\"repo\":\"o/r\"}", &e); json_free(r);
    CHECK_STR(stub.last_body, "{\"review\":true,\"repo\":\"o/r\"}");
    r = call(c, "review", NULL, &e); json_free(r);
    CHECK_STR(stub.last_body, "{\"review\":true}");
    r = call(c, "qa", "{\"repo\":\"o/r\"}", &e); json_free(r);
    CHECK_STR(stub.last_body, "{\"repo\":\"o/r\",\"qa\":true}");
    // A plain start does not carry either flag.
    r = call(c, "start_session", "{\"repo\":\"o/r\"}", &e); json_free(r);
    CHECK_STR(stub.last_body, "{\"repo\":\"o/r\"}");
    api_error_clear(&e); api_client_release(c); stub_reset(&stub);
}
static void test_the_session_list_is_cut_to_the_requested_repo_here(void) {
    Stub stub = { 0 };
    stub_json(&stub, 200, "{\"sessions\":[{\"id\":\"a\",\"repo\":\"o/r\"},{\"id\":\"b\",\"repo\":\"o/other\"},{\"id\":\"c\"},{\"id\":\"d\",\"repo\":\"O/R\"},"
                          "{\"id\":\"e\",\"repo\":\"o/r\"},{\"id\":\"f\",\"repo\":7}],\"total\":6}");
    ApiClient *c = client(&stub);
    ApiError e; api_error_init(&e);
    Json *r = call(c, "sessions", "{\"repo\":\"o/r\",\"status\":\"idle\"}", &e);
    CHECK_STR(stub.last_url, BASE "sessions?status=idle");
    const Json *rows = json_get(r, "sessions");
    CHECK_INT(json_count(rows), 2);
    CHECK_STR(json_str(json_get(json_at(rows, 0), "id")), "a"); CHECK_STR(json_str(json_get(json_at(rows, 1), "id")), "e");
    CHECK_INT(json_int_or(json_get(r, "total"), 0), 6);
    json_free(r);
    // Without a repo nothing is cut.
    r = call(c, "sessions", "{}", &e); CHECK_INT(json_count(json_get(r, "sessions")), 6); json_free(r);
    CHECK_STR(stub.last_url, BASE "sessions");
    // A repo nothing matches leaves an empty list, and an answer without a list gains an empty one.
    r = call(c, "sessions", "{\"repo\":\"x/y\"}", &e); CHECK(json_is_array(json_get(r, "sessions"))); CHECK_INT(json_count(json_get(r, "sessions")), 0); json_free(r);
    stub.body = "{\"total\":0}";
    r = call(c, "sessions", "{\"repo\":\"o/r\"}", &e); CHECK(r != NULL); CHECK(json_is_array(json_get(r, "sessions"))); CHECK_INT(json_count(json_get(r, "sessions")), 0); json_free(r);
    // A failed read is not cut and stays an error.
    stub_json(&stub, 500, "{\"error\":\"Boom\"}");
    CHECK(call(c, "sessions", "{\"repo\":\"o/r\"}", &e) == NULL); CHECK(e.kind == API_HTTP && e.status == 500); CHECK_STR(e.message, "Boom");
    // The filter is only for the list route: elsewhere `repo` is an ordinary argument.
    stub_json(&stub, 200, "{\"sessions\":[{\"repo\":\"x\"}]}");
    r = call(c, "pulls", "{\"repo\":\"o/r\"}", &e); CHECK_STR(stub.last_url, BASE "pulls?repo=o%2Fr"); CHECK_INT(json_count(json_get(r, "sessions")), 1); json_free(r);
    api_error_clear(&e); api_client_release(c); stub_reset(&stub);
}
static void test_the_callers_arguments_are_left_as_they_were(void) {
    Stub stub = { 0 }; stub_json(&stub, 200, "{\"sessions\":[]}");
    ApiClient *c = client(&stub);
    ApiError e; api_error_init(&e);
    Json *args = json_parsez("{\"sessionId\":\"abc\",\"text\":\"Hi\"}");
    char *before = json_serialize(args, false);
    Json *r = api_call(c, "message", args, 0, &e); json_free(r);
    r = api_call(c, "review", args, 0, &e); json_free(r);
    r = api_call(c, "sessions", args, 0, &e); json_free(r);
    CHECK_OWNED_STR(json_serialize(args, false), before);
    free(before); json_free(args);
    api_error_clear(&e); api_client_release(c); stub_reset(&stub);
}
static void test_timeouts_reach_the_transport(void) {
    Stub stub = { 0 }; stub_json(&stub, 200, "{\"text\":\"t\",\"file\":{\"id\":\"f\"}}");
    ApiClient *c = client(&stub);
    ApiError e; api_error_init(&e);
    Json *r = api_call(c, "projects", NULL, 0, &e); json_free(r); CHECK_INT(stub.last_timeout, 30000);
    r = api_call(c, "projects", NULL, 600000, &e); json_free(r); CHECK_INT(stub.last_timeout, 600000);
    char *t = api_transcribe(c, "a", 1, NULL, &e); free(t); CHECK_INT(stub.last_timeout, 150000);
    t = api_upload(c, "f", "a", 1, &e); free(t); CHECK_INT(stub.last_timeout, 120000);
    Discovery d; api_discovery(c, &d, &e); CHECK_INT(stub.last_timeout, 30000);
    api_error_clear(&e); api_client_release(c); stub_reset(&stub);
}
static void test_request_size_limit_boundary(void) {
    Stub stub = { 0 }; stub_json(&stub, 200, "{}");
    ApiClient *c = client(&stub);
    ApiError e; api_error_init(&e);
    // {"t":"..."} is the text plus eight bytes.
    size_t limit = 1048576, n = limit - 8;
    char *text = xmalloc(n + 2); memset(text, 'x', n + 1); text[n] = 0;
    Json *args = json_object(); json_set_str(args, "t", text);
    Json *r = api_call(c, "start_session", args, 0, &e);
    CHECK(r != NULL); json_free(r); CHECK_INT(stub.calls, 1); CHECK_INT(stub.last_body_len, limit);
    json_free(args);
    text[n] = 'x'; text[n + 1] = 0;
    args = json_object(); json_set_str(args, "t", text);
    CHECK(api_call(c, "start_session", args, 0, &e) == NULL); CHECK(e.kind == API_OVERSIZED_REQUEST); CHECK_INT(stub.calls, 1);
    CHECK(!api_error_is_refusal(&e));
    json_free(args); free(text);
    // Reads have no body to limit.
    char *q = xmalloc(limit + 1); memset(q, 'q', limit); q[limit] = 0;
    args = json_object(); json_set_str(args, "q", q);
    r = api_call(c, "pulls", args, 0, &e); CHECK(r != NULL); json_free(r); CHECK_INT(stub.calls, 2);
    json_free(args); free(q);
    api_error_clear(&e); api_client_release(c); stub_reset(&stub);
}

// MARK: - Answers

static void test_successful_answers_need_json(void) {
    Stub stub = { 0 };
    ApiClient *c = client(&stub);
    ApiError e; api_error_init(&e);
    Json *r;
    const char *json_types[] = { "application/json", "application/json; charset=utf-8", "Application/JSON", " application/json ; charset=UTF-8", "APPLICATION/JSON;" };
    for (size_t i = 0; i < sizeof json_types / sizeof *json_types; i++) {
        stub_json(&stub, 200, "{\"a\":1}"); stub.content_type = json_types[i];
        r = api_call(c, "projects", NULL, 0, &e);
        if (!r) printf("  refused %s\n", json_types[i]);
        CHECK(r != NULL); CHECK_INT(json_int_or(json_get(r, "a"), 0), 1); json_free(r);
    }
    const char *other_types[] = { NULL, "", "text/html", "text/plain", "application/problem+json", "application/jsonp", "application/json-seq", "text/json", "application" };
    for (size_t i = 0; i < sizeof other_types / sizeof *other_types; i++) {
        stub_json(&stub, 200, "{\"a\":1}"); stub.content_type = other_types[i];
        api_error_init(&e);
        CHECK(api_call(c, "projects", NULL, 0, &e) == NULL); CHECK(e.kind == API_NON_JSON); CHECK_INT(e.status, 200);
        api_error_clear(&e);
    }
    // Every 2xx is a success when its body is JSON.
    stub_json(&stub, 201, "{\"id\":\"x\"}");
    r = api_call(c, "start_session", NULL, 0, &e); CHECK(r != NULL); json_free(r);
    stub_json(&stub, 299, "[]");
    r = api_call(c, "projects", NULL, 0, &e); CHECK(json_is_array(r)); json_free(r);
    // JSON that is not an object still comes back.
    stub_json(&stub, 200, "null");
    r = api_call(c, "projects", NULL, 0, &e); CHECK(r != NULL && json_is_null(r)); json_free(r);
    // An empty or broken body is not an answer, 204 included.
    const char *broken[] = { "", "{", "{\"a\":}", "<html></html>", "{} trailing" };
    for (size_t i = 0; i < sizeof broken / sizeof *broken; i++) {
        stub_json(&stub, 200, broken[i]);
        api_error_init(&e);
        CHECK(api_call(c, "projects", NULL, 0, &e) == NULL); CHECK(e.kind == API_NON_JSON);
        api_error_clear(&e);
    }
    // A JSON error body on a success is still the success.
    stub_json(&stub, 200, "{\"error\":\"ignored\"}");
    r = api_call(c, "projects", NULL, 0, &e); CHECK(r != NULL); CHECK_STR(json_str(json_get(r, "error")), "ignored"); json_free(r);
    api_error_clear(&e); api_client_release(c); stub_reset(&stub);
}
static void test_redirects_of_any_kind_are_refused(void) {
    Stub stub = { 0 };
    ApiClient *c = client(&stub);
    ApiError e; api_error_init(&e);
    int codes[] = { 300, 301, 302, 303, 304, 307, 308, 399 };
    for (size_t i = 0; i < sizeof codes / sizeof *codes; i++) {
        stub_json(&stub, codes[i], "{\"ok\":true}");
        CHECK(api_call(c, "projects", NULL, 0, &e) == NULL); CHECK(e.kind == API_REDIRECTED); CHECK_INT(e.status, codes[i]);
        CHECK(e.message == NULL); CHECK(!api_error_unauthorized(&e)); CHECK(!api_error_is_refusal(&e));
    }
    api_error_clear(&e); api_client_release(c); stub_reset(&stub);
}
static void test_error_answers_keep_the_servers_words_or_the_status(void) {
    Stub stub = { 0 };
    ApiClient *c = client(&stub);
    ApiError e; api_error_init(&e);
    stub_json(&stub, 422, "{\"error\":\"Repository is required\",\"field\":\"repo\"}");
    CHECK(api_call(c, "create_project", NULL, 0, &e) == NULL);
    CHECK(e.kind == API_HTTP && e.status == 422); CHECK_STR(e.message, "Repository is required"); CHECK(e.retry_after < 0);
    CHECK_OWNED_STR(api_error_description(&e), "Repository is required (HTTP 422)");
    struct { int status; const char *text; } fallback[] = {
        { 400, "bad request" }, { 401, "unauthorized" }, { 403, "forbidden" }, { 404, "not found" }, { 405, "method not allowed" },
        { 408, "request timeout" }, { 409, "conflict" }, { 413, "request too large" }, { 422, "unprocessable entity" },
        { 429, "too many requests" }, { 500, "internal server error" }, { 502, "bad gateway" }, { 503, "service unavailable" },
        { 504, "gateway timeout" }, { 418, "request failed" }, { 501, "request failed" }, { 100, "request failed" }, { 600, "request failed" },
    };
    for (size_t i = 0; i < sizeof fallback / sizeof *fallback; i++) {
        // An HTML error page, a JSON body without a string `error`, and a JSON body that does not parse all fall back to the status.
        stub_json(&stub, fallback[i].status, "<html>Bad gateway</html>"); stub.content_type = "text/html";
        CHECK(api_call(c, "projects", NULL, 0, &e) == NULL); CHECK(e.kind == API_HTTP); CHECK_INT(e.status, fallback[i].status); CHECK_STR(e.message, fallback[i].text);
        stub_json(&stub, fallback[i].status, "{\"error\":{\"code\":7},\"message\":\"nope\"}");
        CHECK(api_call(c, "projects", NULL, 0, &e) == NULL); CHECK_STR(e.message, fallback[i].text);
        stub_json(&stub, fallback[i].status, "{\"error\":");
        CHECK(api_call(c, "projects", NULL, 0, &e) == NULL); CHECK_STR(e.message, fallback[i].text);
        stub_json(&stub, fallback[i].status, "{\"error\":\"\"}");
        CHECK(api_call(c, "projects", NULL, 0, &e) == NULL); CHECK_STR(e.message, fallback[i].text);
    }
    // The server's `error` is read only from a JSON answer.
    stub_json(&stub, 400, "{\"error\":\"Hidden\"}"); stub.content_type = "text/plain";
    CHECK(api_call(c, "projects", NULL, 0, &e) == NULL); CHECK_STR(e.message, "bad request");
    stub_json(&stub, 403, "{\"error\":\"Admin token required\"}");
    CHECK(api_call(c, "settings_projects", NULL, 0, &e) == NULL); CHECK(!api_error_unauthorized(&e)); CHECK(api_error_is_refusal(&e));
    CHECK_OWNED_STR(api_error_description(&e), "Access denied: Admin token required");
    stub_json(&stub, 401, "{\"error\":\"Token expired\"}");
    CHECK(api_call(c, "projects", NULL, 0, &e) == NULL); CHECK(api_error_unauthorized(&e)); CHECK_STR(e.message, "Token expired");
    CHECK_OWNED_STR(api_error_description(&e), "This token has expired or was revoked. Reconnect with a new token.");
    api_error_clear(&e); api_client_release(c); stub_reset(&stub);
}
static void test_retry_after_is_read_from_any_error_answer(void) {
    Stub stub = { 0 };
    ApiClient *c = client(&stub);
    ApiError e; api_error_init(&e);
    stub_json(&stub, 429, "{\"error\":\"Too many\"}"); stub.retry_after = "30";
    CHECK(api_call(c, "projects", NULL, 0, &e) == NULL); CHECK(e.status == 429 && e.retry_after == 30);
    CHECK_OWNED_STR(api_error_description(&e), "The server is rate limiting requests. Updates will resume after a delay.");
    stub_json(&stub, 429, "{}"); stub.retry_after = "Fri, 01 Jan 2100 00:00:00 GMT";
    CHECK(api_call(c, "projects", NULL, 0, &e) == NULL); CHECK(e.retry_after > 2000000000.0); CHECK_STR(e.message, "too many requests");
    stub_json(&stub, 429, "{}"); stub.retry_after = "Thu, 01 Jan 1970 00:00:00 GMT";
    CHECK(api_call(c, "projects", NULL, 0, &e) == NULL); CHECK(e.retry_after == 0);
    stub_json(&stub, 429, "{}"); stub.retry_after = "soon";
    CHECK(api_call(c, "projects", NULL, 0, &e) == NULL); CHECK(e.retry_after < 0);
    stub_json(&stub, 503, "{}"); stub.retry_after = " 120 ";
    CHECK(api_call(c, "projects", NULL, 0, &e) == NULL); CHECK(e.status == 503 && e.retry_after == 120); CHECK(!api_error_is_refusal(&e));
    stub_json(&stub, 503, "{}");
    CHECK(api_call(c, "projects", NULL, 0, &e) == NULL); CHECK(e.retry_after < 0);
    // A write that was rate limited is reported once, not retried.
    stub_json(&stub, 429, "{}"); stub.retry_after = "1";
    int calls = stub.calls;
    CHECK(call(c, "message", "{\"sessionId\":\"s\",\"text\":\"x\"}", &e) == NULL); CHECK_INT(stub.calls, calls + 1);
    api_error_clear(&e); api_client_release(c); stub_reset(&stub);
}
static void test_network_failures_say_why(void) {
    Stub stub = { 0 }; stub.fail = true; stub.fail_message = "Could not connect to the server.";
    ApiClient *c = client(&stub);
    ApiError e; api_error_init(&e);
    CHECK(api_call(c, "projects", NULL, 0, &e) == NULL); CHECK(e.kind == API_NETWORK); CHECK_STR(e.message, "Could not connect to the server.");
    CHECK(e.status == 0 && e.retry_after < 0); CHECK(!api_error_is_refusal(&e)); CHECK(!api_error_unauthorized(&e));
    CHECK_OWNED_STR(api_error_description(&e), "Could not connect to the server.");
    stub.fail_message = NULL;
    CHECK(api_call(c, "projects", NULL, 0, &e) == NULL); CHECK(e.kind == API_NETWORK); CHECK_STR(e.message, "The server could not be reached.");
    int calls = stub.calls;
    CHECK(call(c, "merge_pull", "{\"pr\":3}", &e) == NULL); CHECK_INT(stub.calls, calls + 1);
    char *t = api_transcribe(c, "a", 1, NULL, &e); CHECK(t == NULL); CHECK(e.kind == API_NETWORK);
    t = api_upload(c, "a", "a", 1, &e); CHECK(t == NULL); CHECK(e.kind == API_NETWORK);
    Discovery d; CHECK(!api_discovery(c, &d, &e)); CHECK(e.kind == API_NETWORK);
    Route *routes = NULL; size_t n = 0; CHECK(!api_catalog(c, &routes, &n, &e)); CHECK(e.kind == API_NETWORK);
    api_error_clear(&e); api_client_release(c); stub_reset(&stub);
}
static void test_a_success_needs_no_error_to_fill(void) {
    Stub stub = { 0 }; stub_json(&stub, 200, "{}");
    ApiClient *c = client(&stub);
    Json *r = api_call(c, "projects", NULL, 0, NULL); CHECK(r != NULL); json_free(r);
    stub_json(&stub, 500, "{}");
    CHECK(api_call(c, "projects", NULL, 0, NULL) == NULL);
    CHECK(api_call(c, "nope", NULL, 0, NULL) == NULL);
    api_client_release(c); stub_reset(&stub);
}

// MARK: - Discovery and catalog

static void test_discovery_reads_the_client_record_and_refuses_other_versions(void) {
    Stub stub = { 0 };
    ApiClient *c = client(&stub);
    ApiError e; api_error_init(&e);
    Discovery d;
    stub_json(&stub, 200, "{\"version\":1,\"client\":{\"id\":\"d1\",\"label\":\"Laptop\",\"repos\":[\"a/b\",\"c/d\"],\"permission\":\"manage\",\"expiresAt\":5},\"transcribe\":false}");
    CHECK(api_discovery(c, &d, &e));
    CHECK_STR(stub.last_url, BASE); CHECK_STR(stub.last_method, "GET"); CHECK(stub.last_body == NULL);
    CHECK_INT(d.version, 1); CHECK_STR(d.device.id, "d1"); CHECK_STR(d.device.label, "Laptop"); CHECK_INT(d.device.repo_count, 2); CHECK_INT(d.transcribe, 0);
    discovery_free(&d);
    const char *versions[] = { "0", "2", "-1", "100" };
    for (size_t i = 0; i < sizeof versions / sizeof *versions; i++) {
        char *body = xstrfmt("{\"version\":%s,\"client\":{\"id\":\"d\",\"label\":\"L\",\"repos\":[],\"permission\":\"read\",\"expiresAt\":0}}", versions[i]);
        stub_json(&stub, 200, body);
        api_error_init(&e);
        CHECK(!api_discovery(c, &d, &e));
        bool expected = str_eq(versions[i], "-1") ? e.kind == API_NON_JSON : e.kind == API_INCOMPATIBLE_VERSION;
        if (!expected) printf("  version %s gave kind %d\n", versions[i], e.kind);
        CHECK(expected);
        api_error_clear(&e); free(body);
    }
    // No version, a string version or a fraction is not this API's answer.
    const char *bodies[] = {
        "{\"client\":{\"id\":\"d\",\"label\":\"L\",\"repos\":[],\"permission\":\"read\",\"expiresAt\":0}}",
        "{\"version\":\"1\",\"client\":{\"id\":\"d\",\"label\":\"L\",\"repos\":[],\"permission\":\"read\",\"expiresAt\":0}}",
        "{\"version\":1.5,\"client\":{\"id\":\"d\",\"label\":\"L\",\"repos\":[],\"permission\":\"read\",\"expiresAt\":0}}",
        "{\"version\":1}", "{\"version\":1,\"client\":{}}", "[]", "null",
        // The mobile API's `device` record is not read (#26).
        "{\"version\":1,\"device\":{\"id\":\"d\",\"label\":\"L\",\"repos\":[],\"permission\":\"read\",\"expiresAt\":0}}",
    };
    for (size_t i = 0; i < sizeof bodies / sizeof *bodies; i++) {
        stub_json(&stub, 200, bodies[i]);
        api_error_init(&e);
        CHECK(!api_discovery(c, &d, &e)); CHECK(e.kind == API_NON_JSON);
        api_error_clear(&e);
    }
    stub_json(&stub, 401, "{\"error\":\"Unknown token\"}");
    CHECK(!api_discovery(c, &d, &e)); CHECK(api_error_unauthorized(&e)); CHECK_STR(e.message, "Unknown token");
    stub_json(&stub, 302, ""); stub.content_type = "text/html";
    CHECK(!api_discovery(c, &d, &e)); CHECK(e.kind == API_REDIRECTED);
    api_error_clear(&e); api_client_release(c); stub_reset(&stub);
}
static void test_catalog_errors(void) {
    Stub stub = { 0 };
    ApiClient *c = client(&stub);
    ApiError e; api_error_init(&e);
    Route *routes = NULL; size_t n = 0;
    stub_json(&stub, 404, "{\"error\":\"Not found\"}");
    CHECK(!api_catalog(c, &routes, &n, &e)); CHECK(e.kind == API_HTTP && e.status == 404);
    CHECK_STR(stub.last_url, BASE "openapi.json");
    stub_json(&stub, 200, "<html>"); stub.content_type = "text/html";
    CHECK(!api_catalog(c, &routes, &n, &e)); CHECK(e.kind == API_NON_JSON);
    api_error_clear(&e); api_client_release(c); stub_reset(&stub);
}

// MARK: - Raw bodies

static void test_transcribe_sends_the_recorded_type_and_reads_text(void) {
    Stub stub = { 0 }; stub_json(&stub, 200, "{\"text\":\"\"}");
    ApiClient *c = client(&stub);
    ApiError e; api_error_init(&e);
    unsigned char audio[] = { 0x1A, 0x45, 0x00, 0xDF, 0xA3 };
    char *text = api_transcribe(c, audio, sizeof audio, "audio/webm;codecs=opus", &e);
    CHECK_STR(text, ""); free(text);
    // No language query any more (#26).
    CHECK_STR(stub.last_url, BASE "transcribe"); CHECK_STR(stub.last_method, "POST");
    CHECK_STR(stub.last_content_type, "audio/webm;codecs=opus"); CHECK_INT(stub.last_body_len, sizeof audio);
    CHECK(memcmp(stub.last_body, audio, sizeof audio) == 0);
    CHECK_STR(stub.last_accept, "application/json");
    stub.body = "{\"text\":\"\xC2\xBFQu\xC3\xA9 tal?\"}";
    CHECK_OWNED_STR(api_transcribe(c, audio, sizeof audio, "audio/wav", &e), "\xC2\xBFQu\xC3\xA9 tal?");
    stub.body = "{\"text\":42}";
    CHECK(api_transcribe(c, audio, 1, NULL, &e) == NULL); CHECK(e.kind == API_NON_JSON);
    stub.status = 413; stub.body = "{\"error\":\"Audio too long\"}";
    CHECK(api_transcribe(c, audio, 1, NULL, &e) == NULL); CHECK(e.kind == API_HTTP && e.status == 413); CHECK_STR(e.message, "Audio too long");
    stub_json(&stub, 200, "{\"text\":\"x\"}"); stub.content_type = "text/plain";
    CHECK(api_transcribe(c, audio, 1, NULL, &e) == NULL); CHECK(e.kind == API_NON_JSON);
    api_error_clear(&e); api_client_release(c); stub_reset(&stub);
}
static void test_upload_names_ride_encoded_in_the_query(void) {
    Stub stub = { 0 }; stub_json(&stub, 201, "{\"file\":{\"id\":\"f1\"}}");
    ApiClient *c = client(&stub);
    ApiError e; api_error_init(&e);
    struct { const char *name, *query; } names[] = {
        { "report.pdf", "report.pdf" }, { NULL, "file" }, { "", "file" }, { "a/b\\c.txt", "a%2Fb%5Cc.txt" },
        { "q?x=1&y=2#z.txt", "q%3Fx%3D1%26y%3D2%23z.txt" }, { "caf\xC3\xA9 \xE2\x98\x95.png", "caf%C3%A9%20%E2%98%95.png" },
        { "100%.txt", "100%25.txt" }, { "+plus.txt", "%2Bplus.txt" }, { "~_-.", "~_-." },
    };
    for (size_t i = 0; i < sizeof names / sizeof *names; i++) {
        char *id = api_upload(c, names[i].name, "x", 1, &e);
        CHECK_STR(id, "f1"); free(id);
        char *url = xstrfmt(BASE "uploads?name=%s", names[i].query);
        CHECK_STR(stub.last_url, url); free(url);
        CHECK_STR(stub.last_content_type, "application/octet-stream"); CHECK_STR(stub.last_method, "POST");
    }
    // An empty file is still sent.
    char *id = api_upload(c, "empty.txt", "", 0, &e); CHECK_STR(id, "f1"); free(id); CHECK_INT(stub.last_body_len, 0);
    // An empty id, a missing file record or a non-string id is not an answer.
    const char *bodies[] = { "{\"file\":{\"id\":\"\"}}", "{\"file\":{}}", "{\"id\":\"f1\"}", "{\"file\":{\"id\":7}}", "{\"file\":\"f1\"}" };
    for (size_t i = 0; i < sizeof bodies / sizeof *bodies; i++) {
        stub_json(&stub, 201, bodies[i]);
        api_error_init(&e);
        CHECK(api_upload(c, "a", "x", 1, &e) == NULL); CHECK(e.kind == API_NON_JSON);
        api_error_clear(&e);
    }
    stub_json(&stub, 413, "{\"error\":\"Too large\"}");
    CHECK(api_upload(c, "a", "x", 1, &e) == NULL); CHECK(e.kind == API_HTTP && e.status == 413); CHECK_STR(e.message, "Too large");
    api_error_clear(&e); api_client_release(c); stub_reset(&stub);
}
static void test_upload_limit_boundary(void) {
    Stub stub = { 0 }; stub_json(&stub, 201, "{\"file\":{\"id\":\"big\"}}");
    ApiClient *c = client(&stub);
    ApiError e; api_error_init(&e);
    CHECK_INT(API_UPLOAD_LIMIT, 26214400);
    unsigned char *bytes = xcalloc(1, (size_t)API_UPLOAD_LIMIT + 1);
    bytes[0] = 0x89; bytes[API_UPLOAD_LIMIT - 1] = 0x42;
    char *id = api_upload(c, "max.bin", bytes, API_UPLOAD_LIMIT, &e);
    CHECK_STR(id, "big"); free(id);
    CHECK_INT(stub.calls, 1); CHECK_INT(stub.last_body_len, API_UPLOAD_LIMIT);
    CHECK((unsigned char)stub.last_body[API_UPLOAD_LIMIT - 1] == 0x42);
    CHECK(api_upload(c, "over.bin", bytes, (size_t)API_UPLOAD_LIMIT + 1, &e) == NULL);
    CHECK(e.kind == API_HTTP && e.status == 413); CHECK(api_error_is_refusal(&e)); CHECK_STR(e.message, "The file exceeds the server\xE2\x80\x99s 25 MB limit for an attachment.");
    CHECK_INT(stub.calls, 1);
    free(bytes);
    api_error_clear(&e); api_client_release(c); stub_reset(&stub);
}

// MARK: - Retry-After

static void test_retry_after_seconds(void) {
    CHECK(api_retry_after("0", 0) == 0);
    CHECK(api_retry_after("120", 1000) == 120);
    CHECK(api_retry_after(" 7 ", 0) == 7);
    CHECK(api_retry_after("\t7\r\n", 0) == 7);
    CHECK(api_retry_after("1.5", 0) == 1.5);
    CHECK(api_retry_after("-0.5", 0) == 0);
    CHECK(api_retry_after("86400", 0) == 86400);
    CHECK(api_retry_after("", 0) < 0);
    CHECK(api_retry_after("   ", 0) < 0);
    CHECK(api_retry_after("12abc", 0) < 0);
    CHECK(api_retry_after("12 34", 0) < 0);
    CHECK(api_retry_after("inf", 0) < 0);
    CHECK(api_retry_after("nan", 0) < 0);
    CHECK(api_retry_after("1e999", 0) < 0);
}
static void test_retry_after_http_dates(void) {
    // 2015-10-21T07:28:00Z
    time_t when = 1445412480;
    CHECK(api_retry_after("Wed, 21 Oct 2015 07:28:00 GMT", when - 30) == 30);
    CHECK(api_retry_after("Wed, 21 Oct 2015 07:28:00 GMT", when) == 0);
    CHECK(api_retry_after("Wed, 21 Oct 2015 07:28:00 GMT", when + 3600) == 0);
    CHECK(api_retry_after("  Wed, 21 Oct 2015 07:28:00 GMT  ", when - 1) == 1);
    CHECK(api_retry_after("wed, 21 oct 2015 07:28:00 gmt", when - 5) == 5);
    CHECK(api_retry_after("Thu, 31 Dec 2099 23:59:59 GMT", 4102444799 - 60) == 60);
    CHECK(api_retry_after("Sat, 29 Feb 2020 12:00:00 GMT", 1582977600 - 10) == 10);
    const char *months[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    time_t firsts[] = { 1704067200, 1706745600, 1709251200, 1711929600, 1714521600, 1717200000, 1719792000, 1722470400, 1725148800, 1727740800, 1730419200, 1733011200 };
    for (int i = 0; i < 12; i++) {
        char value[40]; snprintf(value, sizeof value, "Mon, 01 %s 2024 00:00:00 GMT", months[i]);
        CHECK(api_retry_after(value, firsts[i] - 100) == 100);
    }
    const char *bad[] = { "Wed, 21 Foo 2015 07:28:00 GMT", "Wed, 32 Oct 2015 07:28:00 GMT", "Wed, 21 Oct 2015 24:00:00 GMT", "Wed, 21 Oct 2015 07:60:00 GMT",
                          "Wed, 21 Oct 2015 07:28:00", "Wednesday, 21-Oct-15 07:28:00 GMT", "Wed Oct 21 07:28:00 2015", "2015-10-21T07:28:00Z", "Wed, 21 Oct" };
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) { if (api_retry_after(bad[i], 0) >= 0) printf("  read %s\n", bad[i]); CHECK(api_retry_after(bad[i], 0) < 0); }
}



static void test_browser_stream_route_and_cancellation(void) {
    Stub stub = {0}; ApiClient *c = client(&stub); ApiError error; api_error_init(&error);
    const ApiRoute *route = api_route("browser_stream"); CHECK(route != NULL); CHECK_STR(route ? route->method : NULL, "GET");
    CHECK_STR(route ? route->path : NULL, "sessions/{sessionId}/browser/stream");
    ApiStreamCancel cancel; api_stream_cancel_init(&cancel); api_stream_cancel(&cancel);
    Json *args = json_parsez("{\"sessionId\":\"s1\"}");
    CHECK(!api_stream(c, "browser_stream", args, &cancel, NULL, NULL, &error)); CHECK(error.kind == API_CANCELLED); CHECK_INT(stub.calls, 0);
    api_stream_cancel_free(&cancel); api_stream_cancel_init(&cancel);
    CHECK(!api_stream(c, "browser_stream", NULL, &cancel, NULL, NULL, &error)); CHECK_INT(error.status, 400);
    api_error_clear(&error);
    CHECK(!api_stream(c, "browser_on", args, &cancel, NULL, NULL, &error)); CHECK_INT(error.status, 400);
    api_error_clear(&error);
    CHECK(!api_stream(c, "unknown_stream", args, &cancel, NULL, NULL, &error)); CHECK_INT(error.status, 400);
    CHECK_INT(stub.calls, 0);
    api_stream_cancel_free(&cancel); json_free(args); api_error_clear(&error); api_client_release(c); stub_reset(&stub);
}

static void test_final_catalog_transport_and_session_requests(void) {
    Str fixture; str_init(&fixture);
    for (size_t i = 0; i < sizeof OCTOBER_CATALOG / sizeof *OCTOBER_CATALOG; i++) str_appendz(&fixture, OCTOBER_CATALOG[i]);
    Stub stub = {0}; ApiClient *c = client(&stub); ApiError e; api_error_init(&e);
    stub_json(&stub, 200, fixture.data);
    Route *routes = NULL; size_t n = 0;
    CHECK(api_catalog(c, &routes, &n, &e));
    CHECK(routes_allow(routes, n, "POST", "sessions/7/messages", "manage"));
    CHECK(!routes_allow(routes, n, "POST", "sessions/7/messages", "read"));
    routes_free(routes, n); str_free(&fixture);
    const char *operations[] = { "start_session", "message", "cancel", "compact" };
    for (size_t i = 0; i < sizeof operations / sizeof *operations; i++) {
        Json *args = json_object();
        if (i == 0) {
            json_set_str(args, "repo", "o/r"); json_set_str(args, "prompt", "continue");
            json_set_num(args, "provider", 2); json_set_str(args, "model", "gpt-5.6-sol"); json_set_str(args, "effort", "high");
        } else {
            json_set_str(args, "sessionId", "s/7");
            if (i == 1) json_set_str(args, "text", "continue");
        }
        stub_json(&stub, i == 0 ? 201 : 200, "{\"session\":{\"id\":\"s/7\",\"status\":\"running\"}}");
        Json *result = api_call(c, operations[i], args, 0, &e); CHECK(result != NULL); json_free(result);
        CHECK_STR(stub.last_method, "POST");
        CHECK_STR(stub.last_url, i == 0 ? BASE "sessions" : i == 1 ? BASE "sessions/s%2F7/messages" :
            i == 2 ? BASE "sessions/s%2F7/cancel" : BASE "sessions/s%2F7/compact");
        Json *body = json_parsez(stub.last_body);
        if (i == 0) {
            CHECK_INT(json_int_or(json_get(body, "provider"), 0), 2);
            CHECK_STR(json_str(json_get(body, "model")), "gpt-5.6-sol"); CHECK_STR(json_str(json_get(body, "effort")), "high");
            CHECK_STR(json_str(json_get(body, "repo")), "o/r"); CHECK_STR(json_str(json_get(body, "prompt")), "continue");
            CHECK_INT(json_count(body), 5);
        } else if (i == 1) {
            CHECK_STR(json_str(json_get(body, "text")), "continue"); CHECK_INT(json_count(body), 1);
        } else CHECK_INT(json_count(body), 0);
        CHECK(json_is_null(json_get(body, "sessionId"))); json_free(body);
        const int failures[] = { 401, 403, 404, 409, 429, 503 };
        for (size_t k = 0; k < sizeof failures / sizeof *failures; k++) {
            stub_json(&stub, failures[k], "{\"error\":\"Final contract refusal\"}");
            stub.retry_after = failures[k] == 429 ? "120" : NULL;
            int before = stub.calls;
            result = api_call(c, operations[i], args, 0, &e);
            CHECK(result == NULL); CHECK_INT(stub.calls, before + 1); CHECK_INT(e.status, failures[k]);
            CHECK(e.kind == API_HTTP); CHECK_STR(e.message, "Final contract refusal");
            CHECK(api_error_unauthorized(&e) == (failures[k] == 401));
            CHECK(api_error_is_refusal(&e) == (failures[k] < 500));
            CHECK(e.retry_after == (failures[k] == 429 ? 120 : -1));
            json_free(result);
        }
        stub.fail = true; stub.fail_message = "connection lost after write"; int before = stub.calls;
        result = api_call(c, operations[i], args, 0, &e);
        CHECK(result == NULL); CHECK(e.kind == API_NETWORK); CHECK_INT(stub.calls, before + 1);
        json_free(result); json_free(args);
    }
    api_error_clear(&e); api_client_release(c); stub_reset(&stub);
}

static void test_mcp_contract_and_errors(void) {
    Stub stub = {0}; stub.status = 201; stub.content_type = "application/json";
    stub.body = "{\"server\":{\"id\":1791403200000,\"status\":\"needs-sign-in\",\"signInUrl\":\"https://auth.example/?state=s\",\"signInNeedsPaste\":true,\"headerNames\":[\"Authorization\"],\"envNames\":[],\"hasOAuthClientSecret\":true}}";
    ApiClient *c = client(&stub); ApiError e; api_error_init(&e);
    Json *args = json_parsez("{\"id\":1791403200000,\"signIn\":true}");
    Json *result = api_call(c, "connect_mcp_server", args, 60000, &e);
    CHECK(result != NULL); CHECK_STR(stub.last_method, "POST");
    CHECK_STR(stub.last_url, BASE "settings/mcp/servers/1791403200000/connect");
    CHECK_STR(stub.last_body, "{\"signIn\":true}"); CHECK_INT(stub.last_timeout, 60000);
    CHECK(json_bool_is(json_get(json_get(result, "server"), "signInNeedsPaste"), true)); json_free(result);
    json_object_remove(args, "signIn"); json_set_str(args, "url", "http://127.0.0.1:4000/callback?code=one&state=s");
    result = api_call(c, "finish_mcp_sign_in", args, 60000, &e); CHECK(result != NULL);
    CHECK_STR(stub.last_url, BASE "settings/mcp/servers/1791403200000/finish-sign-in");
    CHECK_STR(stub.last_body, "{\"url\":\"http://127.0.0.1:4000/callback?code=one&state=s\"}"); json_free(result);
    const int errors[] = { 400, 401, 403, 404, 409, 429, 503 };
    for (size_t i = 0; i < sizeof errors / sizeof *errors; i++) {
        stub.status = errors[i]; stub.body = "{\"error\":\"sign-in unavailable\"}"; stub.retry_after = "45";
        int before = stub.calls;
        result = api_call(c, "finish_mcp_sign_in", args, 60000, &e);
        CHECK(result == NULL); CHECK_INT(e.status, errors[i]); CHECK(e.retry_after == 45); CHECK_INT(stub.calls, before + 1);
        CHECK(api_error_is_refusal(&e) == (errors[i] < 500)); api_error_clear(&e);
    }
    stub.fail = true; stub.fail_message = "timeout";
    int before = stub.calls; result = api_call(c, "create_mcp_server", args, 60000, &e);
    CHECK(result == NULL && e.kind == API_NETWORK); CHECK_INT(stub.calls, before + 1);
    json_free(args); api_error_clear(&e); api_client_release(c); stub_reset(&stub);
}

static void test_mail_read_transport(void) {
    Stub s = {0}; ApiClient *c = client(&s); ApiError e; api_error_init(&e);
    Json *args = json_object(); json_set_num(args, "account", 8); json_set_str(args, "id", "A/+=%?");
    stub_json(&s, 200, "{\"message\":{}}");
    Json *result = api_call(c, "mail_message", args, 1000, &e); CHECK(result != NULL); json_free(result);
    CHECK_STR(s.last_url, BASE "mail/accounts/8/messages/A%2F%2B%3D%25%3F"); CHECK_STR(s.last_method, "GET"); CHECK(s.last_body == NULL);
    json_object_remove(args, "id"); json_set_str(args, "q", "sender & subject"); json_set_num(args, "unread", 0);
    json_set_num(args, "inbox", 1); json_set_num(args, "starred", 0); json_set_str(args, "label", "My /folder");
    json_set_str(args, "thread", "thread/+="); json_set_str(args, "cursor", "cursor/+="); json_set_num(args, "limit", 50);
    stub_json(&s, 200, "{\"messages\":[],\"nextCursor\":null}");
    result = api_call(c, "mail_messages", args, 1000, &e); CHECK(result != NULL); json_free(result);
    CHECK_STR(s.last_url, BASE "mail/messages?account=8&q=sender%20%26%20subject&unread=0&inbox=1&starred=0&label=My%20%2Ffolder&thread=thread%2F%2B%3D&cursor=cursor%2F%2B%3D&limit=50");
    CHECK_STR(s.last_method, "GET"); CHECK(s.last_body == NULL);
    const int errors[] = { 400, 401, 403, 404, 409, 429, 503 };
    for (size_t i = 0; i < sizeof errors / sizeof *errors; i++) {
        stub_json(&s, errors[i], "{\"error\":\"refused\"}"); s.retry_after = "91";
        int calls = s.calls; CHECK(api_call(c, "mail_messages", args, 1000, &e) == NULL);
        CHECK_INT(s.calls, calls + 1); CHECK_INT(e.status, errors[i]); CHECK(e.retry_after == 91);
    }
    s.fail = true; s.fail_message = "offline"; CHECK(api_call(c, "mail_messages", args, 1000, &e) == NULL); CHECK_INT(e.kind, API_NETWORK);
    json_free(args); api_error_clear(&e); api_client_release(c); stub_reset(&s);
}

static void test_mail_transport_contract(void) {
    Stub s = {0}; ApiClient *c = client(&s); ApiError e; api_error_init(&e);
    const char *ops[] = { "settings_mail_accounts", "connect_mail_account", "finish_mail_account", "update_mail_account", "delete_mail_account", "sync_mail_account" };
    Json *args = json_object(); json_set_num(args, "id", 7); json_set_num(args, "accountId", 7);
    json_set_str(args, "provider", "gmail"); json_set_str(args, "state", "pending"); json_set_str(args, "code", "single-use");
    for (size_t i = 0; i < sizeof ops / sizeof *ops; i++) {
        stub_json(&s, i == 5 ? 202 : i == 2 ? 201 : 200, "{}");
        Json *result = api_call(c, ops[i], args, 1000, &e); CHECK(result != NULL); json_free(result);
        if (i == 1) { Json *b = json_parsez(s.last_body); CHECK_INT(json_int_or(json_get(b, "accountId"), 0), 7); json_free(b); }
        if (i == 3) { CHECK_STR(s.last_url, BASE "settings/mail/accounts/7"); Json *b = json_parsez(s.last_body); CHECK(json_is_null(json_get(b, "id"))); json_free(b); }
        if (i == 5) CHECK(str_has_prefix(s.last_url, BASE "settings/mail/accounts/7/sync"));
    }
    const int errors[] = { 400, 401, 403, 404, 409, 429, 503 };
    for (size_t i = 0; i < sizeof errors / sizeof *errors; i++) {
        stub_json(&s, errors[i], "{\"error\":\"refused\"}"); s.retry_after = "30";
        int calls = s.calls; CHECK(api_call(c, "finish_mail_account", args, 1000, &e) == NULL);
        CHECK_INT(s.calls, calls + 1); CHECK_INT(e.status, errors[i]); CHECK(e.retry_after == 30);
    }
    json_free(args); api_error_clear(&e); api_client_release(c); stub_reset(&s);
}

static void test_removed_integrations_are_unregistered(void) {
    CHECK(api_route("slack_workspaces") == NULL);
    CHECK(api_route("slack_events") == NULL);
    CHECK(api_route("slack_send") == NULL);
    CHECK(api_route("settings_slack_workspaces") == NULL);
    CHECK(api_route("create_slack_workspace") == NULL);
    CHECK(api_route("update_slack_workspace") == NULL);
    CHECK(api_route("delete_slack_workspace") == NULL);
    CHECK(api_route("preview_access") == NULL);
    CHECK(api_route("serve") != NULL);
    CHECK(api_route("serve_branch") != NULL);
    CHECK(api_route("browser_stream") != NULL);
}

void api_tests(void) {
    test_run("browser stream route and cancellation before the network", test_browser_stream_route_and_cancellation);
    test_run("removed integrations are unregistered; serving and shared browser remain", test_removed_integrations_are_unregistered);
    test_run("final catalog transport and provider session request failures", test_final_catalog_transport_and_session_requests);
    test_run("MCP connect/finish pin large ids and never retry failed writes", test_mcp_contract_and_errors);
    test_run("mail read routes encode opaque IDs filters and cursors without writes or retries", test_mail_read_transport);
    test_run("mail six routes preserve accountId 202 failures and never retry exchanges", test_mail_transport_contract);
    test_run("a run profile switch names the session in the path", test_a_run_profile_switch_names_the_session_in_the_path);
    test_run("server address accepts https origins and the api base", test_server_address_accepts_https_origins_and_the_api_base);
    test_run("server address refuses everything else", test_server_address_refuses_everything_else);
    test_run("server address copy is independent", test_server_address_copy_is_independent);
    test_run("token shape", test_token_shape);
    test_run("client refuses a bad token without an error to fill", test_client_refuses_a_bad_token_without_an_error_to_fill);
    test_run("client keeps its own copy of the address and counts references", test_client_keeps_its_own_copy_of_the_address_and_counts_references);
    test_run("error set clear and copy", test_error_set_clear_and_copy);
    test_run("error descriptions for every kind", test_error_descriptions_for_every_kind);
    test_run("unauthorized is only a 401 and refusals are only 4xx", test_unauthorized_is_only_a_401_and_refusals_are_only_4xx);
    test_run("every route has its method path and flags", test_every_route_has_its_method_path_and_flags);
    test_run("route lookup is exact", test_route_lookup_is_exact);
    test_run("every route builds its request", test_every_route_builds_its_request);
    test_run("path arguments are encoded and numbers written whole", test_path_arguments_are_encoded_and_numbers_written_whole);
    test_run("a missing or unusable path argument is refused before the network", test_a_missing_or_unusable_path_argument_is_refused_before_the_network);
    test_run("query arguments of reads", test_query_arguments_of_reads);
    test_run("json bodies of writes", test_json_bodies_of_writes);
    test_run("the set flag is always sent true", test_the_set_flag_is_always_sent_true);
    test_run("the session list is cut to the requested repo here", test_the_session_list_is_cut_to_the_requested_repo_here);
    test_run("the caller's arguments are left as they were", test_the_callers_arguments_are_left_as_they_were);
    test_run("timeouts reach the transport", test_timeouts_reach_the_transport);
    test_run("request size limit boundary", test_request_size_limit_boundary);
    test_run("successful answers need json", test_successful_answers_need_json);
    test_run("redirects of any kind are refused", test_redirects_of_any_kind_are_refused);
    test_run("error answers keep the server's words or the status", test_error_answers_keep_the_servers_words_or_the_status);
    test_run("retry after is read from any error answer", test_retry_after_is_read_from_any_error_answer);
    test_run("network failures say why", test_network_failures_say_why);
    test_run("a success needs no error to fill", test_a_success_needs_no_error_to_fill);
    test_run("discovery reads the client record and refuses other versions", test_discovery_reads_the_client_record_and_refuses_other_versions);
    test_run("catalog errors", test_catalog_errors);
    test_run("transcribe sends the recorded type and reads text", test_transcribe_sends_the_recorded_type_and_reads_text);
    test_run("upload names ride encoded in the query", test_upload_names_ride_encoded_in_the_query);
    test_run("upload limit boundary", test_upload_limit_boundary);
    test_run("retry after seconds", test_retry_after_seconds);
    test_run("retry after http dates", test_retry_after_http_dates);
}
