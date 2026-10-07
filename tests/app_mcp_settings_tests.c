// Exercise the real form controls and response handlers with rendering, dialogs and requests isolated.
#include "screens.h"
#include "suites.h"
#include "test.h"
#include <stdlib.h>
#include <string.h>

static void test_pane_changed(Pane *pane) { (void)pane; }
static void test_selected_id(Pane *pane, const char *id) { (void)pane; (void)id; }
static Pane *test_sidebar(void) { return NULL; }
static int registry_changes, detail_clears;
static void test_registry_changed(void) { registry_changes++; }
static void test_clear_detail(void) { detail_clears++; }
static bool confirm_result;
static int confirmations, calls;
static Json *sent;
static Request pending;
static bool test_confirm(const char *title, const char *message, const char *label, bool destructive) {
    (void)title; (void)message; (void)label; (void)destructive; confirmations++; return confirm_result;
}
static Request *test_call(const char *operation, Json *args, int timeout_ms, void *owner, RequestDone done, int tag, Request **slot) {
    (void)operation; (void)timeout_ms;
    calls++; json_free(sent); sent = args; memset(&pending, 0, sizeof pending);
    pending.client = g_store.client; pending.owner = owner; pending.done = done; pending.tag = tag; pending.slot = slot;
    if (slot) *slot = &pending;
    return &pending;
}
// Compile the screen's static form logic here without adding a production test API.
bool test_mcp_supported(const char *operation);
Screen *test_mcp_screen_new(const Json *row, const Json *defaults);
#define mcp_settings_supported test_mcp_supported
#define mcp_settings_screen_new test_mcp_screen_new
#define pane_relayout test_pane_changed
#define pane_header_changed test_pane_changed
#define pane_set_selected_id test_selected_id
#define app_sidebar_pane test_sidebar
#define settings_mcp_changed test_registry_changed
#define app_clear_detail test_clear_detail
#define app_confirm test_confirm
#define store_call test_call
#include "../app/screen_mcp_settings.c"
#undef mcp_settings_supported
#undef mcp_settings_screen_new
#undef pane_relayout
#undef pane_header_changed
#undef pane_set_selected_id
#undef app_sidebar_pane
#undef settings_mcp_changed
#undef app_clear_detail
#undef app_confirm
#undef store_call

