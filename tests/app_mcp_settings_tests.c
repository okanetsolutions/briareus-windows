// Exercise the real form controls and response handlers with rendering, dialogs and requests isolated.
#include "screens.h"
#include "suites.h"
#include "test.h"
#include <stdlib.h>
#include <string.h>

static void test_pane_changed(Pane *pane) { (void)pane; }
static void test_selected_id(Pane *pane, const char *id) { (void)pane; (void)id; }
static Pane *test_sidebar(void) { return NULL; }
static void test_registry_changed(void) {}
static bool confirm_result;
static int confirmations, calls;
static Json *sent;
static Request pending;
static bool test_confirm(const char *title, const char *message, const char *label, bool destructive) {
    (void)title; (void)message; (void)label; (void)destructive; confirmations++; return confirm_result;
}
static Request *test_call(const char *operation, Json *args, int timeout_ms, void *owner, RequestDone done, int tag, Request **slot) {
    (void)operation; (void)timeout_ms; (void)owner; (void)done; (void)tag;
    calls++; json_free(sent); sent = args; memset(&pending, 0, sizeof pending); if (slot) *slot = &pending; return &pending;
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
#undef app_confirm
#undef store_call

static Route routes[] = {
    { "GET", "/settings/mcp/servers", "admin" },
    { "POST", "/settings/mcp/servers", "admin" },
    { "PUT", "/settings/mcp/servers/{id}", "admin" },
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
    fill(s); calls = confirmations = 0; confirm_result = false;
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
        s.read = NULL; read_done(&s, &req); CHECK(!s.uncertain); CHECK(s.dirty); CHECK_INT(s.id, committed ? 456 : 0);
        CHECK_OWNED_STR(edit_text(s.edits[F_NAME]), "edited-name"); CHECK_OWNED_STR(edit_text(s.edits[F_CLIENT_SECRET]), "private draft");
        CHECK_INT(s.secrets[2], MCP_REPLACE); CHECK(s.error != NULL);
        if (!committed) { form_save(&s); CHECK_INT(calls, 3); CHECK_STR(s.create_name, "edited-name"); s.write = NULL; }
        json_free(req.result); api_error_clear(&req.error); form_cleanup(&s);
    }
    McpForm s; form_fixture(&s, false); s.uncertain = s.dirty = true; s.create_name = xstrdup("tools");
    Request req = {0}; req.client = s.account; req.ok = true; req.result = json_object(); api_error_init(&req.error);
    read_done(&s, &req); CHECK(s.uncertain); CHECK(s.dirty); // A malformed registry must not authorize a retry.
    req.ok = false; read_done(&s, &req); CHECK(s.uncertain);
    form_refresh(&s.base); CHECK(s.read != NULL); CHECK_INT(calls, 1);
    json_free(req.result); api_error_clear(&req.error); form_cleanup(&s);
}
void app_mcp_settings_tests(void) {
    test_run("MCP form omits retained assignments and allows disabled empty selections", test_assignments_and_disabled_empty_selection);
    test_run("MCP form refreshes clean controls and preserves drafts behind conflict confirmation", test_clean_refresh_and_dirty_conflict);
    test_run("MCP uncertain creates reconcile by submitted name without losing drafts or retrying writes", test_uncertain_create_reconciliation);
}
