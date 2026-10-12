#include "screens.h"
#include "suites.h"
#include "test.h"
#include <stdlib.h>
#include <string.h>

static Request pending[8];
static int call_count;
static ULONGLONG quota_ticks;
static ULONGLONG test_ticks(void) { return quota_ticks; }
static void changed(Pane *pane) { (void)pane; }
static Request *test_call(const char *operation, Json *args, int timeout_ms, void *owner, RequestDone done, int tag, Request **slot) {
    (void)timeout_ms;
    Request *r = &pending[call_count++];
    r->operation = xstrdup(operation); r->args = args; r->owner = owner; r->done = done;
    r->tag = tag; r->slot = slot; r->client = g_store.client; *slot = r;
    return r;
}
static void test_cancel(Request **slot) {
    if (*slot) { (*slot)->cancelled = true; (*slot)->slot = NULL; *slot = NULL; }
}
Screen *test_dashboard_new(void);
#define dashboard_screen_new test_dashboard_new
#define store_call test_call
#define request_cancel test_cancel
#define pane_relayout changed
#define pane_header_changed changed
#define GetTickCount64 test_ticks
#include "../app/screen_dashboard.c"
#undef GetTickCount64
#undef dashboard_screen_new
#undef store_call
#undef request_cancel
#undef pane_relayout
#undef pane_header_changed