static Route routes[] = {
    { "GET", "/settings/mcp/servers", "admin" },
    { "POST", "/settings/mcp/servers", "admin" },
    { "PUT", "/settings/mcp/servers/{id}", "admin" },
    { "DELETE", "/settings/mcp/servers/{id}", "admin" },
};
static void form_fixture(McpForm *s, bool existing) {
    memset(&g_store, 0, sizeof g_store);
    g_store.has_device = true; g_store.device.permission = "admin";
    g_store.routes = routes; g_store.route_count = sizeof routes / sizeof *routes;
    // Identity only: the request mock never dereferences this client.
    g_store.client = (ApiClient *)&pending;
    memset(s, 0, sizeof *s); s->account = g_store.client; s->shown = true;
    s->row = json_parsez("{\"name\":\"tools\",\"label\":\"Original\",\"transport\":\"http\",\"url\":\"https://old.example\",\"command\":\"\",\"args\":[],\"repos\":[\"owner/deleted\"],\"enabled\":true,\"oauthRedirect\":\"callback\"}");
    if (existing) { json_set_num(s->row, "id", 123); s->id = 123; }
    for (int f = 0; f < F_COUNT; f++) {
        s->edits[f] = CreateWindowExW(0, L"EDIT", L"", WS_POPUP | ES_MULTILINE, 0, 0, 10, 10, NULL, NULL, GetModuleHandleW(NULL), NULL);
        CHECK(s->edits[f] != NULL);
    }
    fill(s); calls = confirmations = registry_changes = detail_clears = 0; confirm_result = false;
}
static void form_cleanup(McpForm *s) {
    for (int f = 0; f < F_COUNT; f++) DestroyWindow(s->edits[f]);
    json_free(s->row); json_free(s->repos); free(s->base.id); free(s->error); free(s->create_name); mcp_sign_in_clear(&s->sign_in);
    json_free(sent); sent = NULL; memset(&g_store, 0, sizeof g_store);
}
static void test_assignments_and_disabled_empty_selection(void) {
    McpForm s; form_fixture(&s, true); char *why = NULL;
    set_text(s.edits[F_LABEL], "Renamed"); s.dirty = true;
    Json *body = body_now(&s, &why); CHECK(body != NULL); CHECK(why == NULL); CHECK(json_is_null(json_get(body, "repos"))); json_free(body);
    s.enabled = false; body = body_now(&s, &why); CHECK(body != NULL); CHECK(json_is_null(json_get(body, "repos"))); json_free(body);
    toggle_repo(&s, "owner/deleted"); s.enabled = true;
    body = body_now(&s, &why); CHECK(body == NULL); CHECK(why != NULL); free(why); why = NULL;
    s.enabled = false; body = body_now(&s, &why); CHECK(body != NULL); CHECK(json_is_array(json_get(body, "repos"))); CHECK_INT(json_count(json_get(body, "repos")), 0); json_free(body);
    s.id = 0; body = body_now(&s, &why); CHECK(json_is_array(json_get(body, "repos"))); json_free(body);
    form_cleanup(&s);
}
static void test_clean_refresh_and_dirty_conflict(void) {
    McpForm s; form_fixture(&s, true);
    Json *next = json_clone(s.row); json_set_str(next, "url", "https://new.example"); json_set_bool(next, "enabled", false);
    json_object_set(next, "repos", json_parsez("[\"owner/new\"]")); json_set_str(next, "transport", "stdio"); json_set_str(next, "command", "mcp");
    status_update(&s, next);
    CHECK_OWNED_STR(edit_text(s.edits[F_URL]), "https://new.example"); CHECK(s.stdio); CHECK(!s.enabled); CHECK(!s.dirty); CHECK(json_equal(s.repos, json_get(next, "repos")));
    set_text(s.edits[F_LABEL], "Local label"); s.dirty = true;
    set_text(s.edits[F_CLIENT_SECRET], "private draft"); s.secrets[2] = MCP_REPLACE;
    json_set_str(next, "command", "new-command"); status_update(&s, next);
    CHECK(s.conflict); CHECK(s.dirty); CHECK_OWNED_STR(edit_text(s.edits[F_LABEL]), "Local label"); CHECK_OWNED_STR(edit_text(s.edits[F_COMMAND]), "mcp");
    CHECK_OWNED_STR(edit_text(s.edits[F_CLIENT_SECRET]), "private draft"); CHECK_INT(s.secrets[2], MCP_REPLACE);
    form_save(&s); CHECK_INT(confirmations, 1); CHECK_INT(calls, 0); CHECK(s.dirty);
    confirm_result = true; form_save(&s); CHECK_INT(calls, 1); CHECK_STR(json_str(json_get(sent, "command")), "mcp"); s.write = NULL;
    form_action(&s.base, ACT_RELOAD, 0, (POINT){0, 0});
    CHECK(!s.conflict); CHECK(!s.dirty); CHECK_OWNED_STR(edit_text(s.edits[F_COMMAND]), "new-command"); CHECK_OWNED_STR(edit_text(s.edits[F_CLIENT_SECRET]), "");
    // Status-only reads keep both a dirty draft and an in-progress callback intact.
    json_set_str(next, "signInUrl", "https://auth.example/?state=s"); json_set_bool(next, "signInNeedsPaste", true); status_update(&s, next);
    set_text(s.edits[F_CALLBACK], "https://provider.example/?state=s&code=c"); json_set_str(next, "status", "needs-sign-in"); status_update(&s, next);
    CHECK_OWNED_STR(edit_text(s.edits[F_CALLBACK]), "https://provider.example/?state=s&code=c");
    s.dirty = true; status_update(&s, next); CHECK(!s.conflict);
    json_free(next); form_cleanup(&s);
}
static void test_clean_save_preserves_pending_sign_in(void) {
    McpForm s; form_fixture(&s, true);
    json_set_str(s.row, "signInUrl", "https://auth.example/?state=pending"); json_set_bool(s.row, "signInNeedsPaste", true);
    mcp_sign_in_update(&s.sign_in, s.row);
    set_text(s.edits[F_CALLBACK], "http://localhost/?state=pending&code=approved");
    HeaderInfo header = {0}; form_header(&s.base, &header); CHECK(!header.buttons[0].enabled);
    form_save(&s); CHECK_INT(calls, 0); CHECK(s.write == NULL);
    CHECK_STR(s.sign_in.url, "https://auth.example/?state=pending"); CHECK(mcp_sign_in_can_finish(&s.sign_in));
    CHECK_OWNED_STR(edit_text(s.edits[F_CALLBACK]), "http://localhost/?state=pending&code=approved");
    set_text(s.edits[F_LABEL], "Renamed"); changed(&s); form_save(&s); CHECK_INT(calls, 1); s.write = NULL;
    form_cleanup(&s);
    form_fixture(&s, false); CHECK(!s.dirty); form_save(&s); CHECK_INT(calls, 1); s.write = NULL; form_cleanup(&s);
}
static void test_normalized_endpoint_partial_updates(void) {
    McpForm s; form_fixture(&s, true); char *why = NULL;
    Str endpoint; str_init(&endpoint); str_appendz(&endpoint, "https://mcp.example/");
    for (int i = 0; i < 300; i++) str_appendz(&endpoint, "%E4%B8%AD");
    CHECK(endpoint.len > 2048); json_set_str(s.row, "url", endpoint.data); fill(&s);
    set_text(s.edits[F_LABEL], "Renamed"); s.enabled = false; changed(&s);
    Json *body = body_now(&s, &why); CHECK(body != NULL); CHECK(why == NULL);
    CHECK(json_is_null(json_get(body, "url"))); CHECK_STR(json_str(json_get(body, "label")), "Renamed");
    CHECK(json_bool_is(json_get(body, "enabled"), false)); json_free(body);
    form_save(&s); CHECK_INT(calls, 1); CHECK(json_is_null(json_get(sent, "url"))); s.write = NULL;
    // A changed overlong endpoint must fail, and valid replacements must be sent.
    char *changed_url = xstrfmt("%s/changed", endpoint.data); set_text(s.edits[F_URL], changed_url); free(changed_url);
    body = body_now(&s, &why); CHECK(body == NULL); CHECK(why != NULL); free(why); why = NULL;
    set_text(s.edits[F_URL], "https://new.example/mcp");
    body = body_now(&s, &why); CHECK(body != NULL); CHECK_STR(json_str(json_get(body, "url")), "https://new.example/mcp"); json_free(body);
    // Even unchanged text needs validation on create or when switching from stored stdio to HTTP.
    set_text(s.edits[F_URL], endpoint.data); s.id = 0;
    body = body_now(&s, &why); CHECK(body == NULL); CHECK(why != NULL); free(why); why = NULL;
    s.id = 123; json_set_str(s.row, "transport", "stdio");
    body = body_now(&s, &why); CHECK(body == NULL); CHECK(why != NULL); free(why); why = NULL;
    // A newer remote endpoint must not cause a preserved dirty draft to be silently omitted.
    Json *next = json_clone(s.row); json_set_str(next, "transport", "http"); json_set_str(next, "url", "https://remote.example");
    status_update(&s, next); CHECK(s.conflict);
    set_text(s.edits[F_URL], "https://draft.example");
    body = body_now(&s, &why); CHECK(body != NULL); CHECK_STR(json_str(json_get(body, "url")), "https://draft.example"); json_free(body);
    json_free(next); str_free(&endpoint); form_cleanup(&s);
}
static void test_uncertain_create_reconciliation(void) {
    for (int committed = 0; committed < 2; committed++) {
        McpForm s; form_fixture(&s, false); s.dirty = true;
        set_text(s.edits[F_CLIENT_SECRET], "private draft"); s.secrets[2] = MCP_REPLACE;
        form_save(&s); CHECK_INT(calls, 1); CHECK_STR(s.create_name, "tools"); s.write = NULL;
        Request req = {0}; req.client = s.account; req.tag = ACT_SAVE; api_error_init(&req.error);
        write_done(&s, &req); CHECK(s.uncertain); CHECK_INT(calls, 2); CHECK(s.read != NULL); CHECK_INT(s.id, 0);
        form_save(&s); CHECK_INT(calls, 2);
        // Recovery matches the submitted name even if the draft name is edited meanwhile.
        set_text(s.edits[F_NAME], "edited-name");
        req.ok = true; req.result = json_parsez(committed ? "{\"servers\":[{\"id\":456,\"name\":\"tools\",\"url\":\"https://old.example\"}]}" : "{\"servers\":[]}");
        s.read = NULL; read_done(&s, &req); CHECK(s.uncertain == !committed); CHECK(s.create_retry == !committed); CHECK(s.dirty); CHECK_INT(s.id, committed ? 456 : 0);
        CHECK_OWNED_STR(edit_text(s.edits[F_NAME]), "edited-name"); CHECK_OWNED_STR(edit_text(s.edits[F_CLIENT_SECRET]), "private draft");
        CHECK_INT(s.secrets[2], MCP_REPLACE); CHECK(s.error != NULL);
        if (!committed) {
            // An empty GET can precede the original commit. Every explicit POST keeps its original unique name.
            HeaderInfo header = {0}; form_header(&s.base, &header); CHECK(header.buttons[0].enabled);
            read_done(&s, &req); CHECK(s.uncertain); CHECK(s.create_retry); CHECK_INT(calls, 2);
            form_save(&s); CHECK_INT(calls, 3); CHECK_STR(s.create_name, "tools"); CHECK_STR(json_str(json_get(sent, "name")), "tools");
            CHECK(!s.create_retry); s.write = NULL;
            json_free(req.result); req.result = json_object();
            Json *created = json_clone(s.row); json_set_num(created, "id", 456); json_object_set(req.result, "server", created);
            write_done(&s, &req); CHECK_INT(s.id, 456); CHECK(!s.uncertain); CHECK(s.dirty); CHECK(!s.conflict);
            CHECK_OWNED_STR(edit_text(s.edits[F_NAME]), "edited-name");
            form_save(&s); CHECK_INT(calls, 4); CHECK_INT(json_int_or(json_get(sent, "id"), 0), 456);
            CHECK_STR(json_str(json_get(sent, "name")), "edited-name"); s.write = NULL;
        }
        json_free(req.result); api_error_clear(&req.error); form_cleanup(&s);
    }
    McpForm s; form_fixture(&s, false); s.uncertain = s.dirty = true; s.create_name = xstrdup("tools");
    Request req = {0}; req.client = s.account; req.ok = true; req.result = json_object(); api_error_init(&req.error);
    read_done(&s, &req); CHECK(s.uncertain); CHECK(s.dirty); // A malformed registry must not authorize a retry.
    req.ok = false; read_done(&s, &req); CHECK(s.uncertain);
    form_refresh(&s.base); CHECK(s.read != NULL); CHECK_INT(calls, 1);
    json_free(req.result); api_error_clear(&req.error); form_cleanup(&s);
    // A refused retry does not prove that the original timed-out create failed.
    form_fixture(&s, false); s.uncertain = s.create_retry = s.dirty = true; s.create_name = xstrdup("tools");
    set_text(s.edits[F_NAME], "renamed-draft"); form_save(&s); CHECK_STR(json_str(json_get(sent, "name")), "tools");
    s.write = NULL; memset(&req, 0, sizeof req); req.client = s.account; req.tag = ACT_SAVE; api_error_init(&req.error);
    api_error_set(&req.error, API_HTTP, 400, "Name already exists", -1);
    write_done(&s, &req); CHECK(s.uncertain); CHECK(!s.create_retry); CHECK(s.read != NULL); CHECK_STR(s.create_name, "tools");
    req.ok = true; req.result = json_parsez("{\"servers\":[{\"id\":456,\"name\":\"tools\"}]}");
    s.read = NULL; read_done(&s, &req); CHECK_INT(s.id, 456); CHECK(!s.uncertain);
    CHECK_OWNED_STR(edit_text(s.edits[F_NAME]), "renamed-draft");
    json_free(req.result); api_error_clear(&req.error); form_cleanup(&s);
}
static void test_nul_authoring(void) {
    McpForm s; form_fixture(&s, false); char *why = NULL;
    for (int f = F_ARGS; f <= F_ENV; f++) {
        s.stdio = f != F_HEADERS; s.secrets[0] = s.secrets[1] = MCP_REPLACE;
        set_text(s.edits[F_COMMAND], "mcp");
        set_text(s.edits[f], f == F_ARGS ? "[\"a\\u0000b\"]" : "{\"TOKEN\":\"a\\u0000b\"}");
        Json *body = body_now(&s, &why); CHECK(body == NULL); CHECK(why && strstr(why, "NUL")); free(why); why = NULL;
        form_save(&s); CHECK_INT(calls, 0);
        // Literal backslash-u text must survive exactly, including inside replacement secrets.
        set_text(s.edits[f], f == F_ARGS ? "[\"a\\\\u0000b\"]" : "{\"TOKEN\":\"a\\\\u0000b\"}");
        body = body_now(&s, &why); CHECK(body != NULL); CHECK(why == NULL);
        const Json *field = json_get(body, FIELDS[f].key);
        CHECK_STR(json_str(f == F_ARGS ? json_at(field, 0) : json_get(field, "TOKEN")), "a\\u0000b"); json_free(body);
        set_text(s.edits[f], f == F_ARGS ? "[]" : "{}");
    }
    set_text(s.edits[F_ENV], "{\"TO\\u0000KEN\":\"value\"}");
    Json *body = body_now(&s, &why); CHECK(body == NULL); CHECK(why != NULL); free(why); why = NULL;
    s.stdio = false; // Hidden environment/arguments and kept headers are not authored in this save.
    s.secrets[0] = MCP_KEEP; set_text(s.edits[F_ARGS], "[\"\\u0000\"]"); set_text(s.edits[F_HEADERS], "{\"TOKEN\":\"\\u0000\"}");
    body = body_now(&s, &why); CHECK(body != NULL); CHECK(why == NULL); json_free(body);
    // Server payload parsing keeps its established permissive semantics.
    Json *response = json_parsez("[\"a\\u0000b\"]"); CHECK_STR(json_str(json_at(response, 0)), "a\xef\xbf\xbd" "b"); json_free(response);
    form_cleanup(&s);
}
static void test_departed_mutation_completion(void) {
    for (int deleting = 0; deleting < 2; deleting++) for (int ok = 0; ok < 2; ok++) {
        McpForm s; form_fixture(&s, deleting != 0);
        if (deleting) write_call(&s, "delete_mcp_server", json_object(), ACT_DELETE); else form_save(&s);
        CHECK(s.write != NULL); CHECK_INT(calls, 1);
        form_visible(&s.base, false);
        CHECK(s.write == NULL); CHECK(s.uncertain); CHECK(!pending.cancelled); CHECK(pending.owner == NULL); CHECK(pending.slot == NULL);
        CHECK(pending.done != NULL); CHECK_INT(registry_changes, 0);
        RequestDone done = pending.done; pending.ok = ok != 0;
        form_cleanup(&s); // Completion must be safe after the form and its controls are gone.
        g_store.client = pending.client; g_store.has_device = true; g_store.device.permission = "admin";
        g_store.routes = routes; g_store.route_count = sizeof routes / sizeof *routes;
        done(pending.owner, &pending); CHECK_INT(registry_changes, 1); CHECK_INT(detail_clears, 0);
        g_store.client = NULL; done(pending.owner, &pending); CHECK_INT(registry_changes, 1);
        g_store.client = pending.client; g_store.device.permission = "read"; done(pending.owner, &pending); CHECK_INT(registry_changes, 1);
        memset(&g_store, 0, sizeof g_store);
    }
}
static void test_polled_status_refreshes_settings(void) {
    McpForm s; form_fixture(&s, true);
    Json *row = json_clone(s.row); json_set_str(row, "status", "needs-sign-in"); status_update(&s, row);
    Request req = {0}; req.client = s.account; req.ok = true; req.result = json_object(); api_error_init(&req.error);
    Json *rows = json_array(); json_array_push(rows, row); json_object_set(req.result, "servers", rows);
    read_done(&s, &req); CHECK_INT(registry_changes, 0); CHECK_INT(calls, 0);
    json_set_str(row, "status", "ready"); json_set_bool(row, "signedIn", true);
    read_done(&s, &req); CHECK_INT(registry_changes, 1); CHECK_INT(calls, 0);
    CHECK_STR(json_str(json_get(s.row, "status")), "ready"); CHECK(json_bool_is(json_get(s.row, "signedIn"), true));
    read_done(&s, &req); CHECK_INT(registry_changes, 1); CHECK_INT(calls, 0);
    json_set_str(row, "status", "error"); req.client = NULL; read_done(&s, &req); CHECK_INT(registry_changes, 1);
    json_free(req.result); api_error_clear(&req.error); form_cleanup(&s);
}
static void test_polled_deletion_refreshes_settings(void) {
    McpForm s; form_fixture(&s, true);
    Request req = {0}; req.client = s.account; req.ok = true; req.result = json_parsez("{\"servers\":[]}"); api_error_init(&req.error);
    req.client = NULL; read_done(&s, &req); CHECK_INT(registry_changes, 0); CHECK(!s.uncertain);
    req.client = s.account; req.ok = false; read_done(&s, &req); CHECK_INT(registry_changes, 0); CHECK(!s.uncertain);
    req.ok = true; g_store.device.permission = "read"; read_done(&s, &req); CHECK_INT(registry_changes, 0);
    g_store.device.permission = "admin"; read_done(&s, &req); CHECK_INT(registry_changes, 1); CHECK(s.uncertain); CHECK(s.error != NULL);
    json_free(req.result); api_error_clear(&req.error); form_cleanup(&s);
}
void app_mcp_settings_tests(void) {
    test_run("MCP form omits retained assignments and allows disabled empty selections", test_assignments_and_disabled_empty_selection);
    test_run("MCP form refreshes clean controls and preserves drafts behind conflict confirmation", test_clean_refresh_and_dirty_conflict);
    test_run("MCP clean saves preserve pending OAuth while new and dirty forms can save", test_clean_save_preserves_pending_sign_in);
    test_run("MCP updates omit unchanged normalized endpoints but validate creates and replacements", test_normalized_endpoint_partial_updates);
    test_run("MCP uncertain creates reconcile by submitted name without losing drafts or retrying writes", test_uncertain_create_reconciliation);
    test_run("MCP authored JSON rejects NUL without changing literal escapes or response parsing", test_nul_authoring);
    test_run("MCP departed mutations refresh only their current account after completion", test_departed_mutation_completion);
    test_run("MCP polled OAuth status changes refresh Settings without extra reads on unchanged polls", test_polled_status_refreshes_settings);
    test_run("MCP authoritative deletion refreshes Settings only for the current admin account", test_polled_deletion_refreshes_settings);
}
