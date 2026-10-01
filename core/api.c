#include "api.h"
#include "str.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <winhttp.h>

#ifndef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2
#define WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 0x00000800
#endif
#ifndef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3
#define WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3 0x00002000
#endif
#ifndef WINHTTP_OPTION_REDIRECT_POLICY
#define WINHTTP_OPTION_REDIRECT_POLICY 88
#endif
#ifndef WINHTTP_OPTION_REDIRECT_POLICY_NEVER
#define WINHTTP_OPTION_REDIRECT_POLICY_NEVER 0
#endif
#ifndef WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY
#define WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY 4
#endif

#define REQUEST_TIMEOUT_MS 30000
#define MAX_REQUEST_BYTES 1048576

// MARK: - Errors

void api_error_init(ApiError *e) { e->kind = API_OK; e->status = 0; e->message = NULL; e->retry_after = -1; }
void api_error_clear(ApiError *e) { if (!e) return; free(e->message); api_error_init(e); }
void api_error_set(ApiError *e, ApiErrorKind kind, int status, const char *message, double retry_after) {
    if (!e) return;
    api_error_clear(e);
    e->kind = kind; e->status = status; e->message = xstrdup(message); e->retry_after = retry_after;
}
void api_error_copy(ApiError *into, const ApiError *from) { api_error_set(into, from->kind, from->status, from->message, from->retry_after); }
bool api_error_unauthorized(const ApiError *e) { return e && e->kind == API_HTTP && e->status == 401; }
bool api_error_is_refusal(const ApiError *e) { return e && e->kind == API_HTTP && e->status >= 400 && e->status < 500; }

char *api_error_description(const ApiError *e) {
    switch (e->kind) {
    case API_OK: return xstrdup("");
    case API_INVALID_ADDRESS: return xstrdup("Enter an HTTPS server address, optionally ending in /api/v1, without credentials or query parameters.");
    case API_INVALID_TOKEN: return xstrdup("Paste the complete token from Settings \xE2\x86\x92 Devices and clients.");
    case API_REDIRECTED: return xstrdup("The server redirected this request. Check the Cloudflare Access exception for /api/v1 and /api/v1/*.");
    case API_NON_JSON: return xstrdup("The server returned an unexpected response. Check that the client API is deployed and reachable through Cloudflare Access.");
    case API_INCOMPATIBLE_VERSION: return xstrdup("This server uses an unsupported client API version.");
    case API_OVERSIZED_REQUEST: return xstrdup("This message exceeds the server\xE2\x80\x99s 1 MiB request limit. Shorten it before sending.");
    case API_NETWORK: return xstrdup(e->message ? e->message : "The server could not be reached.");
    case API_CANCELLED: return xstrdup("Cancelled.");
    case API_HTTP:
        if (e->status == 401) return xstrdup("This token has expired or was revoked. Reconnect with a new token.");
        if (e->status == 403) return xstrfmt("Access denied: %s", e->message ? e->message : "");
        if (e->status == 429) return xstrdup("The server is rate limiting requests. Updates will resume after a delay.");
        return xstrfmt("%s (HTTP %d)", e->message ? e->message : "Request failed", e->status);
    }
    return xstrdup("Unknown error");
}

static const char *status_text(int status) {
    switch (status) {
    case 400: return "bad request"; case 401: return "unauthorized"; case 403: return "forbidden"; case 404: return "not found";
    case 405: return "method not allowed"; case 408: return "request timeout"; case 409: return "conflict"; case 413: return "request too large";
    case 422: return "unprocessable entity"; case 429: return "too many requests"; case 500: return "internal server error";
    case 502: return "bad gateway"; case 503: return "service unavailable"; case 504: return "gateway timeout";
    default: return "request failed";
    }
}

// MARK: - Address