static Route routes[] = {
    { "GET", "/usage/all", "admin" },
    { "GET", "/settings/providers", "admin" },
    { "GET", "/settings/providers/{id}/status", "admin" },
};
static void fixture(DashboardScreen *s) {
    memset(s, 0, sizeof *s); memset(&g_store, 0, sizeof g_store);
    memset(pending, 0, sizeof pending); call_count = 0; quota_ticks = 1000;
    g_store.client = (ApiClient *)pending; g_store.has_device = true; g_store.device.permission = "admin";
    g_store.routes = routes; g_store.route_count = sizeof routes / sizeof *routes;
}
static void cleanup(DashboardScreen *s) {
    quotas_cancel(s);
    json_free(s->accounts); free(s->status_reqs); free(s->accounts_error);
    json_free(s->data); free(s->data_key);
    for (int f = 0; f < F_COUNT; f++) picks_clear(s, (FilterId)f);
    for (int i = 0; i < call_count; i++) { json_free(pending[i].args); free(pending[i].operation); }
    memset(&g_store, 0, sizeof g_store);
}
static void reply_retry(int index, const char *body, bool ok, double retry_after) {
    Request *r = &pending[index]; r->ok = ok; r->result = json_parsez(body);
    if (r->slot) { *r->slot = NULL; r->slot = NULL; }
    api_error_init(&r->error);
    if (!ok) api_error_set(&r->error, retry_after >= 0 ? API_HTTP : API_NETWORK,
                           retry_after >= 0 ? 429 : 0, "Quota request failed", retry_after);
    r->done(r->owner, r); json_free(r->result); r->result = NULL; api_error_clear(&r->error);
}
static void reply(int index, const char *body, bool ok) { reply_retry(index, body, ok, -1); }
static bool contains(const Doc *doc, const char *text) {
    for (size_t i = 0; i < doc->count; i++) if (doc->items[i].text && strstr(doc->items[i].text, text)) return true;
    return false;
}
static void test_remaining_values(void) {
    static const struct { const char *json, *expected; } cases[] = {
        { "{\"usedPct\":25}", "75% remaining" },
        { "{\"usedPct\":100}", "0% remaining" },
        { "{\"usedPct\":0}", "100% remaining" },
        { "{\"usedPct\":120}", "0% remaining" },
        { "{\"usedPct\":-10}", "100% remaining" },
        { "{\"usedPct\":null}", "Usage unavailable" },
        { "{\"usedPct\":\"25\"}", "Usage unavailable" },
        { "{}", "Usage unavailable" },
    };
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        Json *window = json_parsez(cases[i].json);
        CHECK_OWNED_STR(quota_text(window), cases[i].expected); json_free(window);
    }
    Json *window = json_object(); json_set_num(window, "usedPct", NAN);
    CHECK_OWNED_STR(quota_text(window), "Usage unavailable"); json_free(window);
}
static void test_account_loading_and_layout(void) {
    DashboardScreen s; fixture(&s);
    quotas_load(&s, false); quotas_load(&s, false); CHECK_INT(call_count, 1);
    CHECK_STR(pending[0].operation, "settings_providers");
    reply(0, "{\"providers\":[{\"id\":7,\"label\":\"Work\",\"binary\":\"codex\"},{\"id\":8,\"label\":\"Personal\",\"binary\":\"claude\",\"active\":false}]}", true);
    CHECK_INT(call_count, 3); CHECK_STR(pending[1].operation, "provider_status");
    CHECK_INT(json_int_or(json_get(pending[1].args, "id"), 0), 7);
    CHECK_INT(json_int_or(json_get(pending[2].args, "id"), 0), 8);
    CHECK(json_is_null(json_get(pending[1].args, "fresh")));
    reply(2, "{}", false);
    reply(1, "{\"status\":{\"auth\":{\"email\":\"work@example.com\",\"plan\":\"pro\"},\"usage\":{\"windows\":[{\"label\":\"5 hours\",\"usedPct\":25,\"resetsAt\":\"2030-10-12T19:00:00Z\"},{\"label\":\"7 days\",\"usedPct\":100}]}}}", true);
    s.data = json_parsez("{\"turns\":0,\"period\":\"month\",\"month\":\"2026-10\"}"); s.data_key = query_key(&s);
    for (int width = 360; width <= 1600; width += 620) {
        Doc doc; doc_init(&doc); doc_begin(&doc, NULL, width); dashboard_layout(&s.base, &doc);
        CHECK(contains(&doc, "Usage remaining per account")); CHECK(contains(&doc, "work@example.com"));
        CHECK(contains(&doc, "5 hours: 75% remaining")); CHECK(contains(&doc, "resets "));
        CHECK(contains(&doc, "7 days: 0% remaining")); CHECK(contains(&doc, "Personal"));
        CHECK(contains(&doc, "disabled")); CHECK(contains(&doc, "Quota request failed"));
        CHECK(contains(&doc, "No usage recorded")); doc_free(&doc);
    }
    quotas_load(&s, true); CHECK_INT(call_count, 4);
    reply(3, "{\"providers\":[{\"id\":9,\"label\":\"Replacement\"}]}", true);
    CHECK_INT(call_count, 5); CHECK_INT(json_int_or(json_get(pending[4].args, "fresh"), 0), 1);
    CHECK_INT(json_count(s.accounts), 1);
    dashboard_visible(&s.base, false); CHECK(pending[4].cancelled); CHECK(!quotas_busy(&s));
    cleanup(&s);
}
static void test_unavailable_and_permissions(void) {
    DashboardScreen s; fixture(&s);
    g_store.device.permission = "read"; quotas_load(&s, false); CHECK_INT(call_count, 0);
    g_store.device.permission = "admin"; g_store.route_count = 1; quotas_load(&s, false); CHECK_INT(call_count, 0);
    g_store.route_count = sizeof routes / sizeof *routes;
    quotas_load(&s, false); reply(0, "{}", true); CHECK(s.accounts_error != NULL); CHECK(s.accounts == NULL);
    quotas_load(&s, false); reply(1, "{\"providers\":[{\"id\":1}]}", true);
    CHECK(s.accounts_error == NULL); reply(2, "{\"status\":{\"usage\":null}}", true);
    Doc doc; doc_init(&doc); doc_begin(&doc, NULL, 600); quotas_layout(&s, &doc, 0, 600);
    CHECK(contains(&doc, "Usage unavailable for this account")); CHECK(!contains(&doc, "100% remaining")); doc_free(&doc);
    quotas_load(&s, false); reply(3, "{\"providers\":[]}", true);
    doc_init(&doc); doc_begin(&doc, NULL, 600); quotas_layout(&s, &doc, 0, 600);
    CHECK(contains(&doc, "No provider accounts configured")); doc_free(&doc);
    cleanup(&s);
}
static void test_quota_retry_after(void) {
    for (int status = 0; status < 2; status++) {
        DashboardScreen s; fixture(&s);
        quotas_load(&s, false);
        if (status) {
            reply(0, "{\"providers\":[{\"id\":1},{\"id\":2},{\"id\":3}]}", true);
            reply_retry(1, "{}", false, 300);
            reply_retry(2, "{}", false, 30);
            reply(3, "{\"status\":{}}", true);
        } else reply_retry(0, "{}", false, 300);
        int quota_calls = call_count;
        pick_add(&s, F_MODEL, "test-model");
        load(&s); reply(quota_calls, "{}", true);
        quota_ticks += 60000;
        quotas_load(&s, false); CHECK_INT(call_count, quota_calls + 1);
        quota_ticks = 300999;
        quotas_load(&s, false); CHECK_INT(call_count, quota_calls + 1);
        quota_ticks = 301000;
        quotas_load(&s, false); CHECK_INT(call_count, quota_calls + 2);
        CHECK_STR(pending[quota_calls + 1].operation, "settings_providers");
        cleanup(&s);
    }
}
static void test_fresh_refresh_during_quota_reads(void) {
    for (int status = 0; status < 2; status++) {
        DashboardScreen s; fixture(&s);
        quotas_load(&s, false);
        if (status) reply(0, "{\"providers\":[{\"id\":1},{\"id\":2}]}", true);
        int old_calls = call_count;
        dashboard_refresh(&s.base);
        CHECK_INT(call_count, old_calls + 2);
        for (int i = status ? 1 : 0; i < old_calls; i++) {
            CHECK(pending[i].cancelled); CHECK(pending[i].slot == NULL);
        }
        CHECK_STR(pending[old_calls + 1].operation, "settings_providers");
        reply(old_calls + 1, "{\"providers\":[{\"id\":3}]}", true);
        CHECK_INT(call_count, old_calls + 3);
        CHECK_STR(pending[old_calls + 2].operation, "provider_status");
        CHECK_INT(json_int_or(json_get(pending[old_calls + 2].args, "fresh"), 0), 1);
        CHECK_INT(json_int_or(json_get(pending[old_calls + 2].args, "id"), 0), 3);
        cleanup(&s);
    }
}
void app_dashboard_tests(void) {
    if (!theme.canvas) theme_init();
    test_run("Dashboard quota percentages distinguish missing data from full allowance", test_remaining_values);
    test_run("Dashboard reads each account and renders quota windows without ledger activity", test_account_loading_and_layout);
    test_run("Dashboard quota reads respect permissions and show unavailable or empty accounts", test_unavailable_and_permissions);
    test_run("Dashboard quota cooldown survives ledger success and other account responses", test_quota_retry_after);
    test_run("Dashboard fresh refresh restarts pending account and status reads", test_fresh_refresh_during_quota_reads);
}
