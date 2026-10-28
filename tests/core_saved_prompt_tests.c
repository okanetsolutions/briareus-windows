#include "saved_prompt.h"
#include "api.h"
#include "models.h"
#include "str.h"
#include "test.h"
#include "suites.h"
#include <stdlib.h>
#include <string.h>

static void list_and_insert(void) {
    Json *answer = json_parsez("{\"prompts\":[{\"id\":7,\"title\":\"Zulu\",\"body\":\"global\",\"repo\":null,\"sortOrder\":2},{\"id\":8,\"title\":\"Beta\",\"body\":\"project\",\"repo\":\"o/r\",\"sortOrder\":1},{\"id\":9,\"title\":\"Alpha\",\"body\":\"shared\",\"sortOrder\":1},{\"id\":10,\"title\":\"Other\",\"body\":\"private\",\"repo\":\"o/other\"},{\"title\":\"No id\",\"body\":\"usable\"},{\"title\":\"Broken\"},{\"title\":\"\",\"body\":\"bad\"}]}");
    Json *rows = saved_prompt_list(answer, "o/r"); CHECK_INT(json_count(rows), 4);
    CHECK_STR(json_str(json_get(json_at(rows, 0), "title")), "Alpha");
    CHECK_STR(json_str(json_get(json_at(rows, 1), "title")), "Beta");
    CHECK_STR(json_str(json_get(json_at(rows, 2), "body")), "global");
    CHECK_STR(json_str(json_get(json_at(rows, 3), "body")), "usable");
    CHECK(saved_prompt_find(rows, 8) != NULL); CHECK(saved_prompt_find(rows, 10) == NULL); CHECK(saved_prompt_find(rows, 0) == NULL);
    json_free(rows); rows = saved_prompt_list(answer, NULL); CHECK_INT(json_count(rows), 5); json_free(rows); json_free(answer);
    answer = json_parsez("{\"prompts\":[]}"); rows = saved_prompt_list(answer, "o/r"); CHECK(rows != NULL); CHECK_INT(json_count(rows), 0); json_free(rows); json_free(answer);
    answer = json_parsez("{}"); CHECK(saved_prompt_list(answer, NULL) == NULL); json_free(answer);
    CHECK_OWNED_STR(saved_prompt_insert("", "line 1\nline 2"), "line 1\nline 2");
    CHECK_OWNED_STR(saved_prompt_insert(" \t\n\xC2\xA0", "body"), "body");
    CHECK_OWNED_STR(saved_prompt_insert("authored\n", "body"), "authored\n\n\nbody");
    CHECK_OWNED_STR(saved_prompt_insert("left middle right", "body"), "left middle right\n\nbody");
}
static char *repeated(const char *unit, size_t n) {
    Str s; str_init(&s); for (size_t i = 0; i < n; i++) str_appendz(&s, unit); return str_detach(&s);
}
static void limits_and_edit(void) {
    Json *p = saved_prompt_create("\xC2\xA0 Title \xE3\x80\x80", "\n\t\n  indented\n  inner  \n\t", "o/r");
    CHECK(p != NULL); CHECK_STR(json_str(json_get(p, "title")), "Title");
    CHECK_STR(json_str(json_get(p, "body")), "  indented\n  inner"); CHECK_STR(json_str(json_get(p, "repo")), "o/r"); json_free(p);
    CHECK(saved_prompt_create("\xEF\xBB\xBF", "body", NULL) == NULL);
    CHECK(saved_prompt_create("Title", "\xC2\xA0\n\t", NULL) == NULL);
    CHECK(saved_prompt_create("\xC0\xAF", "body", NULL) == NULL);
    CHECK(saved_prompt_create("Title", "\xED\xA0\x80", NULL) == NULL);
    CHECK(saved_prompt_create("Title", "\xF4\x90\x80\x80", NULL) == NULL);
    CHECK(saved_prompt_create("Title", "\xE2\x82", NULL) == NULL);
    char *title = repeated("\xF0\x9F\x93\x9A", 60), *body = repeated("\xF0\x9F\x93\x9A", 10000);
    p = saved_prompt_create(title, body, NULL); CHECK(p != NULL); CHECK(json_is_null(json_get(p, "repo"))); json_free(p);
    char *over = xstrfmt("%sx", title); CHECK(saved_prompt_create(over, "body", NULL) == NULL); free(over);
    over = xstrfmt("%sx", body); CHECK(saved_prompt_create("Title", over, NULL) == NULL); free(over); free(title); free(body);
    title = repeated("e\xCC\x81", 60); p = saved_prompt_create(title, "body", NULL); CHECK(p != NULL); json_free(p); free(title);
    body = repeated("x", 19999); over = xstrfmt("  %s", body); CHECK(saved_prompt_create("Title", over, NULL) == NULL); free(over); free(body);
    Json *original = json_parsez("{\"id\":41,\"title\":\"Original\",\"body\":\"  kept\\nbody\",\"repo\":\"o/r\",\"sortOrder\":9}");
    p = saved_prompt_edit(original, "Renamed", "  kept\nbody"); CHECK_INT(json_count(p), 2);
    CHECK_INT(json_int_or(json_get(p, "id"), 0), 41); CHECK_STR(json_str(json_get(p, "title")), "Renamed");
    CHECK(json_is_null(json_get(p, "body"))); CHECK(json_is_null(json_get(p, "repo"))); CHECK(json_is_null(json_get(p, "sortOrder"))); json_free(p);
    p = saved_prompt_edit(original, "Original", "new\nbody"); CHECK_INT(json_count(p), 2); CHECK(json_is_null(json_get(p, "title"))); json_free(p);
    p = saved_prompt_edit(original, " Original ", "  kept\nbody"); CHECK_INT(json_count(p), 0); json_free(p);
    CHECK(saved_prompt_edit(original, "", "body") == NULL); json_free(original);
}
typedef struct { int calls; char *method, *url; Json *body; bool fail; int status; } PromptWire;
static bool transport(void *ctx, const char *method, const char *url, const char *const *headers, const void *body, size_t bytes,
    int timeout, int *status, char **type, char **retry, char **response, size_t *len, char **error) {
    (void)headers; (void)timeout; PromptWire *w = ctx; w->calls++;
    free(w->method); free(w->url); json_free(w->body); w->method = xstrdup(method); w->url = xstrdup(url);
    char *text = xstrndup(body ? body : "", bytes); w->body = json_parsez(text); free(text);
    if (w->fail) { *error = xstrdup("Fixture timeout"); return false; }
    *status = w->status; *type = xstrdup("application/json"); *retry = xstrdup("11");
    *response = xstrdup("{\"ok\":true}"); *len = strlen(*response); return true;
}
static void wire_and_permissions(void) {
    PromptWire w = {0}; w.status = 200;
    ServerAddress address; ApiError error; api_error_init(&error); CHECK(server_address_parse("https://example.com", &address));
    ApiClient *client = api_client_new(&address, "brm_aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", &error); server_address_free(&address);
    api_client_set_transport(client, transport, &w);
    Json *args = json_parsez("{\"repo\":\"o/r\"}"); Json *r = api_call(client, "prompts", args, 0, &error); json_free(r); json_free(args);
    CHECK_STR(w.method, "GET"); CHECK_STR(w.url, "https://example.com/api/v1/prompts?repo=o%2Fr"); CHECK(w.body == NULL);
    args = saved_prompt_create("One", "two\nlines", "o/r"); json_set_str(args, "secret", "never forward");
    r = api_call(client, "create_prompt", args, 0, &error); json_free(r); json_free(args);
    CHECK_STR(w.method, "POST"); CHECK_STR(w.url, "https://example.com/api/v1/prompts"); CHECK_INT(json_count(w.body), 3);
    CHECK_STR(json_str(json_get(w.body, "body")), "two\nlines"); CHECK_STR(json_str(json_get(w.body, "repo")), "o/r");
    args = json_parsez("{\"id\":41,\"body\":\"updated\",\"repo\":null,\"sortOrder\":0}");
    r = api_call(client, "update_prompt", args, 0, &error); json_free(r); json_free(args);
    CHECK_STR(w.method, "PUT"); CHECK_STR(w.url, "https://example.com/api/v1/prompts/41"); CHECK_INT(json_count(w.body), 1);
    CHECK_STR(json_str(json_get(w.body, "body")), "updated"); CHECK(json_is_null(json_get(w.body, "repo")));
    args = json_parsez("{\"id\":41}"); w.fail = true;
    r = api_call(client, "delete_prompt", args, 0, &error); CHECK(r == NULL); CHECK_INT(w.calls, 4); CHECK_INT(error.kind, API_NETWORK);
    CHECK_STR(w.method, "DELETE"); CHECK_STR(w.url, "https://example.com/api/v1/prompts/41"); CHECK(w.body == NULL);
    w.fail = false; w.status = 429; r = api_call(client, "delete_prompt", args, 0, &error); CHECK(r == NULL); CHECK_INT(w.calls, 5); CHECK_INT(error.retry_after, 11); json_free(args);
    args = json_parsez("{\"title\":\"\",\"body\":\"bad\"}"); r = api_call(client, "create_prompt", args, 0, &error); CHECK(r == NULL); CHECK_INT(w.calls, 5); json_free(args);
    args = json_parsez("{\"id\":41,\"repo\":\"other\"}"); r = api_call(client, "update_prompt", args, 0, &error); CHECK(r == NULL); CHECK_INT(w.calls, 5); json_free(args);
    Route routes[] = {{"GET", "/prompts", "read"}, {"POST", "/prompts", "admin"}, {"PUT", "/prompts/{id}", "admin"}, {"DELETE", "/prompts/{id}", "admin"}};
    CHECK(routes_allow(routes, 4, "GET", "/prompts", "read")); CHECK(!routes_allow(routes, 4, "POST", "/prompts", "manage"));
    CHECK(!routes_allow(routes, 4, "PUT", "/prompts/{id}", "read")); CHECK(routes_allow(routes, 4, "DELETE", "/prompts/{id}", "admin"));
    CHECK(!routes_allow(routes, 1, "PUT", "/prompts/{id}", "admin"));
    char *repos[] = {"o/r"};
    CHECK(saved_prompt_scope_allowed("read", repos, 1, "o/r")); CHECK(!saved_prompt_scope_allowed("read", repos, 1, "o/other"));
    CHECK(!saved_prompt_scope_allowed("manage", repos, 1, NULL)); CHECK(saved_prompt_scope_allowed("admin", NULL, 0, NULL));
    CHECK(!saved_prompt_scope_allowed("unknown", repos, 1, "o/r"));
    api_error_clear(&error); api_client_release(client); free(w.method); free(w.url); json_free(w.body);
}
void core_saved_prompt_tests(void) {
    test_run("saved prompt scoped list ordering and draft insertion", list_and_insert);
    test_run("saved prompt server Unicode normalization limits and scope preserving edits", limits_and_edit);
    test_run("saved prompt wire routes whitelists permissions and no write retries", wire_and_permissions);
}