bool server_address_parse(const char *input, ServerAddress *out) {
    memset(out, 0, sizeof *out);
    char *trimmed = str_trim(input);
    bool ok = false;
    const char *p = trimmed;
    if (strlen(p) < 8 || tolower((unsigned char)p[0]) != 'h' || tolower((unsigned char)p[1]) != 't' || tolower((unsigned char)p[2]) != 't'
        || tolower((unsigned char)p[3]) != 'p' || tolower((unsigned char)p[4]) != 's' || strncmp(p + 5, "://", 3) != 0) goto done;
    p += 8;
    if (strchr(p, '?') || strchr(p, '#')) goto done;
    const char *slash = strchr(p, '/');
    size_t authority_len = slash ? (size_t)(slash - p) : strlen(p);
    if (authority_len == 0) goto done;
    char *authority = xstrndup(p, authority_len);
    const char *path = slash ? slash : "";
    if (strchr(authority, '@')) { free(authority); goto done; }
    char *host; int port = 0;
    if (authority[0] == '[') {
        char *close = strchr(authority, ']');
        if (!close) { free(authority); goto done; }
        host = xstrndup(authority, (size_t)(close - authority + 1));
        const char *rest = close + 1;
        if (*rest == ':') {
            char *end; long n = strtol(rest + 1, &end, 10);
            if (*end || end == rest + 1 || n < 1 || n > 65535) { free(host); free(authority); goto done; }
            port = (int)n;
        } else if (*rest) { free(host); free(authority); goto done; }
    } else {
        char *colon = strchr(authority, ':');
        if (colon) {
            host = xstrndup(authority, (size_t)(colon - authority));
            char *end; long n = strtol(colon + 1, &end, 10);
            if (*end || end == colon + 1 || n < 1 || n > 65535) { free(host); free(authority); goto done; }
            port = (int)n;
        } else host = xstrdup(authority);
    }
    free(authority);
    if (!*host) { free(host); goto done; }
    for (char *h = host; *h; h++) {
        unsigned char c = (unsigned char)*h;
        if (!(isalnum(c) || c == '-' || c == '.' || c == '[' || c == ']' || c == ':' || c == '_')) { free(host); goto done; }
        *h = (char)tolower(c);
    }
    if (!(str_eq(path, "") || str_eq(path, "/") || str_eq(path, "/api/v1") || str_eq(path, "/api/v1/"))) { free(host); goto done; }
    if (port == 443) port = 0;
    out->host = host; out->port = port;
    out->origin = port ? xstrfmt("https://%s:%d", host, port) : xstrfmt("https://%s", host);
    out->base_url = xstrfmt("%s/api/v1/", out->origin);
    ok = true;
done:
    free(trimmed);
    return ok;
}
void server_address_free(ServerAddress *a) { if (!a) return; free(a->base_url); free(a->origin); free(a->host); memset(a, 0, sizeof *a); }
void server_address_copy(ServerAddress *into, const ServerAddress *from) {
    into->base_url = xstrdup(from->base_url); into->origin = xstrdup(from->origin); into->host = xstrdup(from->host); into->port = from->port;
}

bool api_token_valid(const char *token) {
    if (!token || strncmp(token, "brm_", 4) != 0 || strlen(token) != 47) return false;
    for (const char *p = token + 4; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (!(isalnum(c) || c == '_' || c == '-')) return false;
    }
    return true;
}

// MARK: - Client

struct ApiClient {
    ServerAddress address;
    char *token;
    ApiTransport transport;
    void *transport_ctx;
    HINTERNET session;
    volatile LONG refs;
};

