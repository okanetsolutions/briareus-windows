#include "suites.h"
#define SECRET_HEAP_API
#include "secret_heap_sources.h"
#include "secret_heap_unmap.h"

static void observer_detects_moving_leak(void) {
    heap_reset_counts();
    Str s; heap_str_init(&s); heap_str_appendz(&s, heap_marker);
    heap_mark_sensitive();
    char tail[1024]; memset(tail, 'x', sizeof tail);
    heap_str_append(&s, tail, sizeof tail);
    CHECK(heap_moves > 0); CHECK(heap_unwiped > 0);
    heap_mark_sensitive(); heap_ssh_secret_free(s.data, s.len); heap_str_init(&s);
    CHECK_INT(heap_live, 0); heap_reset_counts();
}

static bool cleanup_transport(void *ctx, const char *method, const char *url, const char *const *headers,
    const void *body, size_t bytes, int timeout, int *status, char **type, char **retry,
    char **response, size_t *len, char **error) {
    (void)headers; (void)timeout;
    int *calls = ctx; (*calls)++;
    CHECK(str_eq(method, "PUT")); CHECK(str_has_suffix(url, "/settings/ssh/servers/7"));
    // The observer sees the final allocation before request() erases it; no copy of the wire body is made.
    CHECK(heap_contains(body, bytes)); heap_mark_sensitive();
    *status = 200; *type = heap_xstrdup("application/json"); *retry = NULL; *error = NULL;
    *response = heap_xstrdup("{\"server\":{\"id\":7,\"hasPassword\":true,\"hasSudoPassword\":true}}");
    *len = strlen(*response); return true;
}
static void api_serialization_erases_all_allocations(void) {
    for (int order = 0; order < 2; order++) {
        heap_reset_counts();
        Json *args = heap_json_object();
        // Controls, quotes, backslashes and multibyte bytes exercise different JSON expansion lengths.
        char second[1024];
        memset(second, 'x', 1023);
        size_t prefix = sizeof heap_marker - 1;
        memcpy(second, heap_marker, prefix);
        for (size_t i = prefix; i + 6 <= 1023; i += 6) memcpy(second + i, "\t\001\"\\\xc3\xa9", 6);
        second[1023] = 0;
        heap_json_set_num(args, "id", 7);
        if (order) heap_json_set_str(args, "sudoPassword", second);
        heap_json_set_str(args, "password", heap_marker);
        if (!order) heap_json_set_str(args, "sudoPassword", second);
        Json *extra = heap_json_array();
        heap_json_array_push(extra, heap_json_bool(false)); heap_json_array_push(extra, heap_json_number(1.25));
        heap_json_object_set(args, "escaped\"key\\", extra);
        ApiError error; heap_api_error_init(&error); ServerAddress address;
        CHECK(heap_server_address_parse("https://fixture.invalid", &address));
        ApiClient *client = heap_api_client_new(&address, "brm_aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", &error);
        heap_server_address_free(&address); CHECK(client != NULL);
        if (!client) abort();
        int calls = 0; heap_api_client_set_transport(client, cleanup_transport, &calls);
        size_t moves_before = heap_moves;
        Json *reply = heap_api_call(client, "update_ssh_server", args, 0, &error);
        CHECK(reply != NULL); CHECK_INT(calls, 1); CHECK_INT(error.kind, API_OK);
        CHECK_INT(heap_unwiped, 0); CHECK(heap_erased >= 2);
        // Object metadata may relocate, but a secret-bearing output must never be released dirty.
        CHECK(heap_moves >= moves_before);
        heap_json_free(reply); heap_api_client_release(client); heap_api_error_clear(&error);
        heap_mark_sensitive(); heap_ssh_secrets_scrub(args); heap_json_free(args);
        CHECK_INT(heap_unwiped, 0); CHECK_INT(heap_live, 0);
    }
}
static void rejected_path_erases_passwords(void) {
    const char *ids[] = {NULL, "null", "{}", "[]"};
    for (size_t i = 0; i < sizeof ids / sizeof *ids; i++) {
        heap_reset_counts();
        Json *args = heap_json_object();
        if (ids[i]) heap_json_object_set(args, "id", heap_json_parsez(ids[i]));
        heap_json_set_str(args, "password", heap_marker);
        heap_json_set_str(args, "sudoPassword", heap_marker);
        ApiError error; heap_api_error_init(&error);
        // Path rejection must happen before a client or transport is needed.
        Json *reply = heap_api_call(NULL, "update_ssh_server", args, 0, &error);
        CHECK(reply == NULL); CHECK_INT(error.kind, API_HTTP); CHECK_INT(error.status, 400);
        CHECK_INT(heap_unwiped, 0);
        CHECK_STR(heap_json_str(heap_json_get(args, "password")), heap_marker);
        CHECK_STR(heap_json_str(heap_json_get(args, "sudoPassword")), heap_marker);
        heap_ssh_secrets_scrub(args); heap_json_free(args); heap_api_error_clear(&error);
        CHECK_INT(heap_live, 0);
    }
}
static void bounded_output_matches_json(void) {
    const char *cases[] = {
        "null", "true", "false", "22", "-1.25", "1e20", "[]", "{}",
        "{\"z\":[null,true,false,0,-10,1.25,1e20],\"q\\\"\\\\\\u0001\":\"\\b\\f\\n\\r\\t\\u0002\\\"\\\\é\"}",
    };
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) for (int sorted = 0; sorted < 2; sorted++) {
        Json *v = heap_json_parsez(cases[i]); CHECK(v != NULL);
        char *expected = heap_json_serialize(v, sorted != 0); size_t bytes = strlen(expected);
        char *out = heap_json_serialize_bounded(v, sorted != 0, bytes); CHECK_STR(out, expected);
        CHECK(heap_json_serialize_bounded(v, sorted != 0, bytes - 1) == NULL);
        char *unlimited = heap_json_serialize_bounded(v, sorted != 0, SIZE_MAX); CHECK_STR(unlimited, expected);
        heap_free(out); heap_free(expected); heap_free(unlimited); heap_json_free(v);
    }
    CHECK(heap_json_serialize_bounded(NULL, false, 0) == NULL);
    char *text = heap_json_serialize_bounded(NULL, false, 4); CHECK_STR(text, "null"); heap_free(text);
    Json *number = heap_json_number(INFINITY); text = heap_json_serialize_bounded(number, false, 4);
    CHECK_STR(text, "null"); heap_free(text); heap_json_free(number); CHECK_INT(heap_live, 0);
}
void core_secret_cleanup_tests(void) {
    test_run("fixture observes dirty releases and always relocates growth", observer_detects_moving_leak);
    test_run("SSH API serialization erases final and intermediate allocations", api_serialization_erases_all_allocations);
    test_run("SSH invalid update path erases cloned passwords", rejected_path_erases_passwords);
    test_run("bounded JSON output handles exact limits escaping types and order", bounded_output_matches_json);
}
