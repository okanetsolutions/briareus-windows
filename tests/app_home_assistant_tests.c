#include "screens.h"
#include "suites.h"
#include "test.h"
#include <stdlib.h>
#include <string.h>

static Request pending[6];
static int calls[6];
static const char *draft_input;
static Screen *retire_in_dialog;
static int releases, repaints;
static ULONGLONG now;
static ULONGLONG test_ticks(void) { return now; }
static void test_release(Screen *screen) { releases++; screen_release(screen); }
static ApiClient *test_retain(ApiClient *client) { return client; }
static void test_client_release(ApiClient *client) { (void)client; }
static char *test_dialog_text(HWND owner, const char *caption, const char *label, const char *ok, const char *value) {
    (void)owner; (void)caption; (void)label; (void)ok; (void)value;
    if (retire_in_dialog) {
        retire_in_dialog->vt->destroy(retire_in_dialog);
        CHECK_INT(releases, 0);
        retire_in_dialog = NULL;
        g_store.client = NULL;
    }
    return xstrdup(draft_input);
}
static HWND test_pane_hwnd(Pane *pane) { (void)pane; return NULL; }
static void test_changed(Pane *pane) { (void)pane; repaints++; }
static Request *test_call(const char *operation, Json *args, int timeout_ms, void *owner, RequestDone cb, int tag, Request **slot) {
    (void)operation; (void)timeout_ms;
    Request *r = &pending[tag]; json_free(r->args); memset(r, 0, sizeof *r);
    r->args = args; r->owner = owner; r->done = cb; r->tag = tag; r->client = g_store.client; r->slot = slot;
    *slot = r; calls[tag]++; return r;
}
static void test_cancel(Request **slot) { if (*slot) { (*slot)->slot = NULL; *slot = NULL; } }
bool test_home_offered(void);
Screen *test_home_new(void);
#define home_assistant_offered test_home_offered
#define home_assistant_screen_new test_home_new
#define pane_hwnd test_pane_hwnd
#define pane_relayout test_changed
#define pane_header_changed test_changed
#define dialog_text test_dialog_text
#define store_call test_call
#define request_cancel test_cancel
#define GetTickCount64 test_ticks
#define screen_release test_release
#define api_client_retain test_retain
#define api_client_release test_client_release
#include "../app/screen_home_assistant.c"
#undef GetTickCount64
#undef screen_release
#undef api_client_retain
#undef api_client_release
#undef home_assistant_offered
#undef home_assistant_screen_new
#undef pane_hwnd
#undef pane_relayout
#undef pane_header_changed
#undef dialog_text
#undef store_call
#undef request_cancel

