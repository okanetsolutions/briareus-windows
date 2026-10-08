// Final-core gates automatically exercise the remaining inbox/SSE calls when their feature PRs land.
// An unregistered client call stays unavailable even if the server already advertises it.
#include "api.h"
#include "json.h"
#include "models.h"
#include "store.h"
#include "str.h"
#include "suites.h"
#include "test.h"
#include "fixtures/october-2026/catalog.h"
#include <stdlib.h>
#include <string.h>

static void test_incoming_calls_match_final_catalog_when_registered(void) {
    const struct { const char *call, *method, *path; } calls[] = {
        { "mail_messages", "GET", "mail/messages" },
        { "mail_message", "GET", "mail/accounts/{account}/messages/{id}" },
        { "slack_workspaces", "GET", "slack/workspaces" },
        { "slack_conversations", "GET", "slack/workspaces/{id}/conversations" },
        { "slack_conversation", "GET", "slack/workspaces/{id}/conversations/{channel}" },
        { "slack_people", "GET", "slack/workspaces/{id}/people" },
        { "slack_open_dm", "POST", "slack/workspaces/{id}/direct-messages" },
        { "slack_history", "GET", "slack/workspaces/{id}/conversations/{channel}/messages" },
        { "slack_send", "POST", "slack/workspaces/{id}/conversations/{channel}/messages" },
        { "slack_thread", "GET", "slack/workspaces/{id}/conversations/{channel}/threads/{ts}" },
        { "slack_read", "POST", "slack/workspaces/{id}/conversations/{channel}/read" },
        { "slack_events", "GET", "slack/workspaces/{id}/events" },
    };
    Str fixture; str_init(&fixture);
    for (size_t i = 0; i < sizeof OCTOBER_CATALOG / sizeof *OCTOBER_CATALOG; i++) str_appendz(&fixture, OCTOBER_CATALOG[i]);
    Json *j = json_parsez(fixture.data); str_free(&fixture);
    Route *routes = NULL; size_t n = 0; CHECK(routes_parse(j, &routes, &n)); json_free(j);
    const char *permissions[] = { "read", "manage", "admin" };
    memset(&g_store, 0, sizeof g_store);
    for (size_t i = 0; i < sizeof calls / sizeof *calls; i++) {
        const ApiRoute *operation = api_route(calls[i].call);
        if (operation) { CHECK_STR(operation->method, calls[i].method); CHECK_STR(operation->path, calls[i].path); }
        // These assertions apply even before Windows registers the operation.
        CHECK(routes_allow(routes, n, calls[i].method, calls[i].path, "admin"));
        CHECK(!routes_allow(routes, n, calls[i].method, calls[i].path, "manage"));
        Route *without = xmalloc(n * sizeof *without); size_t count = 0;
        for (size_t k = 0; k < n; k++) {
            if (!str_eq(routes[k].method, calls[i].method) || !routes_allow(&routes[k], 1, calls[i].method, calls[i].path, "admin"))
                without[count++] = routes[k];
        }
        for (size_t p = 0; p < 3; p++) {
            g_store.has_device = true; g_store.device.permission = (char *)permissions[p];
            g_store.routes = routes; g_store.route_count = n;
            CHECK(store_supports(calls[i].call) == (operation != NULL && p == 2));
            g_store.routes = without; g_store.route_count = count; CHECK(!store_supports(calls[i].call));
            g_store.routes = NULL; g_store.route_count = 0; CHECK(!store_supports(calls[i].call));
        }
        free(without);
    }
    memset(&g_store, 0, sizeof g_store); routes_free(routes, n);
}

void app_contract_tests(void) {
    test_run("incoming mail and Slack calls match final core gates when registered", test_incoming_calls_match_final_catalog_when_registered);
}