ApiClient *api_client_new(const ServerAddress *address, const char *token, ApiError *error) {
    if (!api_token_valid(token)) { api_error_set(error, API_INVALID_TOKEN, 0, NULL, -1); return NULL; }
    ApiClient *c = xcalloc(1, sizeof *c);
    server_address_copy(&c->address, address);
    c->token = xstrdup(token);
    c->transport = api_winhttp_transport;
    c->refs = 1;
    c->session = WinHttpOpen(L"Briareus-Windows/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!c->session) c->session = WinHttpOpen(L"Briareus-Windows/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (c->session) {
        DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
        if (!WinHttpSetOption(c->session, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof protocols)) {
            protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
            WinHttpSetOption(c->session, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof protocols);
        }
        DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
        WinHttpSetOption(c->session, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof policy);
    }
    c->transport_ctx = c->session;
    return c;
}
ApiClient *api_client_retain(ApiClient *c) { if (c) InterlockedIncrement(&c->refs); return c; }
void api_client_release(ApiClient *c) {
    if (!c || InterlockedDecrement(&c->refs) > 0) return;
    if (c->session) WinHttpCloseHandle(c->session);
    server_address_free(&c->address); free(c->token); free(c);
}
void api_client_set_transport(ApiClient *c, ApiTransport transport, void *ctx) { c->transport = transport; c->transport_ctx = ctx; }
const ServerAddress *api_client_address(const ApiClient *c) { return &c->address; }

static Json *send_request(ApiClient *c, const char *method, const char *url, const char *content_type, const void *body, size_t body_len,
                          int timeout_ms, ApiError *error) {
    char *auth = xstrfmt("Bearer %s", c->token);
    const char *headers[8]; size_t h = 0;
    headers[h++] = "Authorization"; headers[h++] = auth;
    headers[h++] = "Accept"; headers[h++] = "application/json";
    if (content_type) { headers[h++] = "Content-Type"; headers[h++] = content_type; }
    headers[h] = NULL;
    int status = 0; char *type = NULL, *retry = NULL, *response = NULL, *why = NULL; size_t response_len = 0;
    // No automatic application-level retries, including for POST reads: the screen owns read backoff.
    bool received = c->transport(c->transport_ctx, method, url, headers, body, body_len, timeout_ms ? timeout_ms : REQUEST_TIMEOUT_MS,
                                 &status, &type, &retry, &response, &response_len, &why);
    free(auth);
    Json *result = NULL;
    if (!received) {
        api_error_set(error, API_NETWORK, 0, why ? why : "The server could not be reached.", -1);
    } else if (status >= 300 && status < 400) {
        api_error_set(error, API_REDIRECTED, status, NULL, -1);
    } else {
        bool json = false;
        if (type) {
            char *folded = str_fold(type);
            char *semi = strchr(folded, ';'); if (semi) *semi = 0;
            char *trimmed = str_trim(folded);
            json = str_eq(trimmed, "application/json");
            free(trimmed); free(folded);
        }
        if (status < 200 || status >= 300) {
            Json *payload = json ? json_parse(response, response_len) : NULL;
            const char *message = json_str_nonempty(json_get(payload, "error"));
            api_error_set(error, API_HTTP, status, message ? message : status_text(status), api_retry_after(retry, time(NULL)));
            json_free(payload);
        } else if (!json) {
            api_error_set(error, API_NON_JSON, status, NULL, -1);
        } else {
            result = json_parse(response, response_len);
            if (!result) api_error_set(error, API_NON_JSON, status, NULL, -1);
        }
    }
    free(type); free(retry); free(response); free(why);
    return result;
}

static Json *request(ApiClient *c, const char *path, const char *method, const Json *body, int timeout_ms, ApiError *error) {
    char *url = xstrfmt("%s%s", c->address.base_url, path);
    char *data = NULL; size_t len = 0;
    if (body) {
        data = json_serialize(body, false); len = strlen(data);
        if (len > MAX_REQUEST_BYTES) { free(data); free(url); api_error_set(error, API_OVERSIZED_REQUEST, 0, NULL, -1); return NULL; }
    }
    Json *result = send_request(c, method, url, body ? "application/json" : NULL, data, len, timeout_ms, error);
    free(data); free(url);
    return result;
}

bool api_discovery(ApiClient *c, Discovery *out, ApiError *error) {
    Json *j = request(c, "", "GET", NULL, 0, error);
    if (!j) return false;
    bool ok = discovery_parse(j, out);
    json_free(j);
    if (!ok) { api_error_set(error, API_NON_JSON, 0, NULL, -1); return false; }
    if (out->version != 1) { discovery_free(out); api_error_set(error, API_INCOMPATIBLE_VERSION, 0, NULL, -1); return false; }
    return true;
}
bool api_catalog(ApiClient *c, Route **routes, size_t *count, ApiError *error) {
    Json *j = request(c, "openapi.json", "GET", NULL, 0, error);
    if (!j) return false;
    bool ok = routes_parse(j, routes, count);
    json_free(j);
    if (!ok) api_error_set(error, API_NON_JSON, 0, NULL, -1);
    return ok;
}

// MARK: - Calls

// Every call the app makes, on the /api/v1 route that answers it. The names are the app's own; the paths are the server's,
// with each parameter named after the argument that fills it. A route without a `{}` sends every argument in the query
// (GET, DELETE) or the body (the rest).
static const ApiRoute ROUTES[] = {
    // Projects
    { "projects", "GET", "projects" },
    { "branches", "GET", "branches" },
    { "runtimes", "GET", "runtimes" },
    { "usage", "GET", "usage" },
    { "usage_all", "GET", "usage/all" },                   // every project's spend; a filter given as an array repeats
    { "actions", "GET", "actions" },
    { "action", "POST", "actions" },                       // an errand on a pull request, named in `action`
    // Pull requests
    { "pulls", "GET", "pulls" },
    { "pull", "GET", "pulls/{pr}" },
    { "pull_description", "GET", "pulls/{pr}/description" },
    { "pull_files", "GET", "pulls/{pr}/files" },
    { "pull_comments", "GET", "pulls/{pr}/comments" },
    { "pull_reviews", "GET", "pulls/{pr}/reviews" },
    { "pull_review_comments", "GET", "pulls/{pr}/review-comments" },
    { "findings", "GET", "pulls/{pr}/findings" },
    { "finding_decision", "POST", "pulls/{pr}/findings/decision" },
    { "merge_pull", "POST", "pulls/{pr}/merge" },
    { "serve_pull", "POST", "pulls/{prNumber}/serve" },
    // Sessions. The list has no project parameter: a `repo` argument cuts the answer down here instead.
    { "sessions", "GET", "sessions", NULL, "repo", "sessions" },
    { "start_session", "POST", "sessions" },
    { "review", "POST", "sessions", "review" },
    { "qa", "POST", "sessions", "qa" },
    { "session", "GET", "sessions/{sessionId}" },
    { "rename", "PATCH", "sessions/{sessionId}" },
    { "delete", "DELETE", "sessions/{sessionId}" },
    { "message", "POST", "sessions/{sessionId}/messages" },
    { "drop_message", "DELETE", "sessions/{sessionId}/queue/{index}" },
    { "cancel", "POST", "sessions/{sessionId}/cancel" },
    { "close", "POST", "sessions/{sessionId}/close" },
    { "reopen", "POST", "sessions/{sessionId}/reopen" },
    { "serve", "POST", "sessions/{sessionId}/serve" },
    { "compact", "POST", "sessions/{sessionId}/compact" },
    { "clear", "POST", "sessions/{sessionId}/clear" },
    { "review_loop", "POST", "sessions/{sessionId}/review-loop" },
    { "qa_loop", "POST", "sessions/{sessionId}/qa-loop" },
    { "link_pr", "POST", "sessions/{sessionId}/link-pr" },
    { "complete_findings", "POST", "sessions/{sessionId}/findings/triage" },
    { "save_findings", "POST", "sessions/{sessionId}/findings/save" },
    { "reply_finding", "POST", "sessions/{sessionId}/findings/reply" },
    { "delete_finding", "POST", "sessions/{sessionId}/findings/delete" },
    // Composer. These two send raw bytes (api_upload, api_transcribe); the entries say whether the server has them.
    { "upload", "POST", "uploads" },
    { "transcribe", "POST", "transcribe" },
    // Settings, for an admin token: every project with all its settings, and the values a new one starts from.
    { "settings_projects", "GET", "settings/projects" },
    { "create_project", "POST", "settings/projects" },
    { "update_project", "PUT", "settings/projects/{id}" },
    { "delete_project", "DELETE", "settings/projects/{id}" },
    { "order_projects", "PUT", "settings/projects/order" },
    // The providers sessions start on: every row, its connection and quota, its login, and a probe of an endpoint.
    { "settings_providers", "GET", "settings/providers" },
    { "create_provider", "POST", "settings/providers" },
    { "update_provider", "PUT", "settings/providers/{id}" },
    { "delete_provider", "DELETE", "settings/providers/{id}" },
    { "test_provider", "POST", "settings/providers/test" },
    { "provider_status", "GET", "settings/providers/{id}/status" },
    { "provider_login", "POST", "settings/providers/{id}/login" },
    { "provider_login_start", "POST", "settings/providers/{id}/login/start" },
    { "provider_login_finish", "POST", "settings/providers/{id}/login/finish" },
    // The database pool: the servers sessions claim one at a time, and a probe of one as its form holds it.
    { "settings_db_servers", "GET", "settings/db-servers" },
    { "create_db_server", "POST", "settings/db-servers" },
    { "update_db_server", "PUT", "settings/db-servers/{id}" },
    { "delete_db_server", "DELETE", "settings/db-servers/{id}" },
    { "test_db_server", "POST", "settings/db-servers/test" },
    // The SSH servers agents may run commands on, each held to one project; also for an admin token.
    { "settings_ssh_servers", "GET", "settings/ssh/servers" },
    { "create_ssh_server", "POST", "settings/ssh/servers" },
    { "update_ssh_server", "PUT", "settings/ssh/servers/{id}" },
    { "delete_ssh_server", "DELETE", "settings/ssh/servers/{id}" },
};
const ApiRoute *api_route(const char *name) {
    for (size_t i = 0; name && i < sizeof ROUTES / sizeof *ROUTES; i++) if (str_eq(ROUTES[i].name, name)) return &ROUTES[i];
    return NULL;
}

static char *url_encode(const char *s) {
    Str out; str_init(&out);
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (isalnum(*p) || *p == '-' || *p == '_' || *p == '.' || *p == '~') str_appendc(&out, (char)*p);
        else str_appendf(&out, "%%%02X", *p);
    }
    return str_detach(&out);
}
/// A string or number as it goes in a URL, or NULL for anything else (and for an empty string in a path).
static char *url_value(const Json *v, bool in_path) {
    const char *s = json_str(v);
    if (s) return in_path && !*s ? NULL : url_encode(s);
    double n;
    if (json_num(v, &n) && isfinite(n)) {
        if (n == floor(n) && fabs(n) < 1e15) return xstrfmt("%.0f", n);
        return in_path ? NULL : xstrfmt("%.17g", n);
    }
    if (!in_path && json_bool_tristate(v) >= 0) return xstrdup(json_bool_tristate(v) ? "1" : "0");
    return NULL;
}
Json *api_call(ApiClient *c, const char *name, const Json *arguments, int timeout_ms, ApiError *error) {
    const ApiRoute *route = api_route(name);
    if (!route) { api_error_set(error, API_HTTP, 400, "Unknown call", -1); return NULL; }
    Json *rest = arguments && json_is_object(arguments) ? json_clone(arguments) : json_object();
    Str path; str_init(&path);
    for (const char *p = route->path; *p;) {
        if (*p != '{') { str_appendc(&path, *p++); continue; }
        const char *end = strchr(p, '}');
        char *arg = xstrndup(p + 1, (size_t)(end - p - 1));
        char *value = url_value(json_get(rest, arg), true);
        if (!value) {
            char *why = xstrfmt("Missing argument: %s", arg);
            api_error_set(error, API_HTTP, 400, why, -1);
            free(why); free(arg); str_free(&path); json_free(rest);
            return NULL;
        }
        str_appendz(&path, value);
        json_object_remove(rest, arg);
        free(value); free(arg);
        p = end + 1;
    }
    char *kept = NULL;
    if (route->filter) {
        const char *f = json_str(json_get(rest, route->filter));
        if (f) kept = xstrdup(f);
        json_object_remove(rest, route->filter);
    }
    bool reads = str_eq(route->method, "GET") || str_eq(route->method, "DELETE");
    Json *result;
    if (reads) {
        char sep = '?';
        for (size_t i = 0; i < json_count(rest); i++) {
            // An array is a repeatable parameter: `project=a&project=b`.
            const Json *arg = json_get(rest, json_key(rest, i));
            size_t n = json_is_array(arg) ? json_count(arg) : 1;
            char *key = url_encode(json_key(rest, i));
            for (size_t k = 0; k < n; k++) {
                char *value = url_value(json_is_array(arg) ? json_at(arg, k) : arg, false);
                if (!value) continue;
                str_appendf(&path, "%c%s=%s", sep, key, value);
                sep = '&';
                free(value);
            }
            free(key);
        }
        result = request(c, path.data, route->method, NULL, timeout_ms, error);
    } else {
        if (route->set) json_set_bool(rest, route->set, true);
        result = request(c, path.data, route->method, rest, timeout_ms, error);
    }
    if (result && kept && route->list) {
        const Json *rows = json_get(result, route->list);
        Json *mine = json_array();
        for (size_t i = 0; i < json_count(rows); i++)
            if (str_eq(json_str(json_get(json_at(rows, i), route->filter)), kept)) json_array_push(mine, json_clone(json_at(rows, i)));
        json_object_set(result, route->list, mine);
    }
    free(kept); str_free(&path); json_free(rest);
    return result;
}