static Route routes[] = {
    { "GET", "/home-assistant", "admin" },
    { "GET", "/home-assistant/entities", "admin" },
    { "GET", "/home-assistant/entities/{id}", "admin" },
    { "GET", "/home-assistant/services", "admin" },
    { "POST", "/home-assistant/services/{domain}/{service}", "admin" },
    { "POST", "/home-assistant/events/{type}", "admin" },
};
static void fixture(HomeScreen *s) {
    memset(&g_store, 0, sizeof g_store); memset(s, 0, sizeof *s);
    memset(pending, 0, sizeof pending); memset(calls, 0, sizeof calls);
    now = 1000; releases = repaints = 0; retire_in_dialog = NULL;
    g_store.has_device = true; g_store.device.permission = "admin";
    g_store.routes = routes; g_store.route_count = sizeof routes / sizeof *routes;
    g_store.client = (ApiClient *)pending;
    s->account = g_store.client; s->shown = true; s->action_data = xstrdup("{}"); s->event_data = xstrdup("{}");
}
static void cleanup(HomeScreen *s) {
    for (int i = 0; i < 6; i++) json_free(pending[i].args);
    json_free(s->status); json_free(s->entities); json_free(s->services); json_free(s->entity); json_free(s->service);
    free(s->filter); free(s->entity_id); free(s->domain); free(s->service_name); free(s->event_type);
    for (int i = 0; i < 4; i++) free(s->read_errors[i]);
    free(s->action_data); free(s->event_data); free(s->error); free(s->receipt);
    memset(&g_store, 0, sizeof g_store);
}
static void reply_after(int tag, const char *body, bool ok, double retry_after) {
    Request *r = &pending[tag]; r->ok = ok; r->result = json_parsez(body);
    if (r->slot) { *r->slot = NULL; r->slot = NULL; }
    api_error_init(&r->error);
    if (!ok) api_error_set(&r->error, API_NETWORK, 0, "Timeout", retry_after);
    r->done(r->owner, r); json_free(r->result); r->result = NULL; api_error_clear(&r->error);
}
static void reply(int tag, const char *body, bool ok) { reply_after(tag, body, ok, -1); }
static void test_access_and_visible_reads(void) {
    HomeScreen s; fixture(&s);
    CHECK(test_home_offered());
    g_store.device.permission = "manage"; CHECK(!test_home_offered()); load(&s); CHECK_INT(calls[READ_STATUS], 0);
    g_store.device.permission = "read"; CHECK(!test_home_offered());
    g_store.device.permission = "admin"; g_store.route_count = 0; CHECK(!test_home_offered());
    g_store.route_count = sizeof routes / sizeof *routes;
    load(&s); reply(READ_STATUS, "{\"configured\":false,\"connected\":false}", true);
    CHECK(!s.connected); CHECK_INT(calls[READ_ENTITIES], 0); CHECK_INT(calls[READ_SERVICES], 0);
    load(&s); reply(READ_STATUS, "{\"configured\":true,\"connected\":true}", true);
    CHECK(s.connected); CHECK_INT(calls[READ_ENTITIES], 1); CHECK_INT(calls[READ_SERVICES], 1);
    visible(&s.base, false); CHECK(s.reads[READ_ENTITIES] == NULL); CHECK(s.reads[READ_SERVICES] == NULL);
    load(&s); CHECK_INT(calls[READ_STATUS], 2);
    s.shown = true; g_store.client = NULL; load(&s); CHECK_INT(calls[READ_STATUS], 2);
    cleanup(&s);
}
static void test_actions_and_uncertain_outcomes(void) {
    HomeScreen s; fixture(&s); s.connected = true; s.tab = 1;
    s.services = json_parsez("[{\"domain\":\"weather\",\"services\":{\"get_forecasts\":{\"response\":{\"optional\":false},\"fields\":{\"type\":{\"required\":true}}}}}]");
    s.entity_id = xstrdup("weather.home");
    action(&s.base, ACT_SERVICE, 0, (POINT){0});
    CHECK(s.response); CHECK_STR(s.domain, "weather"); CHECK_STR(s.service_name, "get_forecasts");
    CHECK_STR(s.action_data, "{\"entity_id\":\"weather.home\"}");
    action(&s.base, ACT_SEND, 0, (POINT){0});
    CHECK_INT(calls[WRITE_ACTION], 1); CHECK_INT(json_int_or(json_get(pending[WRITE_ACTION].args, "returnResponse"), -1), 1);
    CHECK_STR(json_str(json_get(json_get(pending[WRITE_ACTION].args, "data"), "entity_id")), "weather.home");
    action(&s.base, ACT_SEND, 0, (POINT){0}); CHECK_INT(calls[WRITE_ACTION], 1);
    reply(WRITE_ACTION, "{}", false);
    CHECK(g_store.home_uncertain); CHECK_INT(calls[WRITE_ACTION], 1); CHECK_INT(calls[READ_ENTITIES], 1); CHECK_INT(calls[READ_ENTITY], 1);
    action(&s.base, ACT_SEND, 0, (POINT){0}); CHECK_INT(calls[WRITE_ACTION], 1);
    reply(READ_ENTITIES, "{\"entities\":[]}", true); CHECK(!g_store.home_uncertain);
    action(&s.base, ACT_SEND, 0, (POINT){0}); reply(WRITE_ACTION, "{\"ok\":true}", true);
    CHECK(g_store.home_uncertain); CHECK(strstr(s.receipt, "Invalid action receipt") != NULL);
    reply(READ_ENTITIES, "{\"entities\":[]}", true);
    action(&s.base, ACT_SEND, 0, (POINT){0}); reply(WRITE_ACTION, "{\"entities\":[],\"response\":{\"forecast\":[]}}", true);
    CHECK(!g_store.home_uncertain); CHECK(strstr(s.receipt, "forecast") != NULL);
    cleanup(&s);
}
static void test_events_and_account_changes(void) {
    HomeScreen s; fixture(&s); s.connected = true; s.tab = 2; s.event_type = xstrdup("custom_event");
    set_string(&s.event_data, "{\"type\":\"user-data\"}");
    action(&s.base, ACT_SEND, 0, (POINT){0}); CHECK_INT(calls[WRITE_EVENT], 1);
    CHECK_STR(json_str(json_get(pending[WRITE_EVENT].args, "type")), "custom_event");
    CHECK_STR(json_str(json_get(json_get(pending[WRITE_EVENT].args, "data"), "type")), "user-data");
    reply(WRITE_EVENT, "{\"ok\":true}", true); CHECK(strstr(s.receipt, "does not confirm automation completion") != NULL);
    action(&s.base, ACT_SEND, 0, (POINT){0}); reply(WRITE_EVENT, "{\"ok\":false}", true); CHECK(g_store.home_uncertain);
    g_store.client = NULL;
    reply(READ_ENTITIES, "{\"entities\":[{\"id\":\"other.account\"}]}", true);
    CHECK(s.entities == NULL); action(&s.base, ACT_SEND, 0, (POINT){0}); CHECK_INT(calls[WRITE_EVENT], 2);
    cleanup(&s);
}
static void test_separate_drafts_and_pending_navigation(void) {
    HomeScreen s; fixture(&s); s.connected = true; s.tab = 1;
    s.services = json_parsez("[{\"domain\":\"light\",\"services\":{\"turn_on\":{}}}]");
    s.event_type = xstrdup("custom_event");
    action(&s.base, ACT_SERVICE, 0, (POINT){0});
    draft_input = "{\"entity_id\":\"light.kitchen\",\"brightness\":20}";
    action(&s.base, ACT_DATA, 0, (POINT){0});
    action(&s.base, ACT_TAB, 2, (POINT){0});
    draft_input = "{\"entity_id\":\"light.hall\",\"brightness\":100}";
    action(&s.base, ACT_DATA, 0, (POINT){0});
    action(&s.base, ACT_TAB, 1, (POINT){0});
    CHECK(vt.can_leave(&s.base));
    action(&s.base, ACT_SEND, 0, (POINT){0});
    CHECK_STR(json_str(json_get(json_get(pending[WRITE_ACTION].args, "data"), "entity_id")), "light.kitchen");
    CHECK_INT(json_int_or(json_get(json_get(pending[WRITE_ACTION].args, "data"), "brightness"), 0), 20);
    CHECK(!vt.can_leave(&s.base)); CHECK(s.write != NULL);
    action(&s.base, ACT_SEND, 0, (POINT){0}); CHECK_INT(calls[WRITE_ACTION], 1);
    reply(WRITE_ACTION, "{\"entities\":[]}", true);
    CHECK(vt.can_leave(&s.base)); CHECK(strstr(s.receipt, "Action accepted") != NULL);
    action(&s.base, ACT_SERVICE, 0, (POINT){0});
    action(&s.base, ACT_TAB, 2, (POINT){0});
    action(&s.base, ACT_SEND, 0, (POINT){0});
    CHECK_STR(json_str(json_get(json_get(pending[WRITE_EVENT].args, "data"), "entity_id")), "light.hall");
    CHECK_INT(json_int_or(json_get(json_get(pending[WRITE_EVENT].args, "data"), "brightness"), 0), 100);
    CHECK(!vt.can_leave(&s.base));
    reply(WRITE_EVENT, "{\"ok\":true}", true);
    CHECK(vt.can_leave(&s.base)); CHECK(strstr(s.receipt, "Event accepted") != NULL);
    cleanup(&s);
}
static void test_read_errors_and_backoff(void) {
    HomeScreen s; fixture(&s); s.entity_id = xstrdup("light.kitchen");
    load(&s); reply(READ_STATUS, "{\"configured\":true,\"connected\":true}", true);
    reply(READ_ENTITIES, "{\"entities\":[{\"id\":\"light.kitchen\",\"state\":\"off\"}]}", true);
    reply(READ_ENTITY, "{\"entity\":{\"state\":\"off\"}}", true);
    reply(READ_SERVICES, "{}", false);
    for (int i = 1; i <= 3; i++) {
        load(&s); reply(READ_STATUS, "{\"configured\":true,\"connected\":true}", true);
        CHECK_INT(s.poll.failures, i - 1);
        if (i > 1) { CHECK(s.read_errors[READ_ENTITIES] != NULL); CHECK(s.read_errors[READ_ENTITY] != NULL); }
        reply(READ_ENTITIES, "{}", false); reply(READ_ENTITY, "{}", false); reply(READ_SERVICES, "{}", false);
        CHECK_INT(s.poll.failures, i);
        CHECK_INT(poll_delay_ms(10000, s.poll.failures, s.poll.retry_after), 1000 << i);
    }
    CHECK_STR(json_str(json_get(s.entity, "state")), "off");
    load(&s); reply(READ_STATUS, "{\"configured\":true,\"connected\":true}", true);
    reply(READ_ENTITIES, "{}", true); CHECK(s.read_errors[READ_ENTITIES] != NULL); CHECK_INT(s.poll.failures, 4);
    reply(READ_ENTITY, "{}", true); CHECK(s.read_errors[READ_ENTITY] != NULL);
    load(&s); reply(READ_STATUS, "{\"configured\":true,\"connected\":true}", true);
    reply(READ_ENTITIES, "{\"entities\":[]}", true);
    CHECK(s.read_errors[READ_ENTITIES] == NULL); CHECK_INT(s.poll.failures, 0);
    CHECK(s.read_errors[READ_ENTITY] != NULL); CHECK(s.read_errors[READ_SERVICES] != NULL);
    reply(READ_ENTITY, "{\"entity\":{\"state\":\"on\"}}", true);
    CHECK(s.read_errors[READ_ENTITY] == NULL);
    reply(READ_SERVICES, "{\"services\":[]}", true); CHECK(s.read_errors[READ_SERVICES] == NULL);
    cleanup(&s);
}
static void test_editor_retirement(void) {
    const int editors[] = { ACT_FILTER, ACT_EVENT, ACT_DATA };
    for (int i = 0; i < 3; i++) for (int apply = 0; apply < 2; apply++) {
        HomeScreen *s = xcalloc(1, sizeof *s); fixture(s); s->base.vt = &vt;
        load(s);
        retire_in_dialog = &s->base; draft_input = apply ? "{}" : NULL;
        action(&s->base, editors[i], 0, (POINT){0});
        CHECK_INT(releases, 1); CHECK_INT(repaints, 0);
        CHECK(pending[READ_STATUS].slot == NULL);
        for (int k = 0; k < 6; k++) json_free(pending[k].args);
        memset(&g_store, 0, sizeof g_store);
    }
}
static void test_uncertainty_survives_navigation(void) {
    HomeScreen *s = xcalloc(1, sizeof *s); fixture(s); s->base.vt = &vt;
    s->connected = true; s->tab = 2; s->event_type = xstrdup("custom_event");
    action(&s->base, ACT_SEND, 0, (POINT){0});
    reply(WRITE_EVENT, "{}", false); reply(READ_ENTITIES, "{}", false);
    CHECK(vt.can_leave(&s->base)); destroy(&s->base);
    s = (HomeScreen *)test_home_new(); s->shown = true; s->tab = 2;
    s->event_type = xstrdup("custom_event");
    load(s); reply(READ_STATUS, "{\"configured\":true,\"connected\":true}", true);
    action(&s->base, ACT_SEND, 0, (POINT){0}); CHECK_INT(calls[WRITE_EVENT], 1);
    reply(READ_ENTITIES, "{}", false);
    action(&s->base, ACT_SEND, 0, (POINT){0}); CHECK_INT(calls[WRITE_EVENT], 1);
    entities_load(s); reply(READ_ENTITIES, "{\"entities\":[]}", true);
    action(&s->base, ACT_SEND, 0, (POINT){0}); CHECK_INT(calls[WRITE_EVENT], 2);
    reply(WRITE_EVENT, "{\"ok\":true}", true);
    destroy(&s->base);
    for (int i = 0; i < 6; i++) json_free(pending[i].args);
    memset(&g_store, 0, sizeof g_store);
}
static void test_read_retry_deadlines(void) {
    HomeScreen s; fixture(&s); s.entity_id = xstrdup("light.kitchen");
    load(&s); reply_after(READ_STATUS, "{}", false, 120);
    load(&s); CHECK_INT(calls[READ_STATUS], 1);
    now += 120000;
    load(&s); reply(READ_STATUS, "{\"configured\":true,\"connected\":true}", true);
    // The in-flight status success must not bypass any other read's cooldown.
    load(&s);
    for (int tag = READ_ENTITIES; tag <= READ_ENTITY; tag++) reply_after(tag, "{}", false, 120);
    reply(READ_STATUS, "{\"configured\":true,\"connected\":true}", true);
    for (int tag = READ_ENTITIES; tag <= READ_ENTITY; tag++) CHECK_INT(calls[tag], 1);
    now += 119999;
    action(&s.base, ACT_REFRESH, 0, (POINT){0});
    reply(READ_STATUS, "{\"configured\":true,\"connected\":true}", true);
    for (int tag = READ_ENTITIES; tag <= READ_ENTITY; tag++) CHECK_INT(calls[tag], 1);
    now++;
    load(&s); reply(READ_STATUS, "{\"configured\":true,\"connected\":true}", true);
    for (int tag = READ_ENTITIES; tag <= READ_ENTITY; tag++) CHECK_INT(calls[tag], 2);
    cleanup(&s);
}
void app_home_assistant_tests(void) {
    test_run("Smart home editor teardown defers release and discards apply/cancel", test_editor_retirement);
    test_run("Smart home uncertainty survives reopening until fresh state", test_uncertainty_survives_navigation);
    test_run("Smart home enforces every read retry deadline", test_read_retry_deadlines);
    test_run("Smart home keeps separate drafts and blocks navigation during writes", test_separate_drafts_and_pending_navigation);
    test_run("Smart home read errors and backoff survive status success", test_read_errors_and_backoff);
    test_run("Smart home gates admin/catalog/account access and reads only while visible", test_access_and_visible_reads);
    test_run("Smart home action discovery, response data and uncertain outcomes", test_actions_and_uncertain_outcomes);
    test_run("Smart home events preserve data and reject stale account replies", test_events_and_account_changes);
}
