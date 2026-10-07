// Fixtures pinned to nadinyamaui/briareus PR #124, 8585d12e20e57097bae291659765f9cfe9e14691.
#include "mcp.h"
#include "str.h"
#include "suites.h"
#include "test.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

static Json *http_form(void) {
    return json_parsez("{\"name\":\" tools_1-2 \",\"label\":\" Tools \",\"transport\":\"http\",\"url\":\"https://mcp.example/tools\",\"command\":\"ignored\",\"args\":[\"unused\"],\"repos\":[],\"enabled\":true,\"oauthRedirect\":\"callback\",\"status\":\"ready\",\"id\":1791403200000,\"signedIn\":true,\"signInUrl\":\"https://auth.example/?state=private\",\"headerNames\":[\"Authorization\"],\"envNames\":[\"TOKEN\"],\"headers\":{\"Authorization\":\"private\"},\"env\":{\"TOKEN\":\"private\"},\"oauthClientSecret\":\" private \"}");
}
static void rejects(Json *form, McpSecretMode h, McpSecretMode e, McpSecretMode c) {
    char *why = NULL; Json *body = mcp_form_body(form, h, e, c, &why);
    CHECK(body == NULL); CHECK(why != NULL); CHECK(why && !strstr(why, "private")); json_free(body); free(why);
}
static void test_secret_preservation_and_replacement(void) {
    Json *form = http_form(); char *why = NULL;
    Json *body = mcp_form_body(form, MCP_KEEP, MCP_KEEP, MCP_KEEP, &why);
    CHECK(body != NULL); CHECK(why == NULL);
    CHECK_STR(json_str(json_get(body, "name")), "tools_1-2"); CHECK_STR(json_str(json_get(body, "label")), "Tools");
    CHECK_STR(json_str(json_get(body, "command")), ""); CHECK_INT(json_count(json_get(body, "args")), 0);
    const char *const omitted[] = { "headers", "env", "oauthClientSecret", "headerNames", "envNames", "status", "id", "signedIn", "signInUrl" };
    for (size_t i = 0; i < sizeof omitted / sizeof *omitted; i++) CHECK(json_is_null(json_get(body, omitted[i])));
    char *wire = json_serialize(body, false); CHECK(!strstr(wire, "private")); free(wire); json_free(body);
    body = mcp_form_body(form, MCP_REPLACE, MCP_REPLACE, MCP_REPLACE, &why);
    CHECK(body != NULL); CHECK_STR(json_str(json_get(json_get(body, "headers"), "Authorization")), "private");
    CHECK_STR(json_str(json_get(json_get(body, "env"), "TOKEN")), "private"); CHECK_STR(json_str(json_get(body, "oauthClientSecret")), " private ");
    json_free(body);
    body = mcp_form_body(form, MCP_CLEAR, MCP_CLEAR, MCP_CLEAR, &why);
    CHECK(json_is_object(json_get(body, "headers"))); CHECK_INT(json_count(json_get(body, "headers")), 0);
    CHECK_INT(json_count(json_get(body, "env")), 0); CHECK_STR(json_str(json_get(body, "oauthClientSecret")), ""); json_free(body);
    json_object_set(form, "headers", json_null()); rejects(form, MCP_REPLACE, MCP_KEEP, MCP_KEEP);
    json_set_str(form, "oauthClientSecret", "private\n"); rejects(form, MCP_KEEP, MCP_KEEP, MCP_REPLACE);
    rejects(form, (McpSecretMode)9, MCP_KEEP, MCP_KEEP);
    json_free(form);
}
static void test_stdio_and_repository_assignments(void) {
    Json *form = http_form(); json_set_str(form, "transport", "stdio"); json_set_str(form, "command", " /opt/mcp ");
    Json *args = json_parsez("[\"--token\",\" value with spaces \",\"\",\"a\\nb\"]"); json_object_set(form, "args", args);
    json_object_set(form, "repos", json_parsez("[\"owner/project\",\"other/repo\",\"owner/project\"]")); json_set_bool(form, "enabled", false);
    char *why = NULL; Json *body = mcp_form_body(form, MCP_KEEP, MCP_REPLACE, MCP_KEEP, &why);
    CHECK(body != NULL); CHECK_STR(json_str(json_get(body, "url")), ""); CHECK_STR(json_str(json_get(body, "command")), "/opt/mcp");
    CHECK(json_equal(json_get(body, "args"), args)); CHECK_INT(json_count(json_get(body, "repos")), 2); CHECK(json_bool_is(json_get(body, "enabled"), false));
    json_free(body);
    json_set_str(form, "command", " "); rejects(form, MCP_KEEP, MCP_KEEP, MCP_KEEP);
    json_set_str(form, "command", "mcp"); json_object_set(form, "repos", json_array());
    body = mcp_form_body(form, MCP_KEEP, MCP_KEEP, MCP_KEEP, &why); CHECK_INT(json_count(json_get(body, "repos")), 0); json_free(body);
    json_object_set(form, "repos", json_parsez("[\"owner//repo\"]")); rejects(form, MCP_KEEP, MCP_KEEP, MCP_KEEP);
    json_object_set(form, "repos", json_null()); rejects(form, MCP_KEEP, MCP_KEEP, MCP_KEEP);
    json_free(form);
}
static void test_invalid_fields_and_limits(void) {
    Json *form = http_form();
    const char *const names[] = { "", "a b", "reviewer_memory", "reviewer_ssh", "reviewer_slack", "reviewer_workers", "browser", "pri\nvate" };
    for (size_t i = 0; i < sizeof names / sizeof *names; i++) { json_set_str(form, "name", names[i]); rejects(form, MCP_KEEP, MCP_KEEP, MCP_KEEP); }
    json_set_str(form, "name", "ok");
    char long_text[8200]; memset(long_text, 'a', sizeof long_text - 1); long_text[sizeof long_text - 1] = 0;
    json_set_str(form, "label", long_text); rejects(form, MCP_KEEP, MCP_KEEP, MCP_KEEP); json_set_str(form, "label", "ok");
    json_set_str(form, "transport", "ssh"); rejects(form, MCP_KEEP, MCP_KEEP, MCP_KEEP); json_set_str(form, "transport", "http");
    json_set_str(form, "url", "http://remote.example/mcp"); rejects(form, MCP_KEEP, MCP_KEEP, MCP_KEEP); json_set_str(form, "url", "https://mcp.example");
    json_set_str(form, "oauthRedirect", "bad"); rejects(form, MCP_KEEP, MCP_KEEP, MCP_KEEP); json_set_str(form, "oauthRedirect", "loopback");
    json_set_str(form, "enabled", "true"); rejects(form, MCP_KEEP, MCP_KEEP, MCP_KEEP); json_set_bool(form, "enabled", true);
    json_set_str(form, "transport", "stdio"); json_set_str(form, "command", "mcp");
    json_object_set(form, "args", json_object()); rejects(form, MCP_KEEP, MCP_KEEP, MCP_KEEP);
    Json *args = json_array(); for (int i = 0; i < 65; i++) json_array_push(args, json_string("")); json_object_set(form, "args", args); rejects(form, MCP_KEEP, MCP_KEEP, MCP_KEEP);
    json_object_set(form, "args", json_parsez("[1]")); rejects(form, MCP_KEEP, MCP_KEEP, MCP_KEEP);
    args = json_array(); json_array_push(args, json_string(long_text)); json_object_set(form, "args", args); rejects(form, MCP_KEEP, MCP_KEEP, MCP_KEEP); json_object_set(form, "args", json_array());
    const char *const bad_maps[] = { "[]", "{\"bad name\":\"private\"}", "{\"valid\":1}", "{\"valid\":\"private\\r\\n\"}" };
    for (size_t i = 0; i < sizeof bad_maps / sizeof *bad_maps; i++) { json_object_set(form, "headers", json_parsez(bad_maps[i])); rejects(form, MCP_REPLACE, MCP_KEEP, MCP_KEEP); }
    Json *map = json_object(); json_set_str(map, "x", long_text); json_object_set(form, "headers", map); rejects(form, MCP_REPLACE, MCP_KEEP, MCP_KEEP);
    map = json_object(); for (int i = 0; i < 33; i++) { char key[8]; snprintf(key, sizeof key, "K%d", i); json_set_str(map, key, "private"); } json_object_set(form, "env", map); rejects(form, MCP_KEEP, MCP_REPLACE, MCP_KEEP);
    const char *const invalid_env[] = { "", "1TOKEN", "a-b" };
    for (size_t i = 0; i < sizeof invalid_env / sizeof *invalid_env; i++) { map = json_object(); json_set_str(map, invalid_env[i], "private"); json_object_set(form, "env", map); rejects(form, MCP_KEEP, MCP_REPLACE, MCP_KEEP); }
    map = json_object(); json_set_str(map, "", "private"); json_object_set(form, "headers", map); rejects(form, MCP_REPLACE, MCP_KEEP, MCP_KEEP);
    map = json_object(); json_set_str(map, "!#$%&'*+.^_`|~-", "private"); json_object_set(form, "headers", map);
    Json *body = mcp_form_body(form, MCP_REPLACE, MCP_CLEAR, MCP_CLEAR, NULL); CHECK(body != NULL); json_free(body);
    json_free(form);
}
static char *repeat_utf8(const char *character, size_t count) {
    size_t bytes = strlen(character);
    char *text = xmalloc(bytes * count + 1);
    for (size_t i = 0; i < count; i++) memcpy(text + i * bytes, character, bytes);
    text[bytes * count] = 0; return text;
}
static void test_utf16_limits(void) {
    Json *form = http_form();
    const struct { const char *key; size_t units; McpSecretMode secret; } fields[] = {
        { "label", 200, MCP_KEEP }, { "command", 1024, MCP_KEEP }, { "oauthClientId", 256, MCP_KEEP },
        { "oauthScope", 1024, MCP_KEEP }, { "oauthClientName", 100, MCP_KEEP }, { "oauthClientSecret", 1024, MCP_REPLACE },
    };
    json_set_str(form, "transport", "stdio"); json_set_str(form, "command", "mcp");
    const char *const characters[] = { "\xe4\xb8\xad", "\xf0\x9f\x98\x80" };
    for (size_t c = 0; c < 2; c++) {
        size_t units = c ? 2 : 1;
        for (size_t f = 0; f < sizeof fields / sizeof *fields; f++) {
            char *text = repeat_utf8(characters[c], fields[f].units / units);
            json_set_str(form, fields[f].key, text); free(text);
            Json *body = mcp_form_body(form, MCP_KEEP, MCP_KEEP, fields[f].secret, NULL); CHECK(body != NULL); json_free(body);
            text = repeat_utf8(characters[c], fields[f].units / units + 1);
            json_set_str(form, fields[f].key, text); free(text); rejects(form, MCP_KEEP, MCP_KEEP, fields[f].secret);
            json_set_str(form, fields[f].key, "ok");
        }
        for (size_t extra = 0; extra < 2; extra++) {
            char *arg = repeat_utf8(characters[c], 4096 / units + extra), *secret = repeat_utf8(characters[c], 8192 / units + extra);
            Json *args = json_array(), *map = json_object(); json_array_push(args, json_string(arg));
            json_object_set(form, "args", args); json_set_str(map, "TOKEN", secret); json_object_set(form, "env", map);
            if (extra) {
                rejects(form, MCP_KEEP, MCP_KEEP, MCP_KEEP);
                json_object_set(form, "args", json_array()); rejects(form, MCP_KEEP, MCP_REPLACE, MCP_KEEP);
                map = json_object(); json_set_str(map, "Authorization", secret); json_object_set(form, "headers", map);
                rejects(form, MCP_REPLACE, MCP_KEEP, MCP_KEEP);
            } else {
                Json *body = mcp_form_body(form, MCP_KEEP, MCP_REPLACE, MCP_KEEP, NULL); CHECK(body != NULL); json_free(body);
                map = json_object(); json_set_str(map, "Authorization", secret); json_object_set(form, "headers", map);
                body = mcp_form_body(form, MCP_REPLACE, MCP_KEEP, MCP_KEEP, NULL); CHECK(body != NULL); json_free(body);
            }
            free(arg); free(secret);
        }
    }
    json_free(form);
}
static void test_inactive_transport_fields(void) {
    Json *form = http_form();
    const char *const invalid_args[] = { "[", "[1]", "{}" };
    for (size_t i = 0; i < sizeof invalid_args / sizeof *invalid_args; i++) {
        json_object_set(form, "args", json_parsez(invalid_args[i]));
        Json *body = mcp_form_body(form, MCP_KEEP, MCP_KEEP, MCP_KEEP, NULL);
        CHECK(body != NULL); CHECK_INT(json_count(json_get(body, "args")), 0); json_free(body);
        json_set_str(form, "transport", "stdio"); rejects(form, MCP_KEEP, MCP_KEEP, MCP_KEEP); json_set_str(form, "transport", "http");
    }
    char *text = repeat_utf8("x", 2049); json_set_str(form, "command", text);
    Json *body = mcp_form_body(form, MCP_KEEP, MCP_KEEP, MCP_KEEP, NULL); CHECK(body != NULL); json_free(body);
    json_set_str(form, "transport", "stdio"); json_set_str(form, "command", "mcp"); json_set_str(form, "url", text); free(text);
    json_object_set(form, "args", json_array());
    body = mcp_form_body(form, MCP_KEEP, MCP_KEEP, MCP_KEEP, NULL); CHECK(body != NULL); CHECK_STR(json_str(json_get(body, "url")), ""); json_free(body);
    json_set_str(form, "transport", "http"); rejects(form, MCP_KEEP, MCP_KEEP, MCP_KEEP);
    json_free(form);
}
static void test_urls_and_large_ids(void) {
    const char *const good[] = { "https://mcp.example/tools", "https://mcp.example:443/a", "http://localhost/a", "http://127.0.0.1:3000/mcp", "http://[::1]:3000/a" };
    for (size_t i = 0; i < sizeof good / sizeof *good; i++) CHECK(mcp_secure_url(good[i]));
    const char *const bad[] = { NULL, "", "file:///a", "http://remote.example", "https://", "https:///a", "https://:443", "https://user:pw@host", "https://host\\evil/a", "https://host name/a", "http://localhost.evil", "https://[::1", "https://[::1]x/a", "https://host:", "https://host:abc", "https://host:0", "https://host:65536", "https://host\n" };
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) CHECK(!mcp_secure_url(bad[i]));
    CHECK(mcp_callback_url("http://127.0.0.1:3000/callback?code=a%20b&state=s"));
    CHECK(mcp_callback_url("http://localhost/callback?error=access_denied&state=s"));
    CHECK(!mcp_callback_url("http://localhost/?state=s")); CHECK(!mcp_callback_url("http://localhost/?state=&code=c"));
    CHECK(mcp_callback_url("https://provider.example/callback?code=c&state=s")); CHECK(mcp_callback_url("http://localhost/?code=c&state=s#_=_"));
    CHECK(!mcp_callback_url("http://localhost/?state=s#x&code=c")); CHECK(!mcp_callback_url("https://provider.example/#?state=s&code=c"));
    CHECK(!mcp_callback_url(NULL)); CHECK(!mcp_callback_url("http://localhost/?code=&state=s")); CHECK(!mcp_callback_url("http://localhost/?state=#x&code=c"));
    Json *row = json_object(); CHECK(mcp_server_id(row) == 0); json_set_num(row, "id", 1791403200000.0); CHECK(mcp_server_id(row) == 1791403200000.0);
    const double ids[] = { -1, 0, 1.5, INFINITY, NAN, 9007199254740992.0 };
    for (size_t i = 0; i < sizeof ids / sizeof *ids; i++) { json_set_num(row, "id", ids[i]); CHECK(mcp_server_id(row) == 0); } json_free(row);
}
static void test_oauth_expiry_failure_and_invalidation(void) {
    McpSignIn state = {0}; Json *row = json_parsez("{\"status\":\"needs-sign-in\",\"signInUrl\":\"https://auth.example/?state=one\",\"signInNeedsPaste\":false}");
    mcp_sign_in_update(&state, row); CHECK(state.url != NULL); CHECK(!mcp_sign_in_can_finish(&state));
    json_set_bool(row, "signInNeedsPaste", true); mcp_sign_in_update(&state, row); CHECK(mcp_sign_in_can_finish(&state));
    mcp_sign_in_failed(&state); mcp_sign_in_update(&state, row); CHECK(!mcp_sign_in_can_finish(&state));
    // A repeated read does not start a new lifetime or unblock a consumed callback.
    mcp_sign_in_update(&state, row); CHECK(state.blocked);
    json_set_str(row, "signInUrl", "https://auth.example/?state=two"); mcp_sign_in_update(&state, row); CHECK(mcp_sign_in_can_finish(&state));
    // Core expires pending URLs at 15 minutes; needs-sign-in can persist afterwards.
    json_object_set(row, "signInUrl", json_null()); mcp_sign_in_update(&state, row); CHECK(state.url == NULL); CHECK(!mcp_sign_in_can_finish(&state));
    json_set_str(row, "status", "ready"); mcp_sign_in_update(&state, row); CHECK(state.url == NULL);
    json_set_str(row, "signInUrl", "javascript:private"); mcp_sign_in_update(&state, row); CHECK(state.url == NULL);
    json_set_str(row, "signInUrl", "https://auth.example/?state=three"); mcp_sign_in_update(&state, row); mcp_sign_in_clear(&state); CHECK(!state.url && !state.paste && !state.blocked);
    json_free(row);
}
void mcp_tests(void) {
    test_run("MCP secrets are omitted, replaced or explicitly cleared", test_secret_preservation_and_replacement);
    test_run("MCP stdio arguments and repository assignments", test_stdio_and_repository_assignments);
    test_run("MCP validation rejects invalid fields without revealing secrets", test_invalid_fields_and_limits);
    test_run("MCP string limits count UTF-16 units", test_utf16_limits);
    test_run("MCP ignores inactive transport fields", test_inactive_transport_fields);
    test_run("MCP URL validation and millisecond ids", test_urls_and_large_ids);
    test_run("MCP sign-in lifetime follows core; failed callbacks stay blocked", test_oauth_expiry_failure_and_invalidation);
}