char *api_transcribe(ApiClient *c, const void *audio, size_t len, const char *content_type, ApiError *error) {
    char *url = xstrfmt("%stranscribe", c->address.base_url);
    // The server allows the transcription two minutes.
    Json *j = send_request(c, "POST", url, content_type ? content_type : "audio/mp4", audio, len, 150000, error);
    free(url);
    if (!j) return NULL;
    const char *text = json_str(json_get(j, "text"));
    char *result = xstrdup(text);
    json_free(j);
    if (!result) api_error_set(error, API_NON_JSON, 0, NULL, -1);
    return result;
}

char *api_upload(ApiClient *c, const char *name, const void *bytes, size_t len, ApiError *error) {
    if (len > API_UPLOAD_LIMIT) { api_error_set(error, API_HTTP, 413, "The file exceeds the server\xE2\x80\x99s 25 MB limit for an attachment.", -1); return NULL; }
    char *encoded = url_encode(str_empty(name) ? "file" : name);
    char *url = xstrfmt("%suploads?name=%s", c->address.base_url, encoded);
    free(encoded);
    // Always octet-stream, whatever the file is: the server reads the raw body.
    Json *j = send_request(c, "POST", url, "application/octet-stream", bytes, len, 120000, error);
    free(url);
    if (!j) return NULL;
    const char *id = json_str_nonempty(json_get(json_get(j, "file"), "id"));
    char *result = xstrdup(id);
    json_free(j);
    if (!result) api_error_set(error, API_NON_JSON, 0, NULL, -1);
    return result;
}

