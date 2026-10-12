#include "screens.h"
#include "suites.h"
#include "test.h"
#include <stdlib.h>
#include <string.h>

static Request pending[6];
static int calls[6];
static void test_changed(Pane *pane) { (void)pane; }
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
#define pane_relayout test_changed
#define pane_header_changed test_changed
#define store_call test_call
#define request_cancel test_cancel
#include "../app/screen_home_assistant.c"
#undef home_assistant_offered
#undef home_assistant_screen_new
#undef pane_relayout
#undef pane_header_changed
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
    g_store.has_device = true; g_store.device.permission = "admin";
    g_store.routes = routes; g_store.route_count = sizeof routes / sizeof *routes;
    g_store.client = (ApiClient *)pending;
    s->account = g_store.client; s->shown = true; s->data = xstrdup("{}");
}
static void cleanup(HomeScreen *s) {
    for (int i = 0; i < 6; i++) json_free(pending[i].args);
    json_free(s->status); json_free(s->entities); json_free(s->services); json_free(s->entity); json_free(s->service);
    free(s->filter); free(s->entity_id); free(s->domain); free(s->service_name); free(s->event_type);
    free(s->data); free(s->error); free(s->receipt);
    memset(&g_store, 0, sizeof g_store);
}
static void reply(int tag, const char *body, bool ok) {
    Request *r = &pending[tag]; r->ok = ok; r->result = json_parsez(body);
    if (r->slot) { *r->slot = NULL; r->slot = NULL; }
    api_error_init(&r->error);
    if (!ok) api_error_set(&r->error, API_NETWORK, 0, "Timeout", -1);
    r->done(r->owner, r); json_free(r->result); r->result = NULL; api_error_clear(&r->error);
}
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
    CHECK_STR(s.data, "{\"entity_id\":\"weather.home\"}");
    action(&s.base, ACT_SEND, 0, (POINT){0});
    CHECK_INT(calls[WRITE_ACTION], 1); CHECK_INT(json_int_or(json_get(pending[WRITE_ACTION].args, "returnResponse"), -1), 1);
    CHECK_STR(json_str(json_get(json_get(pending[WRITE_ACTION].args, "data"), "entity_id")), "weather.home");
    action(&s.base, ACT_SEND, 0, (POINT){0}); CHECK_INT(calls[WRITE_ACTION], 1);
    reply(WRITE_ACTION, "{}", false);
    CHECK(s.uncertain); CHECK_INT(calls[WRITE_ACTION], 1); CHECK_INT(calls[READ_ENTITIES], 1); CHECK_INT(calls[READ_ENTITY], 1);
    action(&s.base, ACT_SEND, 0, (POINT){0}); CHECK_INT(calls[WRITE_ACTION], 1);
    reply(READ_ENTITIES, "{\"entities\":[]}", true); CHECK(!s.uncertain);
    action(&s.base, ACT_SEND, 0, (POINT){0}); reply(WRITE_ACTION, "{\"ok\":true}", true);
    CHECK(s.uncertain); CHECK(strstr(s.receipt, "Invalid action receipt") != NULL);
    reply(READ_ENTITIES, "{\"entities\":[]}", true);
    action(&s.base, ACT_SEND, 0, (POINT){0}); reply(WRITE_ACTION, "{\"entities\":[],\"response\":{\"forecast\":[]}}", true);
    CHECK(!s.uncertain); CHECK(strstr(s.receipt, "forecast") != NULL);
    cleanup(&s);
}
static void test_events_and_account_changes(void) {
    HomeScreen s; fixture(&s); s.connected = true; s.tab = 2; s.event_type = xstrdup("custom_event");
    set_string(&s.data, "{\"type\":\"user-data\"}");
    action(&s.base, ACT_SEND, 0, (POINT){0}); CHECK_INT(calls[WRITE_EVENT], 1);
    CHECK_STR(json_str(json_get(pending[WRITE_EVENT].args, "type")), "custom_event");
    CHECK_STR(json_str(json_get(json_get(pending[WRITE_EVENT].args, "data"), "type")), "user-data");
    reply(WRITE_EVENT, "{\"ok\":true}", true); CHECK(strstr(s.receipt, "does not confirm automation completion") != NULL);
    action(&s.base, ACT_SEND, 0, (POINT){0}); reply(WRITE_EVENT, "{\"ok\":false}", true); CHECK(s.uncertain);
    g_store.client = NULL;
    reply(READ_ENTITIES, "{\"entities\":[{\"id\":\"other.account\"}]}", true);
    CHECK(s.entities == NULL); action(&s.base, ACT_SEND, 0, (POINT){0}); CHECK_INT(calls[WRITE_EVENT], 2);
    cleanup(&s);
}
void app_home_assistant_tests(void) {
    test_run("Smart home gates admin/catalog/account access and reads only while visible", test_access_and_visible_reads);
    test_run("Smart home action discovery, response data and uncertain outcomes", test_actions_and_uncertain_outcomes);
    test_run("Smart home events preserve data and reject stale account replies", test_events_and_account_changes);
}
