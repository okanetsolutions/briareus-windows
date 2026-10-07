#include "mcp.h"
#include "str.h"
#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

static bool ascii_word(unsigned char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'; }
static bool name_ok(const char *s, bool env) {
    size_t n = s ? strlen(s) : 0;
    if (!n || n > (env ? 128u : 64u)) return false;
    if (env && !(isalpha((unsigned char)*s) || *s == '_')) return false;
    for (; *s; s++) if (!ascii_word((unsigned char)*s) && (env || *s != '-')) return false;
    return true;
}
static bool repo_ok(const char *s) {
    const char *slash = s ? strchr(s, '/') : NULL;
    if (!slash || slash == s || !slash[1]) return false;
    for (const char *p = s; *p; p++) if (p != slash && !ascii_word((unsigned char)*p) && *p != '.' && *p != '-') return false;
    return true;
}
static bool utf16_fits(const char *s, size_t limit) {
    if (!s) return false;
    size_t units = 0;
    // UTF-8 continuation bytes add no units; supplementary characters use a surrogate pair in core's JS strings.
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if ((*p & 0xc0) == 0x80) continue;
        units += *p >= 0xf0 ? 2u : 1u;
        if (units > limit) return false;
    }
    return true;
}
static bool text_ok(const char *s, size_t limit) {
    if (!utf16_fits(s, limit)) return false;
    for (; *s; s++) if ((unsigned char)*s < 32) return false;
    return true;
}
bool mcp_secure_url(const char *url) {
    if (!url || !text_ok(url, 8192)) return false;
    bool https = str_has_prefix(url, "https://"), http = str_has_prefix(url, "http://");
    if (!https && !http) return false;
    const char *host = url + (https ? 8 : 7), *end = host + strcspn(host, "/?#");
    if (end == host) return false;
    for (const char *p = host; p < end; p++) if (*p == '@' || *p == '\\' || isspace((unsigned char)*p)) return false;
    const char *port = host;
    if (*host == '[') { port = strchr(host, ']'); if (!port || port >= end) return false; port++; }
    else while (port < end && *port != ':') port++;
    if (port == host) return false;
    if (port < end) {
        if (*port != ':' || port + 1 == end) return false;
        unsigned long n = 0;
        for (const char *p = port + 1; p < end; p++) { if (*p < '0' || *p > '9') return false; n = n * 10 + (unsigned)(*p - '0'); if (n > 65535) return false; }
        if (!n) return false;
    }
    if (https) return true;
    size_t len = (size_t)(port - host);
    return (len == 9 && !strncmp(host, "localhost", len)) || (len == 9 && !strncmp(host, "127.0.0.1", len)) || (len == 5 && !strncmp(host, "[::1]", len));
}
static bool query_has(const char *q, const char *key) {
    size_t n = strlen(key);
    const char *end = q + strcspn(q, "#");
    for (const char *p = q; p && p < end; ) {
        if ((size_t)(end - p) <= n + 1) return false;
        if (!strncmp(p, key, n) && p[n] == '=' && p[n + 1] && p[n + 1] != '&' && p[n + 1] != '#') return true;
        p = strchr(p, '&'); if (p) p++;
    }
    return false;
}
bool mcp_callback_url(const char *url) {
    if (!mcp_secure_url(url)) return false;
    const char *q = strchr(url, '?');
    const char *fragment = strchr(url, '#');
    return q && (!fragment || q < fragment) && query_has(q + 1, "state") && (query_has(q + 1, "code") || query_has(q + 1, "error"));
}
double mcp_server_id(const Json *row) {
    double id = json_num_or(json_get(row, "id"), 0);
    return isfinite(id) && id > 0 && id <= 9007199254740991.0 && floor(id) == id ? id : 0;
}
static Json *fail(Json *body, char **error, const char *message) {
    if (error) *error = xstrdup(message);
    json_free(body); return NULL;
}
static bool secret_map_ok(const Json *map, bool env) {
    if (!json_is_object(map) || json_count(map) > 32) return false;
    for (size_t i = 0; i < json_count(map); i++) {
        const char *key = json_key(map, i), *val = json_str(json_get(map, key));
        if (env) { if (!name_ok(key, true)) return false; }
        else {
            if (!*key || strlen(key) > 128) return false;
            for (const char *p = key; *p; p++) if (!ascii_word((unsigned char)*p) && !strchr("!#$%&'*+.^`|~-", *p)) return false;
        }
        if (!utf16_fits(val, 8192) || strchr(val, '\r') || strchr(val, '\n')) return false;
    }
    return true;
}
Json *mcp_form_body(const Json *fields, McpSecretMode headers, McpSecretMode env, McpSecretMode client_secret, char **error) {
    if (error) *error = NULL;
    Json *body = json_object();
    char *transport = str_trim(json_str_or(json_get(fields, "transport"), ""));
    bool stdio = str_eq(transport, "stdio"), http = str_eq(transport, "http");
    free(transport);
    if (!stdio && !http) return fail(body, error, "Choose HTTP or stdio.");
    static const struct { const char *key; size_t max; } texts[] = {
        { "name", 64 }, { "label", 200 }, { "transport", 5 }, { "url", 2048 }, { "command", 1024 },
        { "oauthClientId", 256 }, { "oauthScope", 1024 }, { "oauthClientName", 100 }, { "oauthRedirect", 8 },
    };
    for (size_t i = 0; i < sizeof texts / sizeof *texts; i++) {
        bool inactive = (stdio && str_eq(texts[i].key, "url")) || (http && str_eq(texts[i].key, "command"));
        char *s = str_trim(inactive ? "" : json_str_or(json_get(fields, texts[i].key), ""));
        if (!text_ok(s, texts[i].max)) { free(s); return fail(body, error, "A text field is too long or contains control characters."); }
        json_set_str(body, texts[i].key, s); free(s);
    }
    const char *name = json_str(json_get(body, "name"));
    static const char *const reserved[] = { "reviewer_memory", "reviewer_ssh", "reviewer_slack", "reviewer_workers", "browser" };
    if (!name_ok(name, false)) return fail(body, error, "Use 1 to 64 letters, digits, underscores or hyphens for the tool name.");
    for (size_t i = 0; i < sizeof reserved / sizeof *reserved; i++) if (str_eq(name, reserved[i])) return fail(body, error, "That tool name is reserved by Briareus.");
    const Json *args = json_get(fields, "args"), *repos = json_get(fields, "repos");
    if (stdio) {
        if (!json_is_array(args) || json_count(args) > 64) return fail(body, error, "Arguments must be a JSON array of up to 64 strings.");
        for (size_t i = 0; i < json_count(args); i++) if (!utf16_fits(json_str(json_at(args, i)), 4096)) return fail(body, error, "Each argument must be a string of up to 4096 UTF-16 units.");
    }
    if (!json_is_array(repos)) return fail(body, error, "Choose all repositories or selected repositories.");
    Json *unique = json_array();
    for (size_t i = 0; i < json_count(repos); i++) {
        const char *r = json_str(json_at(repos, i));
        if (!repo_ok(r)) { json_free(unique); return fail(body, error, "Repositories must be written as owner/name."); }
        bool found = false;
        for (size_t k = 0; k < json_count(unique); k++) if (str_eq(r, json_str(json_at(unique, k)))) found = true;
        if (!found) json_array_push(unique, json_string(r));
    }
    json_object_set(body, "repos", unique);
    if (json_bool_tristate(json_get(fields, "enabled")) < 0) return fail(body, error, "Enabled must be a boolean.");
    json_object_set(body, "enabled", json_clone(json_get(fields, "enabled")));
    if (http) {
        if (!mcp_secure_url(json_str(json_get(body, "url")))) return fail(body, error, "Enter an HTTPS endpoint, or HTTP on core's loopback host.");
        json_set_str(body, "command", ""); json_object_set(body, "args", json_array());
    } else {
        if (str_empty(json_str(json_get(body, "command")))) return fail(body, error, "Enter a command to run on the core machine.");
        json_set_str(body, "url", ""); json_object_set(body, "args", json_clone(args));
    }
    const char *redirect = json_str(json_get(body, "oauthRedirect"));
    if (!str_eq(redirect, "callback") && !str_eq(redirect, "loopback")) return fail(body, error, "Choose callback or loopback OAuth.");
    const McpSecretMode modes[] = { headers, env, client_secret };
    const char *const keys[] = { "headers", "env", "oauthClientSecret" };
    for (size_t i = 0; i < 3; i++) {
        if (modes[i] == MCP_KEEP) continue;
        if (modes[i] != MCP_CLEAR && modes[i] != MCP_REPLACE) return fail(body, error, "Choose keep, replace or clear for secrets.");
        if (modes[i] == MCP_CLEAR) json_object_set(body, keys[i], i == 2 ? json_string("") : json_object());
        else {
            const Json *v = json_get(fields, keys[i]);
            if (i == 2 ? !text_ok(json_str(v), 1024) : !secret_map_ok(v, i == 1)) return fail(body, error, "Invalid secret fields: use string values and valid names; values are never shown in diagnostics.");
            json_object_set(body, keys[i], json_clone(v));
        }
    }
    return body;
}
void mcp_sign_in_clear(McpSignIn *state) { free(state->url); memset(state, 0, sizeof *state); }
void mcp_sign_in_update(McpSignIn *state, const Json *row) {
    const char *url = json_str_nonempty(json_get(row, "signInUrl"));
    if (url && !mcp_secure_url(url)) url = NULL;
    if (!str_eq(state->url, url)) { mcp_sign_in_clear(state); state->url = xstrdup(url); }
    state->paste = url && json_bool_is(json_get(row, "signInNeedsPaste"), true);
}
void mcp_sign_in_failed(McpSignIn *state) { state->blocked = true; }
bool mcp_sign_in_can_finish(const McpSignIn *state) { return state->url && state->paste && !state->blocked; }