// MARK: - Retry-After

static int month_index(const char *m) {
    static const char *const names[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    for (int i = 0; i < 12; i++) if (str_ieq(m, names[i])) return i + 1;
    return 0;
}
double api_retry_after(const char *value, time_t now) {
    if (!value) return -1;
    char *trimmed = str_trim(value);
    if (!*trimmed) { free(trimmed); return -1; }
    char *end;
    double seconds = strtod(trimmed, &end);
    if (*end == 0 && isfinite(seconds)) { free(trimmed); return seconds < 0 ? 0 : seconds; }
    // "Thu, 01 Jan 1970 00:02:00 GMT"
    char day_name[4], month[4], zone[8];
    int day, year, h, mi, s;
    if (sscanf(trimmed, "%3s, %d %3s %d %d:%d:%d %7s", day_name, &day, month, &year, &h, &mi, &s, zone) == 8 && month_index(month)) {
        char iso[40];
        snprintf(iso, sizeof iso, "%04d-%02d-%02dT%02d:%02d:%02dZ", year, month_index(month), day, h, mi, s);
        time_t when;
        if (board_date_parse(iso, &when)) { free(trimmed); double d = difftime(when, now); return d < 0 ? 0 : d; }
    }
    free(trimmed);
    return -1;
}

// MARK: - WinHTTP transport

static char *winhttp_error_text(DWORD code) {
    switch (code) {
    case ERROR_WINHTTP_TIMEOUT: return xstrdup("The request timed out.");
    case ERROR_WINHTTP_CANNOT_CONNECT: return xstrdup("Could not connect to the server.");
    case ERROR_WINHTTP_NAME_NOT_RESOLVED: return xstrdup("The server address could not be found.");
    case ERROR_WINHTTP_CONNECTION_ERROR: return xstrdup("The connection with the server was interrupted.");
    case ERROR_WINHTTP_SECURE_FAILURE: return xstrdup("The server\xE2\x80\x99s certificate could not be verified.");
    case ERROR_WINHTTP_OPERATION_CANCELLED: return xstrdup("The request was cancelled.");
    default: break;
    }
    wchar_t *buffer = NULL;
    HMODULE module = GetModuleHandleW(L"winhttp.dll");
    DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_IGNORE_INSERTS | FORMAT_MESSAGE_FROM_SYSTEM | (module ? FORMAT_MESSAGE_FROM_HMODULE : 0);
    if (FormatMessageW(flags, module, code, 0, (LPWSTR)&buffer, 0, NULL) && buffer) {
        char *text = wide_to_utf8(buffer);
        LocalFree(buffer);
        char *trimmed = str_trim(text); free(text);
        return trimmed;
    }
    return xstrfmt("The network request failed (error %lu).", (unsigned long)code);
}

static char *query_header(HINTERNET request, DWORD info) {
    DWORD size = 0;
    WinHttpQueryHeaders(request, info, WINHTTP_HEADER_NAME_BY_INDEX, WINHTTP_NO_OUTPUT_BUFFER, &size, WINHTTP_NO_HEADER_INDEX);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || size == 0) return NULL;
    wchar_t *buffer = xmalloc(size + sizeof(wchar_t));
    if (!WinHttpQueryHeaders(request, info, WINHTTP_HEADER_NAME_BY_INDEX, buffer, &size, WINHTTP_NO_HEADER_INDEX)) { free(buffer); return NULL; }
    buffer[size / sizeof(wchar_t)] = 0;
    char *text = wide_to_utf8(buffer);
    free(buffer);
    return text;
}

bool api_winhttp_transport(void *ctx, const char *method, const char *url, const char *const *headers,
                           const void *body, size_t body_len, int timeout_ms,
                           int *status, char **content_type, char **retry_after, char **response, size_t *response_len,
                           char **error_message) {
    HINTERNET session = ctx, own_session = NULL, connection = NULL, request = NULL;
    bool ok = false;
    *status = 0; *content_type = NULL; *retry_after = NULL; *response = NULL; *response_len = 0; *error_message = NULL;
    if (!session) {
        own_session = WinHttpOpen(L"Briareus-Windows/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        session = own_session;
        if (!session) { *error_message = winhttp_error_text(GetLastError()); return false; }
    }
    wchar_t *wurl = utf8_to_wide(url);
    URL_COMPONENTS parts; memset(&parts, 0, sizeof parts); parts.dwStructSize = sizeof parts;
    wchar_t host[256] = L"", path[4096] = L"";
    parts.lpszHostName = host; parts.dwHostNameLength = 255;
    parts.lpszUrlPath = path; parts.dwUrlPathLength = 4095;
    if (!WinHttpCrackUrl(wurl, 0, 0, &parts) || parts.nScheme != INTERNET_SCHEME_HTTPS) {
        *error_message = xstrdup("The server address is not an HTTPS URL."); goto done;
    }
    connection = WinHttpConnect(session, host, parts.nPort, 0);
    if (!connection) { *error_message = winhttp_error_text(GetLastError()); goto done; }
    wchar_t *wmethod = utf8_to_wide(method);
    request = WinHttpOpenRequest(connection, wmethod, path, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                 WINHTTP_FLAG_SECURE | WINHTTP_FLAG_REFRESH);
    free(wmethod);
    if (!request) { *error_message = winhttp_error_text(GetLastError()); goto done; }
    DWORD disabled = WINHTTP_DISABLE_COOKIES | WINHTTP_DISABLE_REDIRECTS | WINHTTP_DISABLE_AUTHENTICATION | WINHTTP_DISABLE_KEEP_ALIVE;
    WinHttpSetOption(request, WINHTTP_OPTION_DISABLE_FEATURE, &disabled, sizeof disabled);
    DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof policy);
    WinHttpSetTimeouts(request, REQUEST_TIMEOUT_MS, REQUEST_TIMEOUT_MS, REQUEST_TIMEOUT_MS, timeout_ms > 0 ? timeout_ms : REQUEST_TIMEOUT_MS);
    Str header_block; str_init(&header_block);
    for (size_t i = 0; headers && headers[i] && headers[i + 1]; i += 2) str_appendf(&header_block, "%s: %s\r\n", headers[i], headers[i + 1]);
    wchar_t *wheaders = utf8_to_wide(header_block.data ? header_block.data : "");
    str_free(&header_block);
    BOOL sent = WinHttpSendRequest(request, *wheaders ? wheaders : WINHTTP_NO_ADDITIONAL_HEADERS, *wheaders ? (DWORD)-1 : 0,
                                   body_len ? (LPVOID)body : WINHTTP_NO_REQUEST_DATA, (DWORD)body_len, (DWORD)body_len, 0);
    free(wheaders);
    if (!sent) { *error_message = winhttp_error_text(GetLastError()); goto done; }
    if (!WinHttpReceiveResponse(request, NULL)) { *error_message = winhttp_error_text(GetLastError()); goto done; }
    DWORD code = 0, size = sizeof code;
    if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &code, &size, WINHTTP_NO_HEADER_INDEX)) {
        *error_message = winhttp_error_text(GetLastError()); goto done;
    }
    *status = (int)code;
    *content_type = query_header(request, WINHTTP_QUERY_CONTENT_TYPE);
    *retry_after = query_header(request, WINHTTP_QUERY_RETRY_AFTER);
    Str data; str_init(&data);
    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request, &available)) { *error_message = winhttp_error_text(GetLastError()); str_free(&data); goto done; }
        if (!available) break;
        str_reserve(&data, available);
        DWORD read = 0;
        if (!WinHttpReadData(request, data.data + data.len, available, &read)) { *error_message = winhttp_error_text(GetLastError()); str_free(&data); goto done; }
        if (!read) break;
        data.len += read; data.data[data.len] = 0;
        if (data.len > 64u * 1024 * 1024) { *error_message = xstrdup("The response is too large."); str_free(&data); goto done; }
    }
    *response_len = data.len;
    *response = str_detach(&data);
    ok = true;
done:
    free(wurl);
    if (request) WinHttpCloseHandle(request);
    if (connection) WinHttpCloseHandle(connection);
    if (own_session) WinHttpCloseHandle(own_session);
    return ok;
}
